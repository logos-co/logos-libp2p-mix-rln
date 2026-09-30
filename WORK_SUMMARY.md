# Mix / Delivery / shared RLN work summary

Updated: 2026-09-29. The current integration chain is:

- Core Mix #58: `d4aeff5f032563fc0f9b042a1c8c049d9fa69fba`
- Mix plugin #22: `ac83f368e286c033e72fbd08dc63a4a802cfac0d`
- Shared RLN #27: `63bb541d18c53e3c6e261421ee9eb8c2fd8445ca`
- Native Delivery #4282: `0701e3c916c71831a695c43ffd40d3c8ba5941ab`
- Delivery module #125: `fba4da35b1d8193fc30e34a7c007e621ba16b5ad`
- Standalone FFI `main`: `3fa97a884320a63a1c4a381da7cd625b7ec02cb4`
  (merge commit for FFI #7)

FFI #2, FFI #7, and Logos module #2 are merged. The five integration PRs
listed above remain open. Standalone Mix now requires the shared RLN backend;
the embedded provider and direct Zerokit v2 dependency are removed. The FFI
repository is renamed to `logos-co/nim-libp2p-mix-ffi`. Its removal and rename
commits are published. TCP/QUIC C smoke tests pass with mock backend callbacks.
The shared-only seven-host fixture also passed with fresh wallets, real proofs,
protected coordination, exact payload delivery, and no direct fallback.

## Accepted architecture

- The standalone module owns its libp2p switch. Every node can forward, send,
  and act as an exit by default. Applications mount receivers for the local
  protocols they serve.
- Applications send through Delivery's existing `send()` API with anonymity
  `Required`. Delivery's native Mix implementation creates the Sphinx route.
  Standalone intermediates forward it; a Delivery Mix exit handles Lightpush.
  The response returns through a SURB; Relay/Filter delivers the application
  message onward to the recipient. `Required` must never fall back to a
  direct application send.
- Both native Delivery Mix and standalone Mix call `liblogos_rln_module` for
  proofs and verification. The backend owns credentials, registry membership,
  durable message-id allocation, and cryptography. The Mix adapter owns the
  existing packet encoding and proof-metadata coordination.
- Mix and Relay RLN use separate application scopes. A host sharing one RLN
  backend configures all its registries and owns its lifetime. Delivery's
  preset gains `manage-backend=false` for this deployment.
- Standalone Mix coordination uses a separately managed Delivery Relay node
  in the current fixture. Sender and recipient remain Delivery light clients.
  Those two switches have different peer IDs and routing tables. Loading
  Delivery alone does not connect this bridge.
- The merged Logos Mixnet specification requires RLN-protected Relay
  coordination. Its current profile uses 10-second epochs, 100 messages per
  epoch, accepted epoch gap 3, and root window 5. Deployment identifiers and
  topics still need agreement. Generic host coordination is not, by itself,
  evidence of Logos Mixnet conformance.

## Implementation

| Repository / checkout | Change |
| --- | --- |
| Core Mix — `/tmp/mix-role-policy` | Async proof generation/verification; deferred cover proof allocation for durable providers; cancellation of pending proof work. Existing synchronous providers remain supported. |
| Mix plugin — `/tmp/mix-plugin-review` | Shared RLN API adapter, Mix proof encoding, metadata coordination, bounded asynchronous request transport. |
| Shared RLN — `/tmp/mix-shared-rln` | Reconstruct an omitted Mix external nullifier from scope and timestamp, verify the binding, and return it for coordination. Canonical proof handling is preserved. |
| Standalone FFI — `/home/r/logos-co/nim-libp2p-mix-rln-ffi` | Shared-provider configuration, async request/reply events, backend membership calls, shutdown cancellation. |
| This module | C++ async backend bridge; shared provider is the only backend; RLN backend declared as a dependency. |
| Native Delivery — `/tmp/mix-delivery-native-interop` | Install the same adapter in native Mix, publish/receive coordination outside `send(Required)`, expose Mix peer records, and keep Mix/Relay callback lifetimes separate. |
| Delivery module — `/tmp/mix-delivery-api-interop` | Forward native Mix RLN requests to the backend, expose peer record methods, support host-owned backend lifecycle. |

The standalone embedded provider and its direct Zerokit v2 build dependency
are removed. Standalone Mix requires the shared RLN backend. Removed local
keystore/tree/provider settings are rejected; C ABI consumers must rebuild.
The shared backend still owns its cryptographic dependencies.

## Verification

Completed locally:

- The latest dependency chain built and passed the seven-host fixture. Delivery
  keeps explicitly registered standalone Mix peers even when the generic
  pure-libp2p budget is zero; unregistered peers remain budget-limited. The
  focused peer-admission regression passed, Delivery module #125 rebuilt from
  the final #4282 head, every scoped membership became active, and the exact
  `Delivery Required` payload reached the recipient through all three
  standalone intermediates. After stopping them, ordinary Relay delivery
  remained usable while Required traffic did not bypass Mix.
  Run:
  `/tmp/mix-rln-e2e/runs/20260929-193640-shared-delivery-mix-local`.
- Core Mix: **165 unit tests** and **34 component tests** passed after async
  integration and cancellation cleanup, including the cancellation regression
  assertion.
- Shared adapter: **3 tests passed**, covering asynchronous scoped calls,
  quota/backend failures, malformed replies, duplicate metadata, cancellation,
  and late/duplicate responses.
- Shared RLN backend: **8 real-cryptography validation tests passed**, including
  omitted-nullifier reconstruction and wrong-scope/wrong-signal rejection.
- Delivery RLN transport: **10 tests passed**, including independent callback
  lifetimes, duplicate response rejection, cancellation, and the pending limit.
- Standalone FFI smoke test passed over **TCP and QUIC** with the pinned core
  and adapter revisions. Standalone C++ module: **6 configuration tests passed**.
- Native Delivery dynamic and static libraries built. Delivery C++ module:
  **61 tests passed**. Delivery Mix sending regression suite: **14 tests passed**,
  including rejection of shared Mix without protected Relay coordination.
- Both Mix and Delivery LGX bundles and the LEZ wallet runtime built. Seven
  isolated hosts have funded wallets and active scoped memberships on the local
  sequencer. Shared-provider standalone startup and metadata publication work.
- After embedded-provider removal, the five-node FFI smoke test passed over
  TCP and QUIC using mock shared-backend callbacks. This checks the C bridge
  and routing. The shared-only LGX bundle builds and all six configuration
  tests pass, including rejection of removed settings. The renamed FFI pin
  also builds successfully with all six configuration tests passing.
- After removal, the seven-host fixture passed with FFI `67765f6` and the
  shared-only main runtime: all memberships became active, Delivery Required
  reached the recipient through standalone Mix, and stopping the intermediates
  blocked Required delivery while Relay remained usable. FFI `e13dbdc` only
  renames the repository references and Nix package names; it builds separately.
  Log: `/tmp/mix-shared-only-network.log`.
  Run: `/tmp/mix-rln-e2e/runs/20260918-085857-shared-delivery-mix-local`.
- Live testing found and fixed a host event-loop deadlock in standalone startup:
  synchronous API handlers now run on SDK workers. It also exposed Delivery's
  metadata handshake disconnecting standalone Mix-only peers before Identify
  supplied their protocols. Registered Mix peers now wait for Identify; only
  those without Waku metadata skip the handshake. **9 peer-manager tests pass**
  across TCP and QUIC, including unregistered-peer and cluster-mismatch rejection.
  The registered-peer regression was observed failing before the ordering fix.
- The seven-host native Delivery message path passed with real shared-backend
  memberships and protected coordination: `send(Required)` → standalone
  intermediates → native Delivery Mix/Lightpush exit → Relay/Filter recipient.
  The exact application payload arrived. After stopping all intermediates,
  the Required payload remained absent while an ordinary Relay control arrived.
  Log: `/tmp/mix-identified-network.log`.
- The complete fixture passed from seven fresh wallets using the final pinned
  Mix, Delivery, shared RLN, and registry bundles. Every scoped membership
  became active, the exact payload arrived through Mix, all Mix participants
  received protected metadata, and the no-direct-fallback check passed.
  Rerun with direct Relay coordination on all three intermediate hosts passed;
  their Delivery logs confirm Relay enabled and Filter/Lightpush services disabled.
  Sender and recipient remained light clients.
  Log: `/tmp/mix-relay-coordination-network.log`.
  Run: `/tmp/mix-rln-e2e/runs/20260918-081055-shared-delivery-mix-local`.
- Fresh provisioning exposed a registry wallet bug: it honored a configured
  10,000,000 gas limit but kept a fee cap calculated for 2,000,000. The wallet
  dependency now calculates the cap from the configured limit. **All 50 wallet
  unit tests pass**, and the complete fixture passed after pinning the fix.
- The final bundles were built from main module runtime `2eea783` plus the
  shared-backend pin below; Delivery module `6c9a2f8`; shared backend `4f1f610`;
  registry `2f5ba3c`; and wallet `6752be25`. Native Delivery remains `9cc5bab`.
  These end-to-end results predate the subsequent embedded-provider removal.

## Upstream wallet migration (2026-09-21)

The root flake overrides the registry module's wallet input to upstream LEZ
`f0778a4316daa4065ff18a77f4f98706149c240e`, which contains the configurable
gas-limit fee-cap fix merged in LEZ #884. The earlier fork pin remains above
only as a record of the earlier integration runs.

On x86_64-linux, the Mix and registry LGX builds pass, along with all six module
configuration tests, 49 upstream wallet unit tests, and six wallet FFI tests.
The original fee-cap regression also passes against upstream's `max_fee_for`:
2,000,000 gas keeps the 134,400,000 cap, and 10,000,000 gas gets 646,400,000.
The installed registry wallet library matches the upstream-built artifact.
Build log: `/tmp/mix-upstream-wallet-registry-build.log`.
Wallet tests: `/tmp/mix-upstream-wallet-tests.log`.
Fee-cap regression: `/tmp/mix-upstream-wallet-fee-regression.log`.

A first fresh-chain fixture run encountered the local chain's initial zero
`CLOCK_50`: registrations created before its first update appeared expired
when it advanced to real time. The fixture documentation now states this
precondition. This is separate from wallet fee admission.

The rerun passed with seven fresh funded wallets on the initialized local
chain: every scoped membership became active, all Mix participants received
protected metadata, the exact payload arrived through Mix, and stopping the
intermediates blocked Required traffic while ordinary Relay delivery continued.
Log: `/tmp/mix-upstream-wallet-network-rerun.log`.
Run: `/tmp/mix-rln-e2e/runs/20260921-094653-shared-delivery-mix-local`.

## Remote validation and deployment limits

Native Delivery, shared RLN, core Mix, and Delivery module CI passed, as did
this module's code-scanning jobs. This includes the core Nix snapshot/build
check. Delivery CI now stages the registry-owned wallet dependency
chain instead of the removed `lez_core` module.

The fresh fixture used new hosts and wallets on an existing isolated local
chain deployment. It does not establish a production deployment, automatic
service discovery, full specification conformance, or deployment identifiers.
Those limits are described in the README.

## PRs and publication state

The current open PR heads are Core Mix #58
`d4aeff5f032563fc0f9b042a1c8c049d9fa69fba`, Mix plugin #22
`ac83f368e286c033e72fbd08dc63a4a802cfac0d`, Shared RLN #27
`63bb541d18c53e3c6e261421ee9eb8c2fd8445ca`, Native Delivery #4282
`0701e3c916c71831a695c43ffd40d3c8ba5941ab`, and Delivery module #125
`fba4da35b1d8193fc30e34a7c007e621ba16b5ad`.

FFI #7 merged as `3fa97a884320a63a1c4a381da7cd625b7ec02cb4`;
this module pins that `main` commit rather than the PR head. It contains the
latest Core Mix and Mix plugin pins. The shared and registry module inputs both
pin Shared RLN #27. Native Delivery pins the same Core Mix and Mix plugin
revisions, and Delivery module #125 pins Native Delivery #4282.

The compatible `logos-lez-rln` checkout remains
`7ea94fc8c42c9a50a49bb291eea17962f88ff0dc`.

- [Core Mix #58](https://github.com/logos-co/nim-libp2p-mix/pull/58)
- [Mix plugin #22](https://github.com/logos-co/mix-rln-spam-protection-plugin/pull/22)
- [Wallet gas limit and fee cap #884](https://github.com/logos-blockchain/logos-execution-zone/pull/884) — merged upstream,
  including fee-cap fix `f0778a4316daa4065ff18a77f4f98706149c240e`.
  The root flake now pins this upstream wallet through the registry module's
  nested input. Earlier integration runs used fork commit
  `6752be252e441717ab934013cce101379ea4966b`; that fork PR is closed.
- [Shared RLN #27](https://github.com/logos-co/logos-rln-modules/pull/27)
- [Native Delivery #4282](https://github.com/logos-messaging/logos-delivery/pull/4282)
- [Delivery module #125](https://github.com/logos-co/logos-delivery-module/pull/125)
- [Standalone FFI #2](https://github.com/logos-co/nim-libp2p-mix-ffi/pull/2) — merged.
- [Standalone FFI #7](https://github.com/logos-co/nim-libp2p-mix-ffi/pull/7) — merged.
- [Logos module #2](https://github.com/logos-co/logos-libp2p-mix-rln/pull/2) — merged before the shared-only cleanup.
- [Zerokit #436](https://github.com/vacp2p/zerokit/pull/436) is **closed**.
  The shared-RLN integration does not require it to merge. The standalone FFI
  no longer references its fork.

The embedded-only lifecycle and routing fixtures were removed. The shared
Delivery/Mix fixture remains the real-cryptography end-to-end test.
