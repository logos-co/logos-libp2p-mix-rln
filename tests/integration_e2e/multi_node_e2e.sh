#!/usr/bin/env bash
# Runs N libp2p_mix_rln_module instances under N separate logoscore daemons
# and drives a real Sphinx-routed mix message from node 0 to node N-1 through
# the daemon-facing CLI. Delivery Relay propagates RLN coordination between
# the daemons. This is the C++ / logoscore analog of nim-libp2p-mix-rln-ffi's
# `smoketest_5node_ffi.c`.
#
# Required env: LIBP2P_MIX_RLN_LGX_DIR
# Optional env: LOGOSCORE_BIN, LGPM_BIN, N (mix nodes, default 5), CODEC
#
# Multi-daemon isolation: logoscore keeps runtime state under
# `~/.logoscore/`, so each daemon needs its own HOME. We spawn one per
# temporary dir and talk to it by exporting `HOME` for every CLI call.

set -euo pipefail
shopt -s nullglob

[[ -n "${LIBP2P_MIX_RLN_LGX_DIR:-}" ]] || {
    echo "missing env: LIBP2P_MIX_RLN_LGX_DIR" >&2; exit 1;
}
: "${LOGOSCORE_BIN:=logoscore}"
: "${LGPM_BIN:=lgpm}"
: "${N:=5}"
: "${CODEC:=/logosmix/test/echo/1.0.0}"
PAYLOAD="hello mix (through logoscore)"

pick_lgx() {
    local dir="$1" matches=("$1"/*.lgx)
    (( ${#matches[@]} == 1 )) || {
        echo "expected exactly 1 .lgx in $dir, found ${#matches[@]}" >&2; exit 1; }
    printf '%s\n' "${matches[0]}"
}
LGX="$(pick_lgx "$LIBP2P_MIX_RLN_LGX_DIR")"

ROOT="$(mktemp -d)"
declare -a DAEMON_PIDS=()
declare -a DAEMON_HOMES=()
declare -a DAEMON_PORTS=()

cleanup() {
    local rc=$?
    if [[ "$rc" -ne 0 ]]; then
        for i in "${!DAEMON_HOMES[@]}"; do
            local log="${DAEMON_HOMES[$i]}/logs.txt"
            [[ -f "$log" ]] || continue
            echo "----- daemon $i log tail -----" >&2
            tail -n 60 "$log" >&2 || true
        done
    fi
    for i in "${!DAEMON_PIDS[@]}"; do
        HOME="${DAEMON_HOMES[$i]}" "$LOGOSCORE_BIN" stop >/dev/null 2>&1 || true
        kill "${DAEMON_PIDS[$i]}" 2>/dev/null || true
    done
    for pid in "${DAEMON_PIDS[@]}"; do wait "$pid" 2>/dev/null || true; done
    rm -rf "$ROOT"
}
trap cleanup EXIT

# ---------- daemon lifecycle -------------------------------------------

wait_module_ready() {
    local home="$1" module="$2" deadline=$(( SECONDS + 30 ))
    while (( SECONDS < deadline )); do
        if HOME="$home" "$LOGOSCORE_BIN" load-module "$module" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    echo "timed out waiting for $module in $home" >&2
    return 1
}

start_daemon() {
    local idx="$1"
    local home="$ROOT/node$idx"
    mkdir -p "$home/modules"
    HOME="$home" "$LGPM_BIN" --modules-dir "$home/modules" --allow-unsigned \
        install --file "$LGX" >/dev/null
    HOME="$home" "$LOGOSCORE_BIN" -D -m "$home/modules" \
        > "$home/logs.txt" 2>&1 &
    DAEMON_PIDS[$idx]=$!
    DAEMON_HOMES[$idx]="$home"
    wait_module_ready "$home" "libp2p_mix_rln_module" || return 1
}

# ---------- convenience call wrapper -----------------------------------
# Usage: call <daemon_idx> <method> [args...]
call() {
    local idx="$1"; shift
    HOME="${DAEMON_HOMES[$idx]}" "$LOGOSCORE_BIN" call libp2p_mix_rln_module "$@"
}
success() {
    local idx="$1"; shift
    [[ "$(call "$idx" "$@" | jq -r '.result.success')" == "true" ]]
}

fail=0

# =======================================================================
# Setup: spawn N daemons, load module, createNode + start on each.
# =======================================================================

echo "----- spawning $N daemons -----"
for i in $(seq 0 $((N - 1))); do
    start_daemon "$i"
    DAEMON_PORTS[$i]=$(( 49152 + RANDOM % 16384 ))
    cfg=$(jq -nc --arg addr "/ip4/127.0.0.1/tcp/${DAEMON_PORTS[$i]}" \
        '{addrs:[$addr], transport:"tcp"}')
    if ! success "$i" createNode "$cfg"; then
        echo "FAIL: createNode on node $i" >&2; fail=1; break
    fi
    if ! success "$i" start; then
        echo "FAIL: start on node $i" >&2; fail=1; break
    fi
    echo "  node $i up on tcp/${DAEMON_PORTS[$i]}"
done
(( fail == 0 )) || exit 1

# =======================================================================
# Cross-register: fetch each node's peer record, install on every other.
# =======================================================================

echo "----- cross-registering peer records -----"
declare -a REC_JSON=()
for i in $(seq 0 $((N - 1))); do
    REC_JSON[$i]=$(call "$i" getLocalMixPeerRecord | jq -c '.result.value')
    [[ -n "${REC_JSON[$i]}" && "${REC_JSON[$i]}" != "null" ]] || {
        echo "FAIL: empty record for node $i" >&2; exit 1; }
done

for i in $(seq 0 $((N - 1))); do
    for j in $(seq 0 $((N - 1))); do
        [[ "$i" == "$j" ]] && continue
        # Pass the whole record as one JSON string — logoscore CLI marshals
        # scalar strings cleanly but not [tstr] arrays.
        resp=$(call "$i" addMixPeer "${REC_JSON[$j]}")
        if [[ "$(jq -r '.result.success' <<<"$resp")" != "true" ]]; then
            echo "FAIL: addMixPeer node $i <- node $j: $(jq -c '.result' <<<"$resp")"
            fail=1
            break 2
        fi
    done
done
(( fail == 0 )) || exit 1
echo "  ${N}×$((N - 1)) peers cross-registered"

# =======================================================================
# RLN membership: Delivery Relay propagates each registration to the other
# daemons over the connections established by addMixPeer.
# =======================================================================

echo "----- registering RLN memberships -----"
sleep 2
for i in $(seq 0 $((N - 1))); do
    if ! success "$i" registerRlnMembership; then
        echo "FAIL: registerRlnMembership node $i" >&2; fail=1; break
    fi
done
(( fail == 0 )) || exit 1
sleep 2
echo "  memberships registered and synced"

# =======================================================================
# Mount receiver on node N-1; send from node 0 with isExitDest=true; poll
# node N-1's inbox until the payload lands (or timeout).
# =======================================================================

DEST=$((N - 1))
echo "----- mounting receiver on node $DEST -----"
if ! success "$DEST" mountReceiver "$CODEC" 4096; then
    echo "FAIL: mountReceiver on node $DEST" >&2; exit 1
fi

DEST_PEER_ID=$(jq -r '.peerId' <<<"${REC_JSON[$DEST]}")
echo "----- sending mix message from node 0 to node $DEST (exit-is-dest) -----"
# logoscore CLI passes bstr args as literal bytes (no base64 decode), so we
# hand it the raw payload string and compare hex-of-payload on the recv side.
send_resp=$(call 0 sendMixMessageToExit "$DEST_PEER_ID" "$CODEC" "$PAYLOAD")
if [[ "$(jq -r '.result.success' <<<"$send_resp")" != "true" ]]; then
    echo "FAIL: sendMixMessageToExit: $(jq -c '.result' <<<"$send_resp")" >&2
    exit 1
fi
echo "  send accepted"

echo "----- polling node $DEST inbox for delivery -----"
deadline=$(( SECONDS + 20 ))
got_hex=""
inbox=""
while (( SECONDS < deadline )); do
    inbox=$(call "$DEST" drainReceivedMessages | jq -c '.result.value')
    n=$(jq -r 'length' <<<"$inbox")
    if (( n > 0 )); then
        got_hex=$(jq -r '.[0].payloadHex' <<<"$inbox")
        break
    fi
    sleep 0.5
done

if [[ -z "$got_hex" ]]; then
    echo "FAIL: no payload delivered within timeout" >&2; exit 1
fi
want_hex=$(printf '%s' "$PAYLOAD" | xxd -p -c 999999)
got_ascii=$(printf '%s' "$got_hex" | xxd -r -p 2>/dev/null || true)
echo "  inbox: proto=$(jq -r '.[0].proto' <<<"$inbox") payload='$got_ascii'"
if [[ "$got_hex" != "$want_hex" ]]; then
    echo "FAIL: payload mismatch; want_hex='$want_hex' got_hex='$got_hex'" >&2; exit 1
fi

for i in $(seq 0 $((N - 1))); do
    success "$i" stop || echo "WARN: stop node $i" >&2
done

echo "PASS: N=$N mix nodes routed a payload through Sphinx + RLN via logoscore"
