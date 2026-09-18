{
  description = "Logos Mix Intermediate Module (libp2p + Mix + RLN)";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.0";
    liblogos_rln_module.url = "git+https://github.com/richard-ramos/logos-rln-modules?ref=feat/mix-wire-binding&rev=f501685dd8d65452508cac77db6fae967feec6ef&dir=logos-rln-module";

    # Standalone facade with shared-provider support; PR #2 is under review.
    libp2p-mix-rln.url = "github:logos-co/nim-libp2p-mix-rln-ffi/b2008a262f733c6600c24f29f196d3198de11946";

    # For `nix run .#standalone-e2e`. Kept out-of-tree because they only
    # matter for the runtime e2e; unit tests / library builds don't need them.
    logoscore-cli.url = "github:logos-co/logos-logoscore-cli";
    package-manager.url = "github:logos-co/logos-package-manager";
    # Test-only Relay backend (stable Delivery module v0.2.1).
    delivery-module.url = "github:logos-co/logos-delivery-module/b8b9ac2f4667bc63644b2116f64a07aa30cfd3ef";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    let
      nixpkgs = logos-module-builder.inputs.nixpkgs;
      systems = [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ];

      forEachSystem = f: builtins.listToAttrs (map (system: {
        name = system;
        value = f system;
      }) systems);

      libp2pMixRlnInputs = {
        packages = forEachSystem (system: {
          cbind = inputs.libp2p-mix-rln.packages.${system}.cbind;
        });
      };

      externalLibInputs = {
        libp2p_mix_rln = {
          input = libp2pMixRlnInputs;
          packages.default = "cbind";
        };
      };

      module = logos-module-builder.lib.mkLogosModule {
        src = ./.;
        configFile = ./metadata.json;
        flakeInputs = inputs;
        inherit externalLibInputs;
        tests = {
          dir = ./tests;
        };
      };

      # `nix run .#standalone-e2e`: drives a live logoscore daemon with the
      # module loaded, so it isn't a hermetic flake check — it's an app run
      # as its own CI step. LOGOSCORE_BIN / LGPM_BIN override the vendored
      # binaries when set.
      perSystem = forEachSystem (system:
        let
          pkgs = import nixpkgs { inherit system; };
          e2eRuntime = [
            pkgs.coreutils pkgs.gnugrep pkgs.bash pkgs.iproute2 pkgs.jq
          ];
          lgxDir = "${module.packages.${system}.lgx}";
          rlnLgxDir = "${inputs.liblogos_rln_module.packages.${system}.lgx}";
          lezRlnLgxDir = "${inputs.liblogos_rln_module.inputs.liblogos_lez_rln_module.packages.${system}.lgx}";
          logoscoreBin = "${inputs.logoscore-cli.packages.${system}.default}/bin/logoscore";
          lgpmBin = "${inputs.package-manager.packages.${system}.cli}/bin/lgpm";
          standaloneE2eScript = ./tests/integration_e2e/standalone_e2e.sh;
          standaloneE2eApp = pkgs.writeShellScript "standalone-e2e" ''
            export PATH=${pkgs.lib.makeBinPath (e2eRuntime ++ [ pkgs.xxd ])}:$PATH
            export LIBP2P_MIX_RLN_LGX_DIR=${lgxDir}
            export RLN_LGX_DIR=${rlnLgxDir}
            export LEZ_RLN_LGX_DIR=${lezRlnLgxDir}
            export LOGOSCORE_BIN="''${LOGOSCORE_BIN:-${logoscoreBin}}"
            export LGPM_BIN="''${LGPM_BIN:-${lgpmBin}}"
            exec ${standaloneE2eScript} "$@"
          '';
          multiNodeE2eScript = ./tests/integration_e2e/multi_node_e2e.sh;
          multiNodeE2eApp = pkgs.writeShellScript "multi-node-e2e" ''
            export PATH=${pkgs.lib.makeBinPath (e2eRuntime ++ [ pkgs.xxd ])}:$PATH
            export LIBP2P_MIX_RLN_LGX_DIR=${lgxDir}
            export RLN_LGX_DIR=${rlnLgxDir}
            export LEZ_RLN_LGX_DIR=${lezRlnLgxDir}
            export LOGOSCORE_BIN="''${LOGOSCORE_BIN:-${logoscoreBin}}"
            export LGPM_BIN="''${LGPM_BIN:-${lgpmBin}}"
            exec ${multiNodeE2eScript} "$@"
          '';
          deliveryCoordinationE2eApp = pkgs.writeShellScript "delivery-coordination-e2e" ''
            export PATH=${pkgs.lib.makeBinPath [ pkgs.python3 ]}:$PATH
            export DELIVERY_LGX_DIR=${inputs.delivery-module.packages.${system}.lgx}
            export DELIVERY_COORDINATION_SCRIPT=${./tests/integration_e2e/delivery_coordination.py}
            exec ${multiNodeE2eApp} "$@"
          '';
          deliveryEdgeE2eApp = pkgs.writeShellScript "delivery-edge-coordination-e2e" ''
            export DELIVERY_MODE=edge
            exec ${deliveryCoordinationE2eApp} "$@"
          '';
        in {
          apps = pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
            standalone-e2e = { type = "app"; program = toString standaloneE2eApp; };
            multi-node-e2e = { type = "app"; program = toString multiNodeE2eApp; };
            delivery-coordination-e2e = { type = "app"; program = toString deliveryCoordinationE2eApp; };
            delivery-edge-coordination-e2e = { type = "app"; program = toString deliveryEdgeE2eApp; };
          };
        }
      );

      existingApps = module.apps or {};
      mergedApps = forEachSystem (system:
        (existingApps.${system} or {}) // (perSystem.${system}.apps or {})
      );
    in module // { apps = mergedApps; };
}
