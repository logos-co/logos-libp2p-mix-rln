#!/usr/bin/env bash
# Runs libp2p_mix_rln_module standalone under logoscore and asserts the
# lifecycle path: createNode → start → getNodeInfo(Version|PeerId|Multiaddrs|
# MixPublicKey) → error handling → stop. Validates the LIDL codegen glue,
# the .lgx bundle, and the logoscore module loader together — none of which
# are exercised by the FFI-level C smoke test.
#
# Required env: LIBP2P_MIX_RLN_LGX_DIR
# Optional env: LOGOSCORE_BIN (default: logoscore), LGPM_BIN (default: lgpm)
#
# Adapted from vacp2p/logos-libp2p-module's standalone_e2e.sh.

set -euo pipefail
shopt -s nullglob

[[ -n "${LIBP2P_MIX_RLN_LGX_DIR:-}" ]] || {
    echo "missing env: LIBP2P_MIX_RLN_LGX_DIR" >&2; exit 1;
}
: "${LOGOSCORE_BIN:=logoscore}"
: "${LGPM_BIN:=lgpm}"

pick_lgx() {
    local dir="$1" matches=("$1"/*.lgx)
    if (( ${#matches[@]} != 1 )); then
        echo "expected exactly 1 .lgx in $dir, found ${#matches[@]}" >&2
        exit 1
    fi
    printf '%s\n' "${matches[0]}"
}
LGX="$(pick_lgx "$LIBP2P_MIX_RLN_LGX_DIR")"

WORK="$(mktemp -d)"
DAEMON_PID=""
PORT=$(( 49152 + RANDOM % 16384 ))

cleanup() {
    local rc=$?
    if [[ "$rc" -ne 0 && -f "$WORK/logs.txt" ]]; then
        echo "----- daemon log tail -----" >&2
        tail -n 200 "$WORK/logs.txt" >&2 || true
    fi
    if [[ -n "$DAEMON_PID" ]]; then
        "$LOGOSCORE_BIN" stop >/dev/null 2>&1 || true
        kill "$DAEMON_PID" 2>/dev/null || true
        wait "$DAEMON_PID" 2>/dev/null || true
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT

dump_daemon_log() {
    echo "----- daemon log tail -----" >&2
    tail -n 80 "$WORK/logs.txt" >&2 2>/dev/null || true
    echo "---------------------------" >&2
}

wait_until() {
    local desc="$1"; shift
    local deadline=$(( SECONDS + 30 ))
    while (( SECONDS < deadline )); do
        if "$@" >/dev/null 2>&1; then return 0; fi
        sleep 0.1
    done
    echo "timed out waiting for: $desc" >&2
    dump_daemon_log
    return 1
}

cd "$WORK"
mkdir -p modules

"$LGPM_BIN" --modules-dir ./modules --allow-unsigned install --file "$LGX"
"$LGPM_BIN" --modules-dir ./modules list

"$LOGOSCORE_BIN" -D -m ./modules > logs.txt 2>&1 &
DAEMON_PID=$!

wait_until "logoscore daemon ready" \
    "$LOGOSCORE_BIN" load-module libp2p_mix_rln_module

# `call` exits 0 even when the module reports failure; the outcome lives in
# the wrapped `.result.{success,value,error}`, not the process exit code.
call()      { "$LOGOSCORE_BIN" call libp2p_mix_rln_module "$@"; }
info()      { call getNodeInfo "$1" | jq -r '.result.value // empty'; }
success()   { [[ "$(call "$@" | jq -r '.result.success')" == "true" ]]; }
rejected()  { [[ "$(call "$@" | jq -r '.result.success')" == "false" ]]; }

fail=0
check() {
    local desc="$1" got="$2" want="$3"
    if [[ "$got" == "$want" ]]; then
        echo "ok: $desc ($got)"
    else
        echo "FAIL: $desc: got [$got] want [$want]" >&2
        fail=1
    fi
}
check_nonempty() {
    local desc="$1" got="$2"
    if [[ -n "$got" ]]; then
        echo "ok: $desc ($got)"
    else
        echo "FAIL: $desc empty" >&2
        fail=1
    fi
}

# The mix-rln module's constructor auto-creates a node from the load-time
# config, but here we haven't set LIBP2P_MIX_RLN_MODULE_CONFIG, so that
# auto-create fails and the ctx is null. Recreate explicitly with a real
# listen port.
echo "----- createNode (bind tcp/$PORT) -----"
config=$(jq -nc --arg addr "/ip4/127.0.0.1/tcp/$PORT" '{addrs:[$addr], transport:"tcp"}')
if success createNode "$config"; then
    echo "ok: createNode accepted"
else
    call createNode "$config"  # print the response for debug
    echo "FAIL: createNode rejected valid config" >&2
    fail=1
fi

echo "----- start -----"
if success start; then
    echo "ok: start succeeded"
else
    call start
    echo "FAIL: start returned non-success" >&2
    fail=1
fi

echo "----- getNodeInfo outputs -----"
check          "getNodeInfo Version"      "$(info Version)" "0.1.0"
check_nonempty "getNodeInfo PeerId"       "$(info PeerId)"
check_nonempty "getNodeInfo Multiaddrs"   "$(info Multiaddrs)"
check_nonempty "getNodeInfo MixPublicKey" "$(info MixPublicKey)"

echo "----- unknown getNodeInfo field is rejected -----"
if rejected getNodeInfo Nonexistent; then
    echo "ok: getNodeInfo Nonexistent rejected"
else
    echo "FAIL: getNodeInfo Nonexistent should fail" >&2
    fail=1
fi

echo "----- invalid createNode config is rejected and logged -----"
if rejected createNode '{not valid json'; then
    echo "ok: createNode malformed JSON rejected"
else
    echo "FAIL: createNode with malformed JSON should fail" >&2
    fail=1
fi

# The node rejected the bad config before tearing down, so it stays up.
check "getNodeInfo Version after rejected reconfigure" "$(info Version)" "0.1.0"

echo "----- stop -----"
if success stop; then
    echo "ok: stop succeeded"
else
    echo "FAIL: stop returned non-success" >&2
    fail=1
fi

if [[ "$fail" -ne 0 ]]; then
    exit 1
fi
echo "PASS"
