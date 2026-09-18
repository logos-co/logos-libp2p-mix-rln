#!/usr/bin/env bash
# Run under logos-rln-e2e's local target; that harness owns chain provisioning.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
: "${RLN_E2E_ROOT:?Set RLN_E2E_ROOT to the logos-rln-e2e checkout}"
for lib in compat json lgx daemon wallet chain; do
    . "$RLN_E2E_ROOT/harness/lib/$lib.sh"
done
install_lgx "${MIX_LGX:?}" "$E2E_MODULES_DIR"
NODES_ALL="sender m1 m2 m3 exit relay receiver"
trap 'daemon_stop_all' EXIT
export MIX_REGISTRY_ID
MIX_REGISTRY_ID=$(python3 - "$E2E_CONFIG_ACCOUNT" <<'PY'
import sys
alphabet = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
value = 0
for char in sys.argv[1]:
    value = value * 58 + alphabet.index(char)
print("logos:local:" + value.to_bytes(32, "big").hex())
PY
)
export MIX_RLN_ID=6d69782d726c6e2d7370616d2d70726f74656374696f6e2f7631000000000000
export RELAY_RLN_ID=5e269b6a19fce081f5808b13442dcbc3522197638dd38df5a28bc4e55236b977
python3 - "$E2E_RUN_DIR/relay-presets.json" <<'PY'
import json, os, sys
with open(sys.argv[1], "w") as out:
    json.dump({"": {"enabled": True, "manage-backend": False,
        "registry-id": os.environ["MIX_REGISTRY_ID"],
        "rln-identifier": os.environ["RELAY_RLN_ID"],
        "epoch-size-sec": 10, "max-epoch-gap": 3}}, out)
PY
export E2E_DAEMON_ENV="${E2E_DAEMON_ENV:-} LOGOS_RLN_DISABLE_AUTO_UNLOCK=1 LOGOS_DELIVERY_RLN_PRESETS=$E2E_RUN_DIR/relay-presets.json"
for node in $NODES_ALL; do
    daemon_self_paying "$node" "$E2E_RUN_DIR/wallet-$node"
    daemon_start "$node"
    daemon_load_modules "$node" liblogos_lez_rln_module liblogos_rln_module delivery_module
    case "$node" in m1|m2|m3) daemon_load_modules "$node" libp2p_mix_rln_module ;; esac
    wallet_open "$node"
    wallet_sync "$node" >/dev/null
    wallet_fund "$node" >/dev/null
    say "$node: separate wallet funded on the local sequencer"
done
python3 "$HERE/network.py"
