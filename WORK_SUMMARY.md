# Standalone Mix-RLN module: current work summary

Updated 2026-09-17. This document describes the current implementation and
remaining work. See [README.md](README.md) for the detailed architecture,
configuration, API, and deployment examples.

## Architecture and behavior

The module runs a standalone Mix intermediate node. Its Nim FFI owns a
libp2p switch with core Mix and the bundled Mix-RLN spam-protection plugin.
Sphinx payloads travel over IPv4 TCP or QUIC; zerokit provides the RLN
cryptography. Logos Delivery is not a runtime dependency.

Application endpoint capabilities are independent, create-time opt-ins:

| Setting | Default | Enables |
| --- | --- | --- |
| `mix.allowSend` | `false` | Application sends and explicit SURB replies. |
| `mix.allowExit` | `false` | Receiver mounting and local/external application exit delivery. |

Intermediate forwarding and cover loops remain active with both options
disabled. Exit policy is enforced by the receiving core node, independently
of peer advertisements. Peer records carry `exitEnabled`, and route selection
reserves an eligible exit before choosing intermediates. The standalone FFI
sets the restrictive defaults explicitly; native core-library callers retain
`allowExit=true` for compatibility.

The host supplies two things:

- **Peer records:** obtain `getLocalMixPeerRecord` from participating nodes and
  install records with `addMixPeer`. `listMixPeers` reports the routing pool;
  automatic discovery is not implemented.
- **RLN coordination:** forward membership and proof-metadata frames from
  `RlnPublishRequested` or `drainCoordBacklog` through a transport, then submit
  received frames with `deliverCoordFrame`. Forwarding must continue after
  initial membership registration.

A separate Delivery module can provide that coordination transport, either
as a Relay node or a light client using Lightpush/Filter service nodes.
When used, Delivery and Mix have separate switches, peer IDs,
peer sets, and lifecycles. Proof generation and verification stay inside the
Mix-RLN plugin; there is no external RLN-module proof provider.

## Active PRs and dependency pins

PR status was checked on the update date. All changes below are pushed.

| PR | Status | Purpose |
| --- | --- | --- |
| [Core Mix #58](https://github.com/logos-co/nim-libp2p-mix/pull/58) | Open, targets `master` | Exit policy and capability storage, shared route selection, cover control, SURB ownership/replies, and compatible dependencies. |
| [Mix-RLN plugin #22](https://github.com/logos-co/mix-rln-spam-protection-plugin/pull/22) | Draft | Align libp2p and core Mix dependencies with the standalone facade. |
| [Zerokit #436](https://github.com/vacp2p/zerokit/pull/436) | Draft | Provide the stateless RLN build and C ABI required by the plugin. |
| [FFI #2](https://github.com/logos-co/nim-libp2p-mix-rln-ffi/pull/2) | Draft | Own the switch, enforce endpoint opt-ins, and expose host coordination and routing APIs. |
| [Module #2](https://github.com/logos-co/logos-libp2p-mix-rln/pull/2) | Draft | Package the FFI, expose the Logos API, and document/test the standalone deployment. |

Current consumed revisions:

| Dependency | Revision |
| --- | --- |
| FFI, pinned by this module | `53f4e0b9905743301c57cb105ee66d14250b8397` |
| Core Mix, pinned by both FFI and plugin | `57def1fef5763fc4cc27a386276cd65165eeb489` |
| Mix-RLN plugin, pinned by FFI | `dc820fd676fa6c44c1e8847e29afcd2bc39200fe` |
| Zerokit, locked by FFI | `a263930af8a7bd95804e1e49859050afbf2dbfd5` |
| Delivery module v0.2.1, optional E2E only | `b8b9ac2f4667bc63644b2116f64a07aa30cfd3ef` |

Core #58's head is `470177c7a377222d89621b32fdc5087de7350f7a`.
Its final commit only refreshes core's `nix/deps.nix`; the FFI uses its own
dependency snapshot and pins the preceding, validated runtime commit above.
The published FFI revision evaluates to the same Nix library derivation used
by the tested module package. A local FFI override is not required.

[Delivery #4181](https://github.com/logos-messaging/logos-delivery/pull/4181)
remains a separate draft for optional spam-protection injection. It is useful
independently but is not a prerequisite for this module. The redundant
[core #46](https://github.com/logos-co/nim-libp2p-mix/pull/46) experiment and the
superseded Delivery adapter/test PRs
[#4182](https://github.com/logos-messaging/logos-delivery/pull/4182) and
[#4185](https://github.com/logos-messaging/logos-delivery/pull/4185) are closed,
with explanations. None belongs in the merge path for the standalone stack.

## Validation and compatibility

The implementation was validated on 2026-09-17:

| Check | Result and scope |
| --- | --- |
| Core unit tests | 165 passed. |
| Core component tests | 33 passed, including local/external exit policy, forged advertisements, cover loops, and SURB ownership. |
| Module configuration tests | Five passed using the Logos test-framework runner. |
| Core Nix build | Passed after regenerating the dependency snapshot. |
| Nim routing tests | Plain and RLN-protected routing passed. |
| C API tests | TCP and QUIC passed: key validation, stable prefixed-key identity, address validation, peer listing, role restrictions, coordination, live cover updates, and SURB replies. |
| Module package | `.lgx` build and single-daemon lifecycle/validation test passed. |
| Five-daemon test | Peer listing, host-mediated membership synchronization, and Sphinx/RLN payload delivery passed with sender-only, intermediate-only, and exit-only roles. |
| Delivery coordination E2E | Five separate Relay nodes passed membership and proof-metadata frames through real receive events into Mix. All 20 remote membership deliveries and the Sphinx/RLN application payload were verified; Mix and Delivery peer IDs were distinct. Dropping membership publications made the fixture fail before routing. |
| Delivery light-client E2E | Five Delivery clients with Relay disabled used two separate Relay/Lightpush/Filter service nodes. Membership, proof metadata, and Mix payload delivery passed. Disabling Filter or removing the Relay link failed readiness before registration. |

These are completed local checks, not a claim that every remote CI platform
is green. `nix run .#delivery-coordination-e2e` and
`nix run .#delivery-edge-coordination-e2e` exercise controlled local Relay and
light-client coordination; the other fixtures use host test buses. This does
not establish production synchronization or partition recovery. Routing fixtures
use reduced cover rates and do not establish production performance at the default rate.

The public configuration now contains only settings applied by the runtime.
Unused discovery/bootstrap, inbound/outbound connection-limit, and RLN
fields were removed. Direct C consumers must rebuild against the generated
header because the configuration struct changed. Malformed configured
private keys now fail creation instead of silently generating a new identity.

## Remaining work

1. Merge core #58 and zerokit #436, then repin the plugin and merge #22.
   Update the FFI's Nimble/Nix pins to merged revisions and merge FFI #2;
   repin this module and merge module #2. Delivery PRs are outside this path.
2. Provide production coordination: distributed membership-index allocation,
   reliable publication, and membership-history synchronization for late
   joiners. Current controlled setup registers members sequentially.
3. Integrate host-managed discovery and validate the chosen coordination
   backend under deployment conditions beyond the local Relay E2E.
4. Validate sustained routing and proof-generation capacity at intended
   deployment parameters, including the default cover rate.
