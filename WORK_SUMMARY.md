# Mix / Delivery / shared RLN work summary

Updated: 2026-09-17. **The complete shared-RLN message path passed locally**,
including the no-direct-fallback test. All seven integration PRs are open.
Both final pinned module bundles build; fresh-host provisioning and remote CI
are tracked separately below.

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

## Implementation

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
  **61 tests passed**. Delivery Mix sending regression suite: **14 tests passed**,
  including rejection of shared Mix without protected Relay coordination.
- Both Mix and Delivery LGX bundles and the LEZ wallet runtime built. Seven
  isolated hosts have funded wallets and active scoped memberships on the local
  sequencer. Shared-provider standalone startup and metadata publication work.
- The legacy module lifecycle test passed. The five-node legacy Sphinx/RLN
  routing test also passed after enabling SDK worker dispatch.
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
- The final pinned Delivery bundle (native `9cc5bab`, module `d178781`) built
  with both native static and dynamic libraries. The passing network run used
  the same native runtime source in a dynamic-library build.

## Remaining validation

A fresh-host run with the final pinned bundles stopped during membership
provisioning: the local sequencer rejected a funded wallet transaction with
`Incorrect fee`. This happened before network startup, after sender, m1, and
m2 obtained both memberships. It does not count as a passing fresh fixture.
The diagnostic retry reproduced it: `max_fee=134400000` was below the
required `140004216`. The pinned wallet honors the configured 10,000,000 gas
limit but calculates its constant fee cap using the 2,000,000 default. The
wallet fee-cap correction is being tested in a dedicated dependency branch; retrying a still-pending
membership does not resubmit it. Logs: `/tmp/mix-final-network-retry.log` and
`/tmp/mix-shared-chain/devnet-diagnostic.log`. The earlier message-path result
stands; clean provisioning is not verified.

Native Delivery CI passed. Remote CI is still running for core Mix and the
Delivery module. The core Nix dependency snapshot and Delivery documentation's RLN
module pins were refreshed after CI exposed stale build metadata.

## PRs and publication state

Core Mix #58 now includes commit `350ee8ff0a78ab3325c863a76d87fc6e6aa09f40`.
Consumers pin functional revision `29eaaf1`; the later commit only refreshes
the core repository's Nix dependency snapshot, which also builds successfully.
Plugin #22 includes `4cb0b16f8a9f3d7e8b1e759e2179277fb6bbd519`. Native Delivery
#4282 contains `9cc5babdd47d07bef396dafb54a367725ecb46e7`; Delivery module #125
contains `6e8925c` (the tested runtime is `d178781`; the follow-up only updates CI). The FFI shared-provider change is committed as `b2008a2`.
This branch contains the Logos module bridge, current architecture docs, and
new local-chain network fixture. The positive and negative message-path checks passed.

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
