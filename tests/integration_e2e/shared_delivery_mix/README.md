# Delivery Mix with shared RLN

This scenario runs under [logos-rln-e2e](https://github.com/logos-co/logos-rln-e2e)
with its local target. It needs current Logos Core CLI and matching built LGX
bundles for this module, Delivery, the shared RLN module, and the LEZ registry
module. The registry module's `rln-layouts` revision must match the local
`logos-lez-rln` programs; mismatched layouts invalidate the test setup.

The scenario creates seven isolated hosts: a native Delivery Mix sender, three
standalone intermediates, a native Delivery Mix/Lightpush exit, a Relay service,
and a Delivery recipient. Each host has its own funded wallet and backend;
Mix and Relay memberships use separate scopes. Intermediate hosts use a second
Delivery switch for metadata coordination.

```sh
export RLN_E2E_ROOT=/path/to/logos-rln-e2e
export LOGOSCORE=/path/to/logoscore
export MIX_LGX=/path/to/mix.lgx
export DELIVERY_LGX=/path/to/delivery.lgx
export RLN_LGX=/path/to/rln.lgx
export LEZ_RLN_LGX=/path/to/lez-rln.lgx
export LEZ_RLN_CHECKOUT=/path/to/compatible/logos-lez-rln
ln -s "$PWD/tests/integration_e2e/shared_delivery_mix" \
  "$RLN_E2E_ROOT/scenarios/shared-delivery-mix"
"$RLN_E2E_ROOT/run.sh" shared-delivery-mix --target local
```

The harness owns chain provisioning, daemon logs, and artifact installation.
Follow its local-target instructions to build the guest programs, host tools,
and sequencer first. Use an isolated sequencer checkout for this test: the
harness's host target starts a fresh local chain. An already provisioned local
chain can instead be selected with the harness's external-target environment
settings.

The assertions require the exact application payload at the recipient and
metadata reception by all Mix participants. After stopping the standalone
intermediates, an ordinary Delivery control message must still arrive while
`Required` traffic must not. This distinguishes a working Relay network from
a working Mix route. The test cover fraction is deliberately low; it is not a
production privacy setting.

Current execution results are recorded in the repository's `WORK_SUMMARY.md`.
