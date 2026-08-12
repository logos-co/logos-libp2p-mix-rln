# logos-libp2p-mix-rln

Logos Core module implementing [LIP LOGOS-MIXNET][lip]: a libp2p-based mixnet
using Sphinx routing with per-hop RLN rate limiting.

Sibling module to [`logos-libp2p-module`][libp2p-module]; shares its C++/Qt
plugin shape, config-via-env convention, and nim-ffi bridge pattern.

## Status: FFI wired, module builds cleanly

The C++ plugin bodies now call into the Nim FFI facade
[`nim-libp2p-mix-rln`][nim-facade] via the sync-over-async bridge pattern
that `logos-libp2p-module` uses. `nix build .#libp2p_mix_rln_module-lib`
produces `libp2p_mix_rln_module_plugin.so` — verified end-to-end with the
zerokit v2 overrides described below.

Wired methods:
- `createNode` (rebuilds the Nim `LibMixRlnCtx` from JSON config)
- `start` / `stop` / `getNodeInfo` (Version / PeerId / Multiaddrs)
- `registerRlnMembership` / `hasRlnMembership`
- `sendMixMessage` / `sendMixMessageWithSurb` / `sendMixSurbReply`
- `listMixPeers` / `getCoverTrafficRate` / `setCoverTrafficRate`

Event fan-out: `onIncomingMixMessage`, `onRlnMembershipRegistered`,
`onRlnPublishRequested` are registered on ctx-create; each dispatches through
the plugin's `emitEvent` `std::function` to the host.

## Build

Requires the same zerokit v2 overrides as `nim-libp2p-mix-rln` until its
upstream fixes land:

```sh
nix build .#libp2p_mix_rln_module-lib \
  --override-input libp2p-mix-rln path:/path/to/nim-libp2p-mix-rln \
  --override-input libp2p-mix-rln/zerokit path:/path/to/zerokit-v2-fork \
  --override-input libp2p-mix-rln/zerokit/nixpkgs github:NixOS/nixpkgs?rev=cd648d6ea62bc0ffba91e61fcfe5e33c1e2004b1
```

See `nim-libp2p-mix-rln/README.md` for what the zerokit-v2 fork needs.

## What's still not wired

Runtime behavior only exercised via a minimal smoke test on the Nim side
(NimMain → ctx_create → get_node_info(PeerId) → destroy). Full end-to-end
messaging between two mix nodes has not been demoed. Known stubs remaining:

- `getNodeInfo(MixPublicKey | RlnMembershipIndex)` — upstream API gap
- `sendMixSurbReply` — upstream reply-store lookup not exposed
- `listMixPeers` — needs Logos Service Discovery wiring

[nim-facade]: https://github.com/logos-co/nim-libp2p-mix-rln

### What's still needed for full spec compliance

1. **`nim-libp2p-mix-rln` — Nim FFI facade.** A separate repo (not yet
   created) analogous to how `nim-libp2p` exposes a `cbind` package consumed
   by `logos-libp2p-module`. It must compose:
   - `nim-libp2p` — host / transports
   - [`nim-libp2p-mix`][mix] — Sphinx, cover traffic, mix protocol
   - [`mix-rln-spam-protection-plugin`][mix-rln] — the `SpamProtection`
     override that generates/verifies per-hop RLN proofs
   - RLN Relay coordination for membership + proof metadata

   and produce `libp2p_mix_rln.{so,dylib,dll}` + `lib/libp2p_mix_rln.h`.

   **Blocker:** the two Nim inputs pin different `libp2p` versions today
   (`nim-libp2p-mix` HEAD → `2.2.0`; `mix-rln-spam-protection-plugin` HEAD →
   `2.1.4`). Pin both to SHAs that agree on one version — likely by bumping
   the mix-rln plugin's requirement to `2.2.0`.

2. **Wire the FFI into `src/plugin.cpp`.** Replace the `notImplemented()`
   stubs with the same sync-over-async bridge `logos-libp2p-module` uses
   (`SyncPromise` + `awaitResult` + typed reply trampolines). Mirror
   `Libp2pModuleImpl::createNode / start / stop` for lifecycle; add
   mix-specific paths for `sendMixMessage`, SURB, and RLN membership.

## Config

Deployment config is delivered via the `LIBP2P_MIX_RLN_MODULE_CONFIG`
environment variable — inline JSON, or a path to a JSON file. See
[`metadata.json`](metadata.json) for the schema and
[`config.example.json`](config.example.json) for a sample.

Several LIP LOGOS-MIXNET parameters are marked TBD in the spec (`period`,
`messagingRate`, `maxEpochGap`, `stakedFund`, the RLN Relay coord topics and
`coordCluster`). This module exposes them as config keys with **placeholder
defaults** — pin them to real values before any non-testnet use.

Protocol constants that LIP LOGOS-MIXNET fixes (path length 3, Sphinx sizes,
`CONSTANT_RATE` cover strategy) are hard-coded in `src/config.h` under
`namespace lip_mixnet` and are not configurable.

## Layout

```
logos-libp2p-mix-rln/
├── metadata.json              # module manifest + config schema
├── config.example.json
├── flake.nix                  # logos-module-builder wiring
├── CMakeLists.txt             # logos_module(...) target
├── src/
│   ├── plugin.h               # Libp2pMixRlnModuleImpl — public C++ surface
│   ├── plugin.cpp             # stubs; wire the FFI here
│   └── config.h               # options struct + JSON loader (working today)
├── tests/
│   ├── CMakeLists.txt
│   └── unit_config.cpp        # exercises the config loader
├── LICENSE                    # MPL-2.0 (matches logos-libp2p-module)
└── README.md
```

## Build

Requires [`logos-module-builder`][builder]. Once the Nim FFI facade exists and
is added as a flake input:

```sh
nix build .#lgx        # bundle into .lgx
nix run .#tests        # run unit tests
```

Until then, only `tests/unit_config.cpp` is meaningful.

[lip]: https://github.com/logos-co/logos-lips/pull/387
[libp2p-module]: https://github.com/logos-co/logos-libp2p-module
[mix]: https://github.com/logos-co/nim-libp2p-mix
[mix-rln]: https://github.com/logos-co/mix-rln-spam-protection-plugin
[builder]: https://github.com/logos-co/logos-module-builder
