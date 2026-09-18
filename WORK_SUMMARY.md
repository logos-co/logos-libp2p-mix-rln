# Mix / Delivery / shared RLN work summary

Updated: 2026-09-18. **The complete fresh-host shared-RLN fixture passed**,
including provisioning and the no-direct-fallback test. All seven integration
PRs and the wallet dependency fix are published and open. The final pinned
bundles build. The previously pending remote CI checks have passed.

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
  Subsequent README/work-summary edits do not change the tested runtime.

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

Core Mix #58 now includes commit `5803d68741eb54ea56ac0f91bc4e8f4caf34ce8e`.
Consumers pin functional revision `29eaaf1`; the later commits only refresh
the core repository's Nix dependency snapshot, which builds successfully and
passes the remote Nix drift/build check.
Plugin #22 includes `4cb0b16f8a9f3d7e8b1e759e2179277fb6bbd519`. Native Delivery
#4282 contains `9cc5babdd47d07bef396dafb54a367725ecb46e7`; Delivery module #125
contains `6c9a2f8684a1c81f2e84abe2dc57e6b4e1fb629d` (CI cleanup and the corrected wallet dependency). The FFI shared-provider change is committed as `b2008a2`.
This branch contains the Logos module bridge, current architecture docs, and
new local-chain network fixture. The complete fresh-host positive and negative fixture passed.

The shared backend branch is at `4f1f610a05d6108a88b6b9a4f365f6830365ae21`,
based on RLN modules main `6e3c6c47d4d1ae7efa635a61a171891c23967fd8` (0.8.2).
This replaces the older `feat/lip-alignment` baseline so local-chain layouts
match `logos-lez-rln` revision `7ea94fc8c42c9a50a49bb291eea17962f88ff0dc`.

- [Core Mix #58](https://github.com/logos-co/nim-libp2p-mix/pull/58)
- [Mix plugin #22](https://github.com/logos-co/mix-rln-spam-protection-plugin/pull/22)
- [Wallet fee cap #1](https://github.com/adklempner/logos-execution-zone/pull/1),
  commit `6752be252e441717ab934013cce101379ea4966b`.
- [Shared RLN #27](https://github.com/logos-co/logos-rln-modules/pull/27)
- [Native Delivery #4282](https://github.com/logos-messaging/logos-delivery/pull/4282)
- [Delivery module #125](https://github.com/logos-co/logos-delivery-module/pull/125)
- [Standalone FFI #2](https://github.com/logos-co/nim-libp2p-mix-rln-ffi/pull/2)
- [Logos module #2](https://github.com/logos-co/logos-libp2p-mix-rln/pull/2)
- [Zerokit #436](https://github.com/vacp2p/zerokit/pull/436), needed by the
  legacy embedded provider.

The previous standalone and optional Delivery-coordination tests passed for
that baseline. They do not prove the new native Delivery Mix path.
