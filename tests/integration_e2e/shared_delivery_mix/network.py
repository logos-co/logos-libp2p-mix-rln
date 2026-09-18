#!/usr/bin/env python3
"""Real Delivery send(Required), standalone intermediates, and shared RLN backend."""
import base64
import concurrent.futures
import json
import os
from pathlib import Path
import queue
import re
import socket
import subprocess
import threading
import time

ROOT = Path(os.environ["E2E_RUN_DIR"])
CLI = os.environ["LOGOSCORE"]
REGISTRY = os.environ["MIX_REGISTRY_ID"]
MIX_SCOPE = os.environ["MIX_RLN_ID"]
RELAY_SCOPE = os.environ["RELAY_RLN_ID"]
MIX = "libp2p_mix_rln_module"
DELIVERY = "delivery_module"
RLN = "liblogos_rln_module"
NODES = ("sender", "m1", "m2", "m3", "exit", "relay", "receiver")
INTERMEDIATES = ("m1", "m2", "m3")
MIX_NODES = ("sender", *INTERMEDIATES, "exit")
META_TOPIC = "/mix/1/metadata/proto"
APP_TOPIC = "/mix-test/1/delivery/proto"
EVENTS = queue.Queue()
WATCHERS = []
RECEIVED = []
METADATA = set()
PUBLISHED = 0


def environment(node):
    return dict(os.environ, LOGOSCORE_CONFIG_DIR=str(ROOT / "nodes" / node / "config"))


def unwrap(value):
    while isinstance(value, str):
        try:
            value = json.loads(value)
        except json.JSONDecodeError:
            return value
    if isinstance(value, dict) and "success" in value:
        if not value["success"]:
            raise RuntimeError(str(value.get("error")))
        return unwrap(value.get("value"))
    if isinstance(value, dict) and value.get("error") is not None:
        raise RuntimeError(str(value["error"]))
    return value


def call(node, module, method, *args):
    # json: carries byte arrays; str: preserves registry IDs and timestamp strings.
    encoded = ["json:" + json.dumps(arg) if isinstance(arg, list) else "str:" + str(arg)
               for arg in args]
    result = subprocess.run([CLI, "--json", "call", module, method, *encoded],
                            env=environment(node), text=True, capture_output=True,
                            timeout=100)
    if result.returncode:
        raise RuntimeError(f"{node} {module}.{method}: {result.stdout} {result.stderr}")
    return unwrap(json.loads(result.stdout)["result"])


def wait_for(predicate, description, timeout=120, pump=None):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if pump:
            pump()
        if predicate():
            return
        time.sleep(0.2)
    raise AssertionError("Timed out: " + description)


def start_backend(node):
    call(node, RLN, "unlock_keystore", "mix-e2e-local-only")
    config = {"epoch_size_sec": 10, "max_epoch_gap": 3,
              "registries": [REGISTRY], "provision": False}
    call(node, RLN, "start", json.dumps(config))
    scopes = [RELAY_SCOPE] + ([MIX_SCOPE] if node in MIX_NODES else [])
    for scope in scopes:
        call(node, RLN, "register_membership", REGISTRY, scope,
             json.dumps([{"key": "rate_limit", "value": "100"}]))
        def active():
            state = call(node, RLN, "get_membership_state", REGISTRY, scope)
            if state.get("state") == "failed":
                raise AssertionError(f"{node}: registration failed: {state}")
            return state.get("state") in ("active", "grace_period")
        wait_for(active, f"{node} membership {scope}", timeout=300)
    print(f"{node}: real RLN memberships active", flush=True)


def watch(node):
    proc = subprocess.Popen([CLI, "--json", "watch", DELIVERY, "--event", "messageReceived"],
                            env=environment(node), text=True, stdout=subprocess.PIPE)
    WATCHERS.append(proc)
    def read():
        for line in proc.stdout:
            try:
                EVENTS.put((node, json.loads(line)))
            except json.JSONDecodeError:
                continue
        EVENTS.put((node, None))
    threading.Thread(target=read, daemon=True).start()


def start_delivery(node, peers, *, service=False):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    config = dict(logLevel="INFO", listenAddress="127.0.0.1", tcpPort=port,
                  nat="extip:127.0.0.1", extMultiAddrs=[f"/ip4/127.0.0.1/tcp/{port}"],
                  extMultiAddrsOnly=True, clusterId=198, numShardsInNetwork=1,
                  relay=service, filter=service, lightpush=service, store=False,
                  peerExchange=False, discv5Discovery=False, rendezvous=False,
                  reliabilityEnabled=True, staticnodes=peers)
    if node in ("sender", "exit"):
        config.update(mix=True, **{"mix-rln-registry-id": REGISTRY,
                                  "mix-rln-identifier-hex": MIX_SCOPE,
                                  "mix-rln-metadata-topic": META_TOPIC})
    if node == "sender":
        config["anonymityLevel"] = "Required"
    call(node, DELIVERY, "createNode", json.dumps(config))
    watch(node)
    call(node, DELIVERY, "start")
    call(node, DELIVERY, "subscribe", META_TOPIC)
    call(node, DELIVERY, "subscribe", APP_TOPIC)
    address = None
    def listening():
        nonlocal address
        text = str(call(node, DELIVERY, "getNodeInfo", "MyMultiaddresses"))
        found = re.search(r"/ip4/127\.0\.0\.1/tcp/\d+/p2p/[A-Za-z0-9]+", text)
        if found:
            address = found.group()
        return address is not None
    wait_for(listening, f"{node} Delivery listener")
    return address


def pump():
    global PUBLISHED
    for node in INTERMEDIATES:
        for frame in call(node, MIX, "drainCoordBacklog"):
            assert frame["contentTopic"] == META_TOPIC
            call(node, DELIVERY, "send", META_TOPIC, list(bytes.fromhex(frame["payloadHex"])))
            PUBLISHED += 1
    while True:
        try:
            node, event = EVENTS.get_nowait()
        except queue.Empty:
            return
        if event is None:
            raise AssertionError(f"{node} event watcher exited")
        if event.get("event") != "messageReceived":
            continue
        data = event["data"]
        topic = data["arg1"]
        encoded = data["arg2"]["_bytes"]
        payload = base64.urlsafe_b64decode(encoded + "=" * (-len(encoded) % 4))
        if topic == META_TOPIC:
            METADATA.add((node, payload))
            if node in INTERMEDIATES:
                call(node, MIX, "deliverCoordFrame", topic, payload.hex())
        elif topic == APP_TOPIC:
            RECEIVED.append((node, payload))


def main():
    with concurrent.futures.ThreadPoolExecutor(max_workers=7) as pool:
        list(pool.map(start_backend, NODES))
    exit_address = start_delivery("exit", [], service=True)
    relay_address = start_delivery("relay", [exit_address], service=True)
    for node in ("sender", *INTERMEDIATES, "receiver"):
        peers = [exit_address] if node == "sender" else [relay_address, exit_address]
        start_delivery(node, peers)
    for node in INTERMEDIATES:
        config = {"addrs": ["/ip4/127.0.0.1/tcp/0"],
                  "mix": {"cover": {"rateFraction": 0.01}},
                  "rln": {"provider": "module", "registryId": REGISTRY,
                          "rlnIdentifierHex": MIX_SCOPE,
                          "proofMetadataContentTopic": META_TOPIC}}
        call(node, MIX, "createNode", json.dumps(config))
        call(node, MIX, "start")
    records = {node: call(node, MIX if node in INTERMEDIATES else DELIVERY,
                          "getLocalMixPeerRecord") for node in MIX_NODES}
    for node in INTERMEDIATES:
        assert not records[node]["exitEnabled"], records[node]
    for node in MIX_NODES:
        module = MIX if node in INTERMEDIATES else DELIVERY
        for peer in MIX_NODES:
            if peer != node:
                call(node, module, "addMixPeer", json.dumps(records[peer]))
    # Let Relay meshes and Filter subscriptions form while continuing coordination.
    ready_at = time.monotonic() + 12
    wait_for(lambda: time.monotonic() >= ready_at, "coordination mesh", pump=pump)
    payload = b"Delivery Required through standalone Mix and shared RLN"
    call("sender", DELIVERY, "send", APP_TOPIC, list(payload))
    wait_for(lambda: ("receiver", payload) in RECEIVED, "mixified message at receiver",
             timeout=180, pump=pump)
    assert PUBLISHED > 0, "No standalone hop validated and published proof metadata"
    wait_for(lambda: all(any(n == node for n, _ in METADATA) for node in MIX_NODES),
             "protected metadata delivery to every Mix participant", pump=pump)
    print("PASS: Delivery Required reached the recipient through standalone Mix", flush=True)
    for node in INTERMEDIATES:
        call(node, MIX, "stop")
    blocked = b"Required must not bypass stopped intermediates"
    call("sender", DELIVERY, "send", APP_TOPIC, list(blocked))
    control = b"Relay and Filter remain available"
    call("exit", DELIVERY, "send", APP_TOPIC, list(control))
    wait_for(lambda: ("receiver", control) in RECEIVED, "ordinary delivery control", pump=pump)
    deadline = time.monotonic() + 30
    while time.monotonic() < deadline:
        pump()
        assert ("receiver", blocked) not in RECEIVED, "Required fell back to a direct path"
        time.sleep(0.2)
    print("PASS: stopped Mix intermediates prevent Required delivery; Relay stays usable", flush=True)


if __name__ == "__main__":
    try:
        main()
    finally:
        for watcher in WATCHERS:
            watcher.terminate()
        for watcher in WATCHERS:
            watcher.wait(timeout=5)
