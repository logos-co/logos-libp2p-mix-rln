# logos-libp2p-mix-rln

Logos Core module implementing [LIP LOGOS-MIXNET][lip]: a Delivery-backed
mixnet using Sphinx routing with per-hop RLN rate limiting.

Sibling module to [`logos-libp2p-module`][libp2p-module]; shares its
C++/Qt plugin shape, config-via-env convention, and nim-ffi bridge pattern.

Wraps the C FFI facade at
[`logos-co/nim-libp2p-mix-rln-ffi`][nim-facade].

## Status

Validated end-to-end through a multi-daemon test: five `logoscore` daemons
route a real Sphinx-encrypted payload from node 0 to node 4 with per-hop
RLN proofs, exit-is-dest delivery, and payload verification on the receiver.

## Build

Requires [`logos-module-builder`][builder]. Because the C FFI facade
transitively pins zerokit v2 and its nix build needs two overrides (blocked
on [zerokit PR #436][zerokit-pr] — filed as draft, not for merge; see the
PR for why), the module build also needs to plumb those overrides through:

```sh
nix build .#lgx
```

The FFI facade pins the [zerokit fork branch][zerokit-fork] that carries
PR #436 as its default `zerokit` input, so no overrides are needed. Once
those packaging changes land upstream, the pin flips back to
`vacp2p/zerokit`.

## Tests

### Unit

```sh
nix run .#tests
```

### Standalone end-to-end (single daemon)

Loads the module under a live `logoscore` daemon, drives the lifecycle
(`createNode → start → getNodeInfo → stop`), asserts error handling:

```sh
nix run .#standalone-e2e
```

### Multi-node end-to-end (5 daemons, Sphinx + RLN)

Spawns N=5 independent `logoscore` daemons (each under its own `HOME`),
cross-registers peer records, syncs RLN memberships over Delivery Relay,
mounts a receiver protocol on the exit, sends a mix
message from node 0 to node 4 through the exit-is-dest path, and verifies
the payload byte-for-byte on the receiver's inbox:

```sh
nix run .#multi-node-e2e
```

Expected tail:

```
----- polling node 4 inbox for delivery -----
  inbox: proto=/logosmix/test/echo/1.0.0 payload='hello mix (through logoscore)'
PASS: N=5 mix nodes routed a payload through Sphinx + RLN via logoscore
```

`N=3 nix run .#multi-node-e2e` etc. changes the mix pool size; the Sphinx
path length is fixed at 3, so `N >= 3` is required.

## Using the module

The examples below use the packaged module through `lgpm` and `logoscore`.
They require `nix`, `jq`, `lgpm`, and `logoscore`.

### 1. Build, install, and load

Build the package and install the generated `.lgx` into an isolated module
directory:

~~~sh
nix build .#lgx
LGX=$(find -L result -name '*.lgx' -print -quit)
mkdir -p modules
lgpm --modules-dir ./modules --allow-unsigned install --file "$LGX"
logoscore -D -m ./modules >logoscore.log 2>&1 &
logoscore load-module libp2p_mix_rln_module
~~~

Define a helper for the remaining examples. Every method response is wrapped
as `{"result":{"success":...,"value":...,"error":...}}`; inspect
`.result.success` rather than relying only on the CLI exit status.

~~~sh
mixcall() {
  logoscore call libp2p_mix_rln_module "$@"
}

mixcall status | jq
~~~

The `--allow-unsigned` flag is only appropriate for local development. Use
the normal signed-package policy in deployments.

### 2. Create and start a node

Create the node before calling `start`. This TCP example asks the operating
system for a free port and uses a low cover rate suitable for development:

~~~sh
TCP_CONFIG='{
  "addrs": ["/ip4/0.0.0.0/tcp/0"],
  "transport": "tcp",
  "mix": {"cover": {"rateFraction": 0.01}}
}'

mixcall createNode "$TCP_CONFIG" | jq
mixcall start | jq
mixcall getNodeInfo PeerId | jq -r '.result.value'
mixcall getNodeInfo Multiaddrs | jq -r '.result.value'
mixcall getNodeInfo MixPublicKey | jq -r '.result.value'
~~~

For QUIC, select `quic` and provide a QUIC multiaddress:

~~~sh
QUIC_CONFIG='{
  "addrs": ["/ip4/0.0.0.0/udp/0/quic-v1"],
  "transport": "quic",
  "mix": {"cover": {"rateFraction": 0.01}}
}'

mixcall createNode "$QUIC_CONFIG" | jq
mixcall start | jq
~~~

Alternatively, set `LIBP2P_MIX_RLN_MODULE_CONFIG` to inline JSON or an
absolute path before starting `logoscore`. The module constructor then creates
the node when it is loaded, so only `start` is needed:

~~~sh
export LIBP2P_MIX_RLN_MODULE_CONFIG="$PWD/config.example.json"
logoscore -D -m ./modules >logoscore.log 2>&1 &
logoscore load-module libp2p_mix_rln_module
mixcall start | jq
~~~

Do not call `createNode` to reconfigure a running node. Stop it first,
create the replacement, and then start it.

### 3. Connect the Mix topology

Each node exposes a self-contained peer record. Install every other node's
record with `addMixPeer`. A Sphinx path requires at least three Mix nodes,
and each participating node needs a usable view of the pool.

On node B:

~~~sh
PEER_B=$(mixcall getLocalMixPeerRecord | jq -c '.result.value')
~~~

Pass that complete JSON object as one string to node A:

~~~sh
mixcall addMixPeer "$PEER_B" | jq
mixcall listMixPeers | jq '.result.value.peers'
~~~

Repeat this for the required peer pairs. `addMixPeer` both installs the Mix
routing record and establishes the Delivery-managed connectivity used by RLN
coordination. The
[`multi_node_e2e.sh`](tests/integration_e2e/multi_node_e2e.sh) script is the
canonical executable example for running several isolated daemons.

### 4. Register RLN membership

After the peers are connected, register each node:

~~~sh
mixcall registerRlnMembership | jq '.result.value'
mixcall hasRlnMembership | jq '.result.value'
mixcall getNodeInfo RlnMembershipIndex | jq -r '.result.value'
~~~

Registration returns `{"registered":true,"index":N}`. Current off-chain
membership announcements are best effort, so register nodes sequentially and
allow an announcement to propagate before registering the next node. Reliable
dissemination, distributed index allocation, and late-join synchronization
remain production work.

### 5. Receive and send Mix messages

For a Mix node that is itself the final destination, mount the receiving
libp2p codec before sending:

~~~sh
CODEC=/logosmix/example/1.0.0
mixcall mountReceiver "$CODEC" 4096 | jq
~~~

From another node, use the destination's peer ID:

~~~sh
DEST_PEER_ID=16Uiu2H...
mixcall sendMixMessageToExit "$DEST_PEER_ID" "$CODEC" "hello over mix" | jq
~~~

Poll and clear the destination inbox:

~~~sh
mixcall drainReceivedMessages | jq '.result.value'
~~~

Each entry has this shape:

~~~json
{
  "proto": "/logosmix/example/1.0.0",
  "payloadHex": "68656c6c6f206f766572206d6978",
  "surbHex": ""
}
~~~

`drainReceivedMessages` is destructive: messages returned by a call are
removed from the in-memory inbox. The CLI treats byte-string arguments as
literal bytes, so it is convenient for text examples. Applications should use
the generated Logos SDK for arbitrary binary payloads.

To route through Mix and then forward to an external libp2p destination, use
`sendMixMessage` with both its peer ID and reachable multiaddress:

~~~sh
mixcall sendMixMessage \
  "$DEST_PEER_ID" \
  "/ip4/192.0.2.10/tcp/9000" \
  "$CODEC" \
  "hello external destination" | jq
~~~

### 6. Request and send a SURB reply

Use `sendMixMessageToExitWithSurb` for a Mix exit destination, or
`sendMixMessageWithSurb` for an external destination. The send waits for the
reply and returns `{"ok":true,"reply":[...]}`.

The destination receives a non-empty `surbHex` in its inbox entry (and raw
SURB bytes in the `IncomingMixMessage` event). It must decode that value and
call `sendMixSurbReply(surbBytes, replyBytes)`. The reply is routed without
revealing a direct return address.

SURBs are arbitrary binary values and commonly contain zero bytes. Shell
arguments cannot safely carry them, so use the generated Logos SDK rather than
`logoscore call` for the `sendMixSurbReply` step. The application-level
sequence is:

~~~text
sender:      sendMixMessageToExitWithSurb(...)  ───────────────┐
destination: receive {payload, surb}                           │
destination: sendMixSurbReply(surb, reply) ───────────────────>│
sender:      returns {ok: true, reply: [...]}
~~~

The current synchronous SURB send uses a 10-second reply timeout. A destination
must process the request concurrently while the sender is waiting.

### 7. Inspect and update cover traffic

The configured fraction controls the live constant-rate scheduler and must be
in `(0.0, 1.0]`:

~~~sh
mixcall getCoverTrafficRate | jq '.result.value.rate'
mixcall setCoverTrafficRate 0.02 | jq
mixcall getCoverTrafficRate | jq '.result.value.rate'
~~~

The LIP default is `0.7`. That can be expensive with the current RLN proof
pipeline; use deployment-appropriate capacity and parameters.

### 8. Stop the node

~~~sh
mixcall stop | jq
logoscore stop
~~~

### Public API summary

| Area | Methods |
| --- | --- |
| Health | `ok`, `status` |
| Lifecycle | `createNode`, `start`, `stop` |
| Introspection | `getNodeInfo` |
| RLN | `registerRlnMembership`, `hasRlnMembership` |
| Sending | `sendMixMessage`, `sendMixMessageToExit`, `sendMixMessageWithSurb`, `sendMixMessageToExitWithSurb`, `sendMixSurbReply` |
| Topology | `getLocalMixPeerRecord`, `addMixPeer`, `listMixPeers` |
| Receiving | `mountReceiver`, `drainReceivedMessages` |
| Cover traffic | `getCoverTrafficRate`, `setCoverTrafficRate` |
| Diagnostics | `collectMetrics` (currently returns an empty map) |

`getNodeInfo` accepts `Version`, `PeerId`, `Multiaddrs`,
`MixPublicKey`, and `RlnMembershipIndex`.

## Config

Deployment config is delivered via `LIBP2P_MIX_RLN_MODULE_CONFIG` —
inline JSON or a path to a JSON file. See [`metadata.json`](metadata.json)
for the schema and [`config.example.json`](config.example.json) for a
sample.

Several LIP LOGOS-MIXNET parameters are marked TBD in the spec (`period`,
`messagingRate`, `maxEpochGap`, `stakedFund`, RLN Relay coord topics,
`coordCluster`). This module exposes them as config keys with
**placeholder defaults** — pin them to real values before any non-testnet
use.

Protocol constants that LIP LOGOS-MIXNET fixes (path length 3, Sphinx
sizes, `CONSTANT_RATE` cover strategy) are hard-coded in `src/config.h`
under `namespace lip_mixnet` and are not configurable.

## What's pending

**Upstream / dependencies**
- [zerokit PR #436][zerokit-pr] — draft, will land against v2.x once
  someone signs off. Longer-term, the whole facade needs a v3 migration
  because master (v3.0.0) removed the `stateless` feature entirely.

**Module-level**
- Discovery: `listMixPeers` currently returns from the in-process node
  pool populated by `addMixPeer`. Real deployment needs Logos Service
  Discovery integration (Extensible Peer Records per LIP LOGOS-MIXNET) so
  peers are found without shell-orchestrated cross-registration.
- RLN membership announcements are still best effort. Production use needs
  distributed index allocation, reliable dissemination, and late-join
  history synchronization.
- CI: the e2e apps run locally only; a GitHub Actions workflow guarding
  them against regressions is TBD.

SURB replies, RLN membership-index lookup, live cover-rate updates, and
TCP/QUIC transport selection are implemented and covered by the facade smoke test.

## Layout

```
logos-libp2p-mix-rln/
├── metadata.json                    # module manifest + config schema
├── config.example.json
├── flake.nix                        # module + e2e apps
├── CMakeLists.txt
├── src/
│   ├── plugin.h                     # Libp2pMixRlnModuleImpl — public C++ surface
│   ├── plugin.cpp                   # FFI bridge, event trampolines
│   └── config.h                     # options struct + JSON loader
├── tests/
│   ├── CMakeLists.txt
│   ├── unit_config.cpp
│   └── integration_e2e/
│       ├── standalone_e2e.sh        # single-daemon lifecycle
│       └── multi_node_e2e.sh        # N-daemon Sphinx + RLN e2e
├── LICENSE                          # MPL-2.0
└── README.md
```

[lip]: https://github.com/logos-co/logos-lips/pull/387
[libp2p-module]: https://github.com/logos-co/logos-libp2p-module
[nim-facade]: https://github.com/logos-co/nim-libp2p-mix-rln-ffi
[upstream-issues]: https://github.com/logos-co/nim-libp2p-mix-rln-ffi/blob/main/UPSTREAM_ISSUES.md
[builder]: https://github.com/logos-co/logos-module-builder
[zerokit-pr]: https://github.com/vacp2p/zerokit/pull/436
[zerokit-fork]: https://github.com/richard-ramos/zerokit/tree/nix-rln-stateless
