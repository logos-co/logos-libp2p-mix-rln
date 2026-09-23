# Run a standalone Mix intermediate

This guide describes how to run `libp2p_mix_rln_module` as an intermediate
node in the current shared-RLN architecture. An intermediate accepts Sphinx
packets, verifies their Mix RLN proofs, delays and forwards them, and produces
cover traffic. It does not originate application traffic or act as a Mix exit.

The complete tested reference is the
[`shared_delivery_mix`](../tests/integration_e2e/shared_delivery_mix) fixture.
Use that fixture for a local end-to-end deployment; use the ordered procedure
below when integrating one intermediate into an existing network.

## Architecture

One host runs two separate libp2p switches and one shared RLN backend:

```text
                           one host
  +----------------------------------------------------------+
  |                                                          |
  |  standalone Mix switch          Delivery Relay switch    |
  |  - accepts/forwards packets      - transports RLN metadata|
  |  - uses MIX_RLN_ID               - uses RELAY_RLN_ID      |
  |            \                         /                    |
  |             +-- liblogos_rln_module --+                  |
  |                    |                                     |
  |             registry provider                           |
  |                    |                                     |
  |            node payer wallet                             |
  +----------------------------------------------------------+
```

The switches have different peer IDs, listeners, peer pools, and lifecycles.
Loading both modules does not connect them automatically. The host must relay
proof-metadata frames between them as described below.

The shared backend holds two distinct RLN credentials:

```text
(REGISTRY_ID, MIX_RLN_ID)   -> Mix credential and quota
(REGISTRY_ID, RELAY_RLN_ID) -> Relay credential and quota
```

The same payer wallet may fund both registrations. The payer wallet is not the
RLN credential: the backend generates each credential and stores its secret in
the encrypted RLN keystore.

## Before you begin

Obtain the following from the network operator. Do not invent node-local
values for network-wide fields.

| Value | Requirement |
| --- | --- |
| `REGISTRY_ID` | Registry used for the membership. Mix and Relay may use different registries, but this guide uses one. |
| `MIX_RLN_ID` | Exactly 32 bytes encoded as hex; shared by all Mix participants. |
| `RELAY_RLN_ID` | Exactly 32 bytes encoded as hex; different from `MIX_RLN_ID` and shared by Relay participants. |
| Epoch policy | `epoch_size_sec` and `max_epoch_gap` agreed by generators and validators. The current Mix profile uses 10 and 3. |
| Rate limits | Registry-approved limits for the Mix and Relay memberships. They may differ. |
| Metadata topic | Delivery content topic used for Mix proof coordination. Every Mix participant must use the same value. |
| Delivery peers | Reachable Relay peers for the coordination switch. |
| Mix peer records | Complete records for enough intermediates and at least one eligible exit. |

The host also needs:

- Logos Core with a persistent instance/configuration directory.
- A synchronized, funded registry payer wallet.
- A persistent RLN keystore and a securely managed password.
- Built bundles for `liblogos_lez_rln_module`, `liblogos_rln_module`,
  `delivery_module`, and `libp2p_mix_rln_module`.
- Publicly reachable listen addresses when operating outside a local test.

The module can be built and its configuration tests run with:

```sh
nix build .#lgx --no-write-lock-file
nix build .#unit-tests --no-write-lock-file --out-link result-tests
./result-tests/bin/mix_rln_config_tests
```

Use mutually compatible bundle revisions. The RLN module and registry module
must agree on registry layouts, and the Delivery and Mix bundles must expose
the shared-RLN interfaces used here.

This project pins the registry wallet to upstream LEZ
`f0778a4316daa4065ff18a77f4f98706149c240e`, which includes the configured-gas-limit
fee-cap fix from [LEZ #884](https://github.com/logos-blockchain/logos-execution-zone/pull/884).
Build the registry bundle through this project's flake so it inherits that
nested override; building the registry repository independently uses its own
wallet pin. Follow the [registry bundle build instructions](../tests/integration_e2e/shared_delivery_mix/README.md)
and use the resulting `.lgx` as `LEZ_RLN_LGX`.

## 1. Prepare and fund the wallet

Configure the node's wallet home before starting Logos Core. Start the daemon,
open and synchronize its registry wallet, and fund the payer account with
enough native balance for:

- the Relay membership price and transaction fee;
- the Mix membership price and transaction fee; and
- fee headroom for retries or future renewal.

The local fixture creates a separate payer wallet for every host with
`daemon_self_paying`, then calls `wallet_open`, `wallet_sync`, and
`wallet_fund`. Production wallet provisioning is deployment-specific.

Direct registrations made by one registry-module instance use that instance's
payer. To pay from different wallets, use delegated registration with separate
gifters, or run separate registry-module processes. The direct registration
path does not select a payer per call.

## 2. Load the modules

Load these modules into the host:

```text
liblogos_lez_rln_module
liblogos_rln_module
delivery_module
libp2p_mix_rln_module
```

The standalone Mix module depends on `liblogos_rln_module`. Delivery is also
required because the current profile transports proof metadata through Relay.

Loading a module does not create or start either network node.

## 3. Unlock and start the shared RLN backend

Unlock the backend keystore:

```text
liblogos_rln_module.unlock_keystore(RLN_KEYSTORE_PASSWORD)
```

Start the backend with every registry used by Mix or Relay:

```json
{
  "epoch_size_sec": 10,
  "max_epoch_gap": 3,
  "registries": ["<registry-id>"],
  "provision": false
}
```

Conceptually:

```text
liblogos_rln_module.start(configJson)
```

The backend supports per-registry epoch overrides. If Mix and Relay use
different registries and policies, list both registries as objects with their
own `epoch_size_sec` and `max_epoch_gap`. Two scopes in the same registry use
that registry's effective epoch policy.

Only one component should own the shared backend lifecycle. In this guide the
host owns it, so Delivery is configured later with `manage-backend=false`.

## 4. Register separate Relay and Mix memberships

For the LEZ registry, first confirm that the chain's `CLOCK_50` account has a
nonzero timestamp. This system account stores the block timestamp and refreshes
every 50 blocks. The registry reads it to determine membership activation,
grace periods, and expiry; it is separate from the RLN proof epoch policy.
On the tested fresh local chain it starts at zero until the first 50-block
update. Registering before that update can create memberships that appear
expired as soon as the clock advances. Use an initialized chain before
registering either scope; waiting 50 seconds does not establish this condition.

Register the Relay membership:

```text
liblogos_rln_module.register_membership(
  REGISTRY_ID,
  RELAY_RLN_ID,
  [{"key":"rate_limit","value":"100"}]
)
```

Register the Mix membership:

```text
liblogos_rln_module.register_membership(
  REGISTRY_ID,
  MIX_RLN_ID,
  [{"key":"rate_limit","value":"100"}]
)
```

These are two real registration calls. Each creates a new credential; the
scope identifier itself is not a credential or secret.

Record the `membership_hash` returned by each call. Registration is
asynchronous at the chain level, so a successful submission is not sufficient.
Poll:

```text
liblogos_rln_module.get_memberships(REGISTRY_ID)
```

Match records by `membership_hash` and wait until both report `active` or
`grace_period`. Stop if either record reports `failed`.

Provisioning through the backend is recommended because it makes both scopes
and activation checks explicit. Alternatively, after creating the Mix node,
`registerRlnMembership()` delegates registration of the configured Mix scope
to the backend. It does not register the Relay membership.

## 5. Configure the Delivery coordination node

Configure Delivery RLN with the Relay identifier. The preset below assumes the
host already started the backend:

```json
{
  "": {
    "enabled": true,
    "manage-backend": false,
    "registry-id": "<registry-id>",
    "rln-identifier": "<relay-rln-id>",
    "epoch-size-sec": 10,
    "max-epoch-gap": 3
  }
}
```

Expose the preset using the mechanism supported by the Delivery bundle. The
tested fixture writes it to a file and sets `LOGOS_DELIVERY_RLN_PRESETS` before
starting the daemon.

Create the Delivery node as a Relay participant and connect it to existing
Delivery peers. A minimal shape is:

```json
{
  "listenAddress": "0.0.0.0",
  "tcpPort": 9001,
  "relay": true,
  "filter": false,
  "lightpush": false,
  "store": false,
  "peerExchange": false,
  "staticnodes": ["<delivery-relay-peer-multiaddress>"]
}
```

Call `delivery_module.createNode(configJson)`, start it, and subscribe to the
agreed Mix metadata topic:

```text
delivery_module.start()
delivery_module.subscribe(MIX_METADATA_TOPIC)
```

An intermediate does not need to offer Lightpush or Filter services merely to
transport coordination metadata. Deployment policy may enable other Delivery
services independently.

## 6. Configure and start the Mix intermediate

Use an explicit intermediate-only configuration:

```json
{
  "addrs": ["/ip4/0.0.0.0/tcp/9100"],
  "transport": "tcp",
  "maxConnections": 50,
  "maxConnsPerPeer": 2,
  "mix": {
    "allowSend": false,
    "allowExit": false,
    "cover": {
      "rateFraction": 0.7
    }
  },
  "rln": {
    "registryId": "<registry-id>",
    "rlnIdentifierHex": "<mix-rln-id>",
    "epochDurationSeconds": 10,
    "maxEpochGap": 3,
    "userMessageLimit": 100,
    "proofMetadataContentTopic": "<mix-metadata-topic>"
  }
}
```

Then call, in order:

```text
libp2p_mix_rln_module.createNode(configJson)
libp2p_mix_rln_module.start()
```

Keep both role flags false. `allowSend` enables application-originated sends;
`allowExit` enables final application delivery. Neither is required by an
intermediate. Intermediate-only mode still generates cover traffic.

The example `MIX_RLN_ID` in `config.example.json` is the zero-padded encoding
of `mix-rln-spam-protection/v1`. Treat it and the example metadata topic as
integration values until the deployment has assigned identifiers.

For QUIC, select `"transport":"quic"` and use an address such as
`/ip4/0.0.0.0/udp/9100/quic-v1`.

## 7. Connect Mix proof coordination to Delivery

The host must forward outgoing Mix metadata into Delivery and incoming
Delivery metadata into Mix.

For outgoing frames, choose one of these mechanisms:

- Watch the Mix module's `RlnPublishRequested` event, whose payload is
  `{contentTopic, payload}`.
- Poll `drainCoordBacklog()`, which returns and clears
  `[{contentTopic, payloadHex}]`.

Publish every frame through ordinary, non-Mix Delivery sending:

```text
delivery_module.send(frame.contentTopic, frame.payload)
```

For incoming Delivery messages on `MIX_METADATA_TOPIC`, pass the exact bytes
to Mix:

```text
libp2p_mix_rln_module.deliverCoordFrame(
  contentTopic,
  payloadHex
)
```

Important invariants:

- Preserve the content topic and payload exactly.
- Keep this bridge running for the entire Mix-node lifetime.
- Do not route coordination messages through Mix; proof validation must not
  depend on the traffic being validated.
- An emitted event is not proof of successful Relay publication; handle send
  failures in the host.
- Event delivery also leaves the frame in the backlog. If events drive
  publication, drain the corresponding backlog entries without publishing
  them again.
- Queue cross-module work outside the Mix event callback. Re-entering a
  synchronous module method from the callback can block the async FFI loop.

The fixture's polling implementation is in
[`network.py`](../tests/integration_e2e/shared_delivery_mix/network.py).

## 8. Add Mix peers

Obtain complete peer records from the network operator or discovery layer. A
record contains:

```text
peerId
multiaddrs
mixPubKeyHex
libp2pPubKeyHex
```

Add each record:

```text
libp2p_mix_rln_module.addMixPeer(peerRecordJson)
```

Inspect the resulting pool with `listMixPeers()`. The local node's record is
available from `getLocalMixPeerRecord()` and should be distributed to other
Mix participants.

Peer records have no exit capability: Logos senders explicitly name the
destination that terminates the Mix route. `addMixPeer` does not discover peers
or prove their registry membership; those remain host/operator responsibilities.

## 9. Verify readiness

Before advertising the node, verify all of the following:

- `libp2p_mix_rln_module.status()` reports a created node with no
  initialization error.
- `hasRlnMembership()` reports an active usable Mix membership.
- The backend lists active Relay and Mix membership hashes.
- Delivery is connected to its Relay peers and subscribed to the metadata
  topic.
- `listMixPeers()` contains reachable intermediates and an eligible exit.
- The coordination bridge transports frames in both directions.
- The host's advertised Mix multiaddress is reachable externally.
- Epoch size, accepted gap, registry, Mix identifier, and metadata topic match
  the rest of the Mix deployment.

Perform an end-to-end `Required` Delivery send through the Mix network. Confirm
that the exact payload reaches the recipient, proof metadata is exchanged, and
the path does not fall back to direct Delivery if the Mix intermediates stop.

`ok()` and `status()` cover module initialization; they do not prove chain
activation, network connectivity, routing readiness, or remaining quota.
`collectMetrics()` currently returns an empty map.

## 10. Operate and stop safely

Keep all of these available together:

```text
registry provider and synchronized wallet
shared RLN backend and unlocked keystore
Delivery coordination switch and bridge
standalone Mix switch and peer pool
```

Cover traffic consumes Mix quota when packets are sent. Failed or cancelled
sends do not reclaim message-ID allocations. Size the Mix membership rate
limit for forwarded and cover traffic; size the Relay membership separately
for coordination traffic.

Stop components in reverse dependency order:

```text
1. libp2p_mix_rln_module.stop()
2. delivery_module.stop()
3. liblogos_rln_module.stop()
4. stop the registry/wallet services and daemon
```

Do not let Delivery stop a backend that Mix is still using.

## Local end-to-end reference

The repository includes a seven-host fixture with a sender, three standalone
intermediates, a native Mix exit, a Relay service, and a recipient. It provisions
fresh wallets, registers both scopes, transports protected metadata, verifies
exact payload delivery, and checks that `Required` does not fall back to a
direct send.

Provide compatible artifacts:

```sh
export RLN_E2E_ROOT=/path/to/logos-rln-e2e
export LOGOSCORE=/path/to/logoscore
export MIX_LGX=/path/to/mix.lgx
export DELIVERY_LGX=/path/to/delivery.lgx
export RLN_LGX=/path/to/rln.lgx
export LEZ_RLN_LGX=/path/to/lez-rln.lgx
export LEZ_RLN_CHECKOUT=/path/to/compatible/logos-lez-rln
```

Install the scenario into the harness and run it:

```sh
ln -s "$PWD/tests/integration_e2e/shared_delivery_mix" \
  "$RLN_E2E_ROOT/scenarios/shared-delivery-mix"
"$RLN_E2E_ROOT/run.sh" shared-delivery-mix --target local
```

The fixture uses loopback addresses, a test keystore password, integration
identifiers, a local registry, and a cover rate reduced for test cost. Do not
copy those operational values into a public deployment.

## Troubleshooting

### Membership stays pending

Check wallet synchronization, native balance, transaction fees, registry
bounds, and registry-module logs. Track the exact returned `membership_hash`;
scope lookup may be ambiguous while another registration is pending.

### `unknown membership` or proof generation fails

Confirm that the active membership was registered with the same registry and
identifier used by the caller. `MIX_RLN_ID` belongs in the Mix configuration;
`RELAY_RLN_ID` belongs in the Delivery RLN preset.

### Epoch mismatch

The backend, Mix adapter, and Delivery Relay configuration must agree on the
effective epoch policy for their registry. Changing `epoch_size_sec` after a
credential has spent allocations is intentionally rejected; register a fresh
membership for the new size.

### Mix starts but packets do not route

Check `listMixPeers()`, peer multiaddress reachability, peer-record public keys,
and the presence of an eligible exit. Delivery peers and Mix peers are
different pools.

### Proofs validate locally but coordination fails

Check the Delivery subscription, metadata topic equality, Relay connectivity,
and both directions of the coordination bridge. Ensure that event-driven code
is not republishing the same backlog entry.

### Quota is exhausted unexpectedly

Inspect the Mix membership rather than the Relay membership. Forwarding and
cover packets consume the Mix scope's allocations. Failed sends still consume
reserved message IDs. Raise the registered rate limit according to registry
policy or reduce traffic; do not reuse an allocation.
