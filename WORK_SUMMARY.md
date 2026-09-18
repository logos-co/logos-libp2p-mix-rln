# Mix / Delivery / shared RLN work summary

Updated: 2026-09-17. **The shared-RLN migration is still in progress.** Core
Mix and adapter changes are pushed; the complete Delivery/module integration
is not ready yet. This file distinguishes verified work from remaining work.

## Accepted architecture

- The standalone module owns its libp2p switch and defaults to an intermediate
  role: `mix.allowSend=false`, `mix.allowExit=false`. Endpoint capabilities
  remain deliberate opt-ins.
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
- Standalone Mix coordination can use a separately managed Delivery node.
  Those two switches have different peer IDs and routing tables. Loading
  Delivery alone does not connect this bridge.
- The merged Logos Mixnet specification requires RLN-protected Relay
  coordination. Its current profile uses 10-second epochs, 100 messages per
  epoch, accepted epoch gap 3, and root window 5. Deployment identifiers and
  topics still need agreement. Generic host coordination is not, by itself,
  evidence of Logos Mixnet conformance.

## Implemented locally

| Repository / checkout | Change |
| --- | --- |
| Core Mix — `/tmp/mix-role-policy` | Async proof generation/verification; deferred cover proof allocation for durable providers; cancellation of pending proof work. Existing synchronous providers remain supported. |
| Mix plugin — `/tmp/mix-plugin-review` | Shared RLN API adapter, Mix proof encoding, metadata coordination, bounded asynchronous request transport. |
| Shared RLN — `/tmp/mix-shared-rln` | Reconstruct an omitted Mix external nullifier from scope and timestamp, verify the binding, and return it for coordination. Canonical proof handling is preserved. |
| Standalone FFI — `/home/r/logos-co/nim-libp2p-mix-rln-ffi` | Shared-provider configuration, async request/reply events, backend membership calls, shutdown cancellation. |
| This module | C++ async backend bridge; shared provider is the default; RLN backend declared as a dependency. |
| Native Delivery — `/tmp/mix-delivery-native-interop` | Install the same adapter in native Mix, publish/receive coordination outside `send(Required)`, expose Mix peer records, and keep Mix/Relay callback lifetimes separate. |
| Delivery module — `/tmp/mix-delivery-api-interop` | Forward native Mix RLN requests to the backend, expose peer record methods, support host-owned backend lifecycle. |

The legacy embedded Mix RLN provider remains during migration. It uses a
different zerokit generation and external-nullifier construction; mixing
legacy and shared-provider participants is not a supported deployment.

## Verification

Completed locally:

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
  **61 tests passed**. Delivery Mix sending regression suite: **13 tests passed**,
  including rejection of shared Mix without protected Relay coordination.
- Both Mix and Delivery LGX bundles built. The isolated local sequencer and
  registry are provisioned; the LEZ wallet runtime build is still running.

Not yet verified:

- Full `Delivery.send(Required)` → standalone intermediates → Delivery exit →
  recipient, with real shared-backend memberships and protected coordination.
- The corresponding no-direct-fallback negative end-to-end test.
- Final pinned builds, module tests, updated architecture examples, and PR CI.

## Remaining work

1. Finish library/module builds and regression checks; fix any remaining issues.
2. Reuse `logos-rln-e2e`'s local sequencer, wallet funding, and registry tooling
   for the new topology. Its old Mix scenario is quarantined and is not a
   usable acceptance test. The isolated checkout at `/tmp/mix-local-lez` has built and provisioned
   a local registry; its module wallet runtime is still building.
3. Run the complete positive and negative message-path tests.
4. Update README diagrams, startup/configuration examples, and test commands
   to describe the verified architecture. Remove migration-only duplication
   where the verified replacement allows it.
5. Finish publication and final pinned builds. The Delivery and shared-backend
   draft PRs are open; update their descriptions and this file with the final
   network results.

## PRs and publication state

Core Mix #58 now includes commit `29eaaf1d6adb57fa95e70d0c577cf6c4855598d9`.
Plugin #22 includes `4cb0b16f8a9f3d7e8b1e759e2179277fb6bbd519`. Native Delivery
#4282 contains `ab951c46b63c3d066f9c18ecf82a3598220291c3`; Delivery module #125
contains `429096cbcdaea67a0cdb827ce62e8ed37ba05b83`. The FFI shared-provider change is committed as `b2008a2`.
This branch contains the Logos module bridge, current architecture docs, and
new local-chain network fixture. The full network run remains pending.

The shared backend branch is at `f501685dd8d65452508cac77db6fae967feec6ef`,
based on RLN modules main `6e3c6c47d4d1ae7efa635a61a171891c23967fd8` (0.8.2).
This replaces the older `feat/lip-alignment` baseline so local-chain layouts
match `logos-lez-rln` revision `7ea94fc8c42c9a50a49bb291eea17962f88ff0dc`.

- [Core Mix #58](https://github.com/logos-co/nim-libp2p-mix/pull/58)
- [Mix plugin #22](https://github.com/logos-co/mix-rln-spam-protection-plugin/pull/22)
- [Shared RLN #27](https://github.com/logos-co/logos-rln-modules/pull/27)
- [Native Delivery #4282](https://github.com/logos-messaging/logos-delivery/pull/4282)
- [Delivery module #125](https://github.com/logos-co/logos-delivery-module/pull/125)
- [Standalone FFI #2](https://github.com/logos-co/nim-libp2p-mix-rln-ffi/pull/2)
- [Logos module #2](https://github.com/logos-co/logos-libp2p-mix-rln/pull/2)
- [Zerokit #436](https://github.com/vacp2p/zerokit/pull/436), needed by the
  legacy embedded provider.

The previous standalone and optional Delivery-coordination tests passed for
that baseline. They do not prove the new native Delivery Mix path.
