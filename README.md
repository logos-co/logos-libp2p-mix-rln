# logos-libp2p-mix-rln

Logos Core module implementing [LIP LOGOS-MIXNET][lip]: a libp2p-based
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
cross-registers peer records, syncs RLN memberships by shuttling coord
frames in shell, mounts a receiver protocol on the exit, sends a mix
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
- Publish [`nim-libp2p-mix-rln-ffi`][nim-facade] as public — done.
- See [`nim-libp2p-mix-rln-ffi/UPSTREAM_ISSUES.md`][upstream-issues] for
  the smaller nim-libp2p / mix / mix-rln API gaps that force the facade
  to keep private wrappers.

**Module-level**
- Discovery: `listMixPeers` currently returns from the in-process node
  pool populated by `addMixPeer`. Real deployment needs Logos Service
  Discovery integration (Extensible Peer Records per LIP LOGOS-MIXNET) so
  peers are found without shell-orchestrated cross-registration.
- RLN coord: `deliverCoordFrame` / `drainCoordBacklog` is the pull-based
  path used by the multi-node test. The event-driven path
  (`onRlnPublishRequested` → host forwards) is wired but not exercised
  end-to-end; a real host binding it to the RLN Relay pubsub topic is TBD.
- `sendMixSurbReply` — stubbed pending reply-store lookup upstream.
- `getNodeInfo(RlnMembershipIndex)` — stubbed pending group-manager
  accessor upstream.
- Cover traffic rate is stored but scheduler propagation is a no-op.
- CI: the e2e apps run locally only; a GitHub Actions workflow guarding
  them against regressions is TBD.

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
