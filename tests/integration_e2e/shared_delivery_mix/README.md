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
Delivery switch running Relay for metadata coordination. These nodes join the
Relay mesh directly; they do not enable Lightpush or Filter services. The sender
and recipient remain light clients, supported by the exit and Relay service.

## Tested revisions

The top-level flakes pin their transitive dependencies. Use these revisions for
a reproducible fixture run:

| Component | Revision |
| --- | --- |
| [nim-libp2p-mix-ffi](https://github.com/logos-co/nim-libp2p-mix-ffi) | `c5ece5fb8e5f1a31724c6f7537d0bdc7d0fd802d` |
| [logos-rln-modules](https://github.com/logos-co/logos-rln-modules) | `91ecf7f042b8ffde86e5a1d38c441e988683bf1c` |
| [logos-delivery-module](https://github.com/logos-co/logos-delivery-module) | `7d2e057687c170e93a34f519d682b1d84294b2d1` |
| [logos-rln-e2e](https://github.com/logos-co/logos-rln-e2e) | `f3ec16e31b00d0d44e2e389236e9e642a34480be` |
| [logos-lez-rln](https://github.com/logos-co/logos-lez-rln) | `387d9c1f6b353fa3d44cd10e459ec7729d9318a7` |

The Mix flake pins the shared RLN revision that supports the FFI adapter wire
format and exports its RLN and LEZ bundles for the fixture. The FFI and Delivery
module lock their merged upstream Mix and Delivery changes, so those transitive
revisions do not need separate entries here. The fixture configures
`maxPureLibp2pPeers` to 100 on the native Delivery sender and exit so registered
Mix peers use normal admission.

## Prerequisites

The local target needs Nix with flakes enabled, Rust/Cargo, Docker, and the
standard `git`, `jq`, `curl`, `python3`, `rsync`, and `tar` tools.
Port 3040 must be free before using `--target local`; use the harness's
external-target settings when intentionally attaching to an existing
sequencer.

The host provisioning binaries also require `pkg-config` and the PC/SC
development library. Install the packages for your distribution:

```sh
# Debian or Ubuntu
sudo apt install pkgconf libpcsclite-dev

# Arch Linux
sudo pacman -S pkgconf pcsclite

# Fedora
sudo dnf install pkgconf pcsc-lite-devel
```

Install the RISC Zero Cargo subcommand before building the LEZ guest programs;
this is the same setup used by `logos-lez-rln` CI:

```sh
curl -L https://risczero.com/install | bash
export PATH="$HOME/.risc0/bin:$PATH"
rzup install
cargo risczero --version
```

## Build and run

For a new workspace, clone this repository first. Skip this block if you are
already reading the file from an existing checkout.

```sh
mkdir -p logos-mix-rln-workspace
cd logos-mix-rln-workspace
git clone https://github.com/logos-co/logos-libp2p-mix-rln.git
cd logos-libp2p-mix-rln
```

Continue from the `logos-libp2p-mix-rln` root. The commands below clone the
test harness and local-chain tooling into a sibling dependency directory. If
those checkouts already exist, skip those two `git clone` commands. Nix
fetches the remaining source dependencies, including the pinned
`nim-libp2p-mix` revision.

Building only `.#lgx`, as in the root README, produces the Mix bundle; the
seven-host fixture needs the other three module bundles and Logos Core as well.

```sh
export MIX_RLN_ROOT="$PWD"
export E2E_DEPS_ROOT="$(dirname "$MIX_RLN_ROOT")/logos-mix-rln-e2e-deps"
export RLN_E2E_ROOT="$E2E_DEPS_ROOT/logos-rln-e2e"
export LEZ_RLN_CHECKOUT="$E2E_DEPS_ROOT/logos-lez-rln"
export NIX_CONFIG="experimental-features = nix-command flakes"

mkdir -p "$E2E_DEPS_ROOT"
git clone https://github.com/logos-co/logos-rln-e2e.git "$RLN_E2E_ROOT"
git clone https://github.com/logos-co/logos-lez-rln.git "$LEZ_RLN_CHECKOUT"

git -C "$RLN_E2E_ROOT" checkout --detach f3ec16e31b00d0d44e2e389236e9e642a34480be
git -C "$LEZ_RLN_CHECKOUT" checkout --detach 387d9c1f6b353fa3d44cd10e459ec7729d9318a7

# Build the local-chain guest programs and provisioning tools.
(
  cd "$LEZ_RLN_CHECKOUT/lez-rln"
  cargo risczero build --manifest-path methods/guest/Cargo.toml
  PYO3_PYTHON="$(command -v python3)" cargo build --release --bin run_setup --bin derive_accounts --bin mint_payer --bin fund_account
)

nix build .#lgx --no-write-lock-file --out-link result-mix
nix build .#liblogos_rln_module-lgx --no-write-lock-file --out-link result-rln
nix build .#liblogos_lez_rln_module-lgx --no-write-lock-file --out-link result-lez-rln

DELIVERY_MODULE_REV=7d2e057687c170e93a34f519d682b1d84294b2d1
DELIVERY_FLAKE="github:logos-co/logos-delivery-module/$DELIVERY_MODULE_REV"
nix build "${DELIVERY_FLAKE}#lgx" --no-write-lock-file --out-link result-delivery
nix build "${RLN_E2E_ROOT}#logoscore" --no-write-lock-file --out-link result-logoscore

export LOGOSCORE="$MIX_RLN_ROOT/result-logoscore/bin/logoscore"
export MIX_LGX="$MIX_RLN_ROOT/result-mix/logos-libp2p_mix_rln_module-module-lib.lgx"
export DELIVERY_LGX="$MIX_RLN_ROOT/result-delivery/logos-delivery_module-module-lib.lgx"
export RLN_LGX="$MIX_RLN_ROOT/result-rln/logos-liblogos_rln_module-module-lib.lgx"
export LEZ_RLN_LGX="$MIX_RLN_ROOT/result-lez-rln/logos-liblogos_lez_rln_module-module-lib.lgx"

ln -s "$MIX_RLN_ROOT/tests/integration_e2e/shared_delivery_mix" "$RLN_E2E_ROOT/scenarios/shared-delivery-mix"
"$RLN_E2E_ROOT/run.sh" shared-delivery-mix --target local
```

The harness owns chain provisioning, daemon logs, and artifact installation.
The first run may also compile the sequencer. Use the isolated checkout above:
the harness's local target starts a fresh chain. An already provisioned local
chain can instead be selected with the harness's external-target environment
settings.

The assertions require the exact application payload at the recipient and
metadata reception by all Mix participants. After stopping the standalone
intermediates, an ordinary Delivery control message must still arrive while
`Required` traffic must not. This distinguishes a working Relay network from
a working Mix route. The test cover fraction is deliberately low; it is not a
production privacy setting.

Last verified on 2026-10-12 with the revisions above: the exact Required
payload and protected metadata reached their destinations, and stopping all
standalone intermediates blocked Required traffic while Relay remained usable.
