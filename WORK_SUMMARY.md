# Delivery-backed Mix-RLN integration: work summary

Status captured on 2026-09-01.

## Objective

The goal of this work was to avoid shipping and operating two libp2p nodes when a Logos application uses both Mix-RLN and Logos Delivery.

The original separation had two networking owners:

- the Mix-RLN FFI/module stack created and managed its own libp2p switch;
- Delivery created and managed another libp2p switch for Relay, Lightpush, peer management, and the other Delivery protocols.

The implemented direction makes Delivery's `WakuNode` the networking owner. Mix and RLN coordination reuse the same Delivery switch and peer connections.

```text
logos-libp2p-mix-rln
          |
          v
nim-libp2p-mix-rln-ffi
          |
          v
Delivery WakuNode
          |
          +-- one libp2p Switch -- Waku Relay -- RLN coordination
          |                   |
          |                   +-- Waku Mix -- Sphinx payload routing
          |                                      |
          |                                      +-- injected Mix-RLN protection
          |
          +-- Delivery peer management and lifecycle
```

## Architectural decisions

### Use Delivery instead of creating another libp2p node

Mix is mounted on `WakuNode.switch`. RLN membership updates and proof metadata are carried as Delivery Relay messages. Mix payload routing, Relay coordination, peer dialing, and shutdown therefore share one switch and one node lifecycle.

This removes the need for the host application to:

- run a second libp2p node;
- copy peer state between nodes;
- subscribe to an RLN publication callback and manually forward coordination frames;
- maintain a host-side backlog for coordination frames received before all FFI contexts are ready.

### Keep the integration modular

The Mix-RLN provider is injected through the existing Mix spam-protection interface. Delivery does not absorb the plugin's implementation. The provider remains a separate dependency with a clear boundary, while Delivery supplies transport, coordination, and lifecycle integration.

### Preserve compatibility where practical

The FFI C ABI remains unchanged. The former host-publish event and manual coordination-frame injection symbols remain available as compatibility paths, but the Delivery-backed integration no longer needs them.

### Allow development before upstream merges

The repositories use exact commit pins, so downstream work and end-to-end validation can continue while the upstream pull requests remain open. These pins must be updated to final merged revisions later.

## Work completed by repository

### Core Mix library

[`logos-co/nim-libp2p-mix#58`](https://github.com/logos-co/nim-libp2p-mix/pull/58)
completes the protocol primitives needed by downstream layers:

- makes the constant-rate cover fraction mutable while the scheduler is running;
- exposes received SURBs from exit connections;
- sends replies through a supplied SURB;
- updates a Mix node's advertised address after an ephemeral transport port is bound;
- adds focused cover scheduler, SURB, and address-update tests.

The signed PR head is `75f8bd5386f08ea909332774ddbeb78599b89d92`.

### Mix-RLN spam-protection plugin

[logos-co/mix-rln-spam-protection-plugin#22](https://github.com/logos-co/mix-rln-spam-protection-plugin/pull/22) aligns the plugin's dependency graph with Delivery:

- updates nim-libp2p to 2.3.1;
- pins nim-libp2p-mix to Delivery's revision;
- removes the conflicting duplicate libp2p resolution that prevented injection into Delivery's Mix instance.

This PR changes dependency resolution, not plugin behavior.

The plugin's existing membership-index accessor is now consumed by the FFI.
The signed PR head is `2dee9aaa2214895805fded977543c2770db4dc16`.

### Logos Delivery: spam-protection injection seam

[logos-messaging/logos-delivery#4181](https://github.com/logos-messaging/logos-delivery/pull/4181) adds the generic injection point:

- adds optional spam protection to `MixConf` and `MixConfBuilder`;
- carries the instance through node construction and `mountMix`;
- initializes the existing `WakuMix` on `node.switch` with the provider;
- selects `SpamProtectionDelayStrategy` when protection is enabled so proof-generation time does not collapse short Mix delays into a timing signal;
- preserves existing behavior when no provider is supplied.

No second switch or node is created.

### Logos Delivery: Mix-RLN adapter and coordination

[logos-messaging/logos-delivery#4182](https://github.com/logos-messaging/logos-delivery/pull/4182), stacked on #4181, implements the Delivery-specific adapter:

- constructs the Mix-RLN provider from optional programmatic configuration;
- injects it into the existing `WakuMix` instance;
- publishes membership updates and proof metadata as ephemeral, autosharded Waku messages over Delivery Relay;
- listens for those coordination messages through `MessageSeenEvent` without replacing application handlers;
- routes received coordination payloads into the plugin;
- integrates provider initialization, startup, shutdown, and self-registration with the Delivery node lifecycle;
- requires Relay and autosharding when Mix-RLN coordination is enabled;
- adds configuration and integration-oriented test coverage;
- adds the plugin to the reproducible Nimble and Nix dependency graph.

### Logos Delivery: two-node E2E and CI repair

[logos-messaging/logos-delivery#4185](https://github.com/logos-messaging/logos-delivery/pull/4185), stacked on #4182, adds the two-node Delivery integration test and fixes the issues exposed while running it.

The test verifies that:

- each Mix instance uses its `WakuNode`'s existing libp2p switch;
- two RLN registrations propagate between Delivery nodes over Relay;
- a proof with mismatched binding data is rejected;
- the same proof with correct binding data is accepted;
- proof metadata propagates to the other node;
- reuse is rejected as a duplicate.

Two test defects were corrected:

- Relay, Mix, and coordination are now mounted before the Delivery node starts, matching the libp2p rule that protocols mounted on a running switch must already be started;
- mismatched binding data is expected to produce an error result, matching the plugin's current verification contract.

The initial #4185 CI runs also exposed non-reproducible dependency resolution:

- `nim-snappy` version 0.1.0 tracks its moving `master` branch, so a clean install no longer matched the old lock revision;
- Nimble 0.24.1 could retain Chronos 4.2.4 even though the generated supplemental requirements and lock expected 4.2.5.

The repair:

- pins `nim-snappy` to revision `a99d113197e81bf764a3b005b0ade3f9f3758069`;
- pins the root Chronos constraint to 4.2.5;
- updates `nimble.lock` checksums and revisions;
- synchronizes the Nix dependency revision and hash.

Follow-up work threads the cover-traffic fraction through Delivery's builder,
factory, node, and `WakuMix` layers and pins the completed core/plugin APIs.
Factory coverage verifies the configured value reaches the live Mix instance.

The signed PR head is `8a254b7e136bf5ce9660ebf746f2ebe69bd54bd7`.

### Mix-RLN FFI facade

[logos-co/nim-libp2p-mix-rln-ffi#1](https://github.com/logos-co/nim-libp2p-mix-rln-ffi/pull/1) changes the FFI implementation from owning a separate libp2p node to owning a Delivery `WakuNode`:

- removes the FFI-owned switch and separately assembled Mix-RLN provider;
- uses Delivery's switch for Relay, Mix, peer connections, receivers, and lifecycle;
- uses Delivery Relay for membership and proof-metadata coordination;
- keeps the existing C lifecycle, peer, membership, and message APIs;
- retains legacy coordination symbols for ABI compatibility;
- pins Delivery PR #4185 at commit `8a254b7e136bf5ce9660ebf746f2ebe69bd54bd7`;
- aligns the Nimble and hermetic Nix dependency graph with Delivery.

The completed facade also:

- accepts TCP and QUIC listen multiaddresses and selects matching peer addresses;
- updates Mix with the actual bound address when port zero is requested;
- exposes the plugin's real RLN membership index;
- applies cover-rate changes to the running constant-rate scheduler;
- extracts incoming SURBs, sends manual SURB replies, and returns reply payloads
  from synchronous request/reply sends;
- maps the public request timeout into the reply wait;
- verifies these behaviors in one five-node C smoke test on TCP and QUIC.

The signed PR head is `9d8e4c2ef66f68ac6d7545f7ee12ac6910329e30`.

### Logos Mix-RLN module

[logos-co/logos-libp2p-mix-rln#1](https://github.com/logos-co/logos-libp2p-mix-rln/pull/1) updates the Logos module to consume the Delivery-backed FFI:

- pins FFI PR #1 at commit `9d8e4c2ef66f68ac6d7545f7ee12ac6910329e30`;
- removes the host-side RLN publish callback;
- removes the coordination backlog and manual frame-delivery path;
- lets `addMixPeer` create Delivery-managed connections used by Relay coordination and Mix;
- returns synchronous SURB reply bytes to the host;
- exposes a SURB-capable exit-destination send;
- preserves incoming SURBs in both push events and the pull inbox API;
- updates documentation and the multi-node integration test for one Delivery-owned network node.

The module build now generates 21 public methods.

## Pull-request and pin topology

```text
Delivery #4181 ----------------------+
                                     v
Mix-RLN plugin #22 ------------> Delivery #4182 ---> Delivery #4185 <--- Core Mix #58
                                                          |
                                                          | exact commit pin
                                                          v
                                                   FFI facade PR #1
                                                          |
                                                          | exact commit pin
                                                          v
                                                   Logos module PR #1
```

Current PR state:

| Repository | PR | State | Head commit |
| --- | --- | --- | --- |
| Core Mix | [#58](https://github.com/logos-co/nim-libp2p-mix/pull/58) | Draft | `75f8bd5386f08ea909332774ddbeb78599b89d92` |
| Mix-RLN plugin | [#22](https://github.com/logos-co/mix-rln-spam-protection-plugin/pull/22) | Open | `2dee9aaa2214895805fded977543c2770db4dc16` |
| Delivery injection | [#4181](https://github.com/logos-messaging/logos-delivery/pull/4181) | Open | `a9d9bb1380fdfd034f92592b1519b5d7c74fc7cf` |
| Delivery adapter | [#4182](https://github.com/logos-messaging/logos-delivery/pull/4182) | Draft | `5b8fbfedf0e649b518e5af75539c81fbd1710600` |
| Delivery E2E | [#4185](https://github.com/logos-messaging/logos-delivery/pull/4185) | Draft | `8a254b7e136bf5ce9660ebf746f2ebe69bd54bd7` |
| FFI facade | [#1](https://github.com/logos-co/nim-libp2p-mix-rln-ffi/pull/1) | Draft | `9d8e4c2ef66f68ac6d7545f7ee12ac6910329e30` |
| Logos module | [#1](https://github.com/logos-co/logos-libp2p-mix-rln/pull/1) | Draft | This change |

## Validation performed

### Delivery dependency and integration checks

- A clean dependency installation passed the lock audit: 48 of 48 packages matched `nimble.lock`.
- The targeted two-node Mix-RLN Delivery test passed: 1 test run, 1 OK, 0 failed.
- Proof generation, binding rejection, valid verification, metadata propagation, and duplicate rejection were observed at runtime.
- `nimble.lock` JSON, Nix dependency generation, checksums, and `git diff --check` were validated.
- #4185's refreshed CI passed Ubuntu build/tests, macOS build, Windows build, Android, both iOS targets, Docker, lint, API E2E, and nwaku interop.
- The remaining failing `test-macos-15` job is also present on #4181 and #4182 and is not introduced by #4185.

### FFI checks

- The hermetic `.#cbind` build passed against the exact core, Delivery, and
  plugin PR heads.
- The five-node C API smoke passed on TCP and QUIC.
- The smoke verified live cover-rate mutation, RLN membership-index lookup,
  bound-port address updates, Sphinx routing, incoming SURB extraction, manual
  SURB replies, and sender-side reply bytes.
- The plain Mix routing and per-hop RLN routing derivations both passed.
- The FFI pull request's C/C++ and Python CodeQL jobs passed.

### Logos module checks

- `nix build .#lgx` passed against the pinned FFI commit.
- The generated Logos module exposes 21 methods.
- The live single-daemon lifecycle and error-handling test passed.
- The five-daemon multi-node E2E passed without shell-forwarding coordination frames.
- Memberships synchronized through Delivery Relay.
- A real Sphinx+RLN payload reached the receiver byte-for-byte.
- The deterministic payload fixture uses a 1% cover rate. At the 70% protocol
  default, the live scheduler saturated the proof pipeline and exceeded the
  host call timeout.

The repository's pre-existing generated `unit-tests` runner still produces an empty `bin/` derivation. That is separate from the module/FFI build and the successful runtime E2E.

## Outcome

The working stack now demonstrates the intended architecture:

- one Delivery-owned libp2p node per application instance;
- one switch shared by Relay and Mix;
- Mix-RLN injected instead of compiled into a second networking stack;
- RLN coordination transported by Delivery rather than by host callbacks;
- downstream development unblocked through exact PR commit pins;
- end-to-end membership synchronization and Sphinx+RLN delivery proven across multiple processes.

Merging the Delivery PRs is not required to continue development while exact pins are used. It is required before the stack can replace draft commit pins with stable upstream references.

## Remaining work

### Merge and repin the stack

Recommended order:

1. Merge the Mix-RLN dependency-alignment PR and Delivery #4181.
2. Merge core Mix #58.
3. Rebase and merge Delivery #4182.
4. Rebase and merge Delivery #4185.
5. Update the FFI pins to final merged revisions and regenerate its Nimble/Nix pins.
6. Merge the FFI PR, repin the Logos module, and merge the module PR.

The macOS test failure should be investigated independently because it reproduces below this stack.

### Protocol completeness

The current off-chain coordination is intentionally limited:

- membership registration is sequential;
- distributed member-index allocation is not implemented;
- membership-history synchronization for late joiners is not implemented;
- membership gossip is best effort.

These limitations should be resolved before treating the off-chain membership mechanism as production-ready.

### Additional coverage and productization

- Add a full multi-hop Sphinx payload route to Delivery's own integration suite. The downstream five-daemon module E2E already covers this behavior, but #4185 focuses on Delivery coordination and proof handling.
- Decide when the legacy FFI coordination symbols can be deprecated or removed.
- Productize this standalone Logos mixnet module for deployable routing-only hops. It already uses a minimal Delivery node with Relay, Mix, and Mix-RLN; fleet operation still needs discovery/bootstrap and production coordination parameters.
