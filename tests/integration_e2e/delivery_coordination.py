#!/usr/bin/env python3
"""Live Delivery bridge for multi_node_e2e.sh; no in-process frame fanout."""

import base64
import json
import os
from pathlib import Path
import queue
import re
import socket
import signal
import subprocess
import sys
import threading
import time

MIX = "libp2p_mix_rln_module"
DELIVERY = "delivery_module"
TOPICS = ("/mix/1/membership/proto", "/mix/1/metadata/proto")
root = Path(sys.argv[1])
count = int(sys.argv[2])
cli = os.environ["LOGOSCORE_BIN"]
edge = os.environ.get("DELIVERY_MODE", "relay") == "edge"
events = queue.Queue()
watchers = []
published = {}
received = {}


def env(node):
    return dict(os.environ, HOME=str(root / f"node{node}"))


def call(node, module, method, *args):
    proc = subprocess.run(
        [cli, "call", module, method, *map(str, args)], env=env(node),
        text=True, capture_output=True, timeout=45, check=True,
    )
    result = json.loads(proc.stdout)["result"]
    if not result.get("success"):
        raise RuntimeError(f"node {node}: {module}.{method}: {result}")
    return result.get("value")


def watch(node, proc):
    for line in proc.stdout:
        events.put((node, line))
    events.put((node, None))


def publish(node, topic, frame):
    # The CLI accepts literal bytes. Hex in a JSON envelope safely carries NULs
    # and lets the test identify origin and verify the decoded bytes exactly.
    body = json.dumps(frame, separators=(",", ":"))
    call(node, DELIVERY, "send", topic, body)


def receive():
    while True:
        try:
            node, line = events.get_nowait()
        except queue.Empty:
            return
        if line is None:
            raise RuntimeError(f"Delivery event watcher {node} exited")
        event = json.loads(line)
        if event.get("event") != "messageReceived":
            continue
        data = event["data"]
        topic = data["arg1"]
        # logoscore JSON encodes byte strings as unpadded base64url.
        payload = data["arg2"]["_bytes"]
        frame = json.loads(base64.urlsafe_b64decode(payload + "=" * (-len(payload) % 4)))
        key = (frame["origin"], topic, frame["id"])
        if key not in published or frame != published[key]:
            raise AssertionError(f"unexpected or changed Delivery frame: {frame}")
        if node == frame["origin"] or node in received[key]:
            continue
        if "payloadHex" in frame:
            call(node, MIX, "deliverCoordFrame", topic, frame["payloadHex"])
        received[key].add(node)


def drain():
    for node in range(count):
        for frame in call(node, MIX, "drainCoordBacklog"):
            topic = frame["contentTopic"]
            assert topic in TOPICS, topic
            envelope = {"origin": node, "id": len(published),
                        "payloadHex": frame["payloadHex"]}
            key = (node, topic, envelope["id"])
            published[key] = envelope
            received[key] = set()
            publish(node, topic, envelope)
    receive()


def wait_for(predicate, description, pump=receive, timeout=60):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        pump()
        if predicate():
            return
        time.sleep(0.1)
    raise RuntimeError(f"timed out: {description}; received={received}")


def start_delivery(node, peers, *, relay, service=False):
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    config = dict(
        logLevel="DEBUG", listenAddress="127.0.0.1", tcpPort=port,
        nat="extip:127.0.0.1", extMultiAddrs=[f"/ip4/127.0.0.1/tcp/{port}"],
        extMultiAddrsOnly=True, clusterId="198", numShardsInNetwork=1,
        relay=relay, store=False, filter=service, lightpush=service,
        peerExchange=False, discv5Discovery=False,
        reliabilityEnabled=True, staticnodes=peers,
    )
    if edge:
        config["rendezvous"] = False
    call(node, DELIVERY, "createNode", json.dumps(config))
    call(node, DELIVERY, "start")
    info = call(node, DELIVERY, "getNodeInfo", "MyMultiaddresses")
    address = re.search(r"/ip4/127\.0\.0\.1/tcp/\d+/p2p/[A-Za-z0-9]+", info)
    assert address, info
    for topic in TOPICS:
        call(node, DELIVERY, "subscribe", topic)
    return address.group()


def main():
    peers = []
    if edge:
        for node in range(count, count + 2):
            peers.append(start_delivery(node, peers[:1], relay=True, service=True))
        print("  two Relay service nodes provide Lightpush and Filter", flush=True)
    for node in range(count):
        # Split clients across services so coordination must cross the Relay link.
        upstream = [peers[node % 2]] if edge else peers[:1]
        address = start_delivery(node, upstream, relay=not edge)
        if not edge:
            peers.append(address)
        mix = call(node, MIX, "getLocalMixPeerRecord")
        assert address.rsplit("/", 1)[1] != mix["peerId"]
        print(f"  node {node}: Delivery relay={not edge} {address}", flush=True)
        proc = subprocess.Popen(
            [cli, "-j", "watch", DELIVERY, "--event", "messageReceived"],
            env=env(node), text=True, stdout=subprocess.PIPE,
        )
        watchers.append(proc)
        threading.Thread(target=watch, args=(node, proc), daemon=True).start()

    # Retry readiness probes while Relay/Filter subscriptions settle. Actual RLN
    # frames below must arrive on every remote node without test retransmits.
    for topic in TOPICS:
        key = (0, topic, "probe")
        published[key] = {"origin": 0, "id": "probe"}
        received[key] = set()
        def probe():
            publish(0, topic, published[key])
            time.sleep(1)
            receive()
        wait_for(lambda: len(received[key]) == count - 1,
                 f"Delivery readiness on {topic}", probe)

    for node in range(count):
        before = set(published)
        call(node, MIX, "registerRlnMembership")
        drain()
        keys = {key for key in published.keys() - before if key[1] == TOPICS[0]}
        assert keys, f"node {node} did not publish membership"
        wait_for(lambda: all(len(received[key]) == count - 1 for key in keys),
                 f"membership {node} received by all other nodes", drain)
    print("  memberships synced through Delivery", flush=True)
    (root / "coord-ready").touch()
    wait_for(lambda: (root / "coord-finish").exists(), "Mix payload test", drain)
    wait_for(lambda: any(key[1] == TOPICS[1] and "payloadHex" in published[key]
                         and len(destinations) == count - 1
                         for key, destinations in received.items()),
             "proof metadata received through Delivery", drain)
    if edge:
        for node in range(count):
            log = (root / f"node{node}" / "logs.txt").read_text()
            assert "Message propagated via Lightpush" in log, f"no Lightpush success on {node}"
        print("  all Delivery clients published through Lightpush", flush=True)
    for topic in TOPICS:
        total = sum(len(received[key]) for key in published
                    if key[1] == topic and "payloadHex" in published[key])
        print(f"  Delivery {topic}: {total} verified remote frame deliveries", flush=True)


def terminate(signum, frame):
    raise SystemExit(128 + signum)


signal.signal(signal.SIGTERM, terminate)
try:
    main()
finally:
    for proc in watchers:
        proc.terminate()
    for proc in watchers:
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
