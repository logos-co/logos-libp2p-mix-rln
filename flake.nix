{
  description = "Logos Mixnet Module (Delivery + Mix + RLN)";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";

    # The Nim FFI facade lives at logos-co/nim-libp2p-mix-rln-ffi. Building
    # it currently requires the two zerokit v2 overrides its README documents
    # (blocked on zerokit PR #436) — see this repo's README for the full
    # command.
    # Pin the Delivery-backed facade directly while its draft PR is pending.
    libp2p-mix-rln.url = "github:logos-co/nim-libp2p-mix-rln-ffi/83b47fcff41fedc5f6f143bac16940212c7ad1cf";

    # For `nix run .#standalone-e2e`. Kept out-of-tree because they only
    # matter for the runtime e2e; unit tests / library builds don't need them.
    logoscore-cli.url = "github:logos-co/logos-logoscore-cli";
    package-manager.url = "github:logos-co/logos-package-manager";
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
          logoscoreBin = "${inputs.logoscore-cli.packages.${system}.default}/bin/logoscore";
          lgpmBin = "${inputs.package-manager.packages.${system}.cli}/bin/lgpm";
          standaloneE2eScript = ./tests/integration_e2e/standalone_e2e.sh;
          standaloneE2eApp = pkgs.writeShellScript "standalone-e2e" ''
            export PATH=${pkgs.lib.makeBinPath (e2eRuntime ++ [ pkgs.xxd ])}:$PATH
            export LIBP2P_MIX_RLN_LGX_DIR=${lgxDir}
            export LOGOSCORE_BIN="''${LOGOSCORE_BIN:-${logoscoreBin}}"
            export LGPM_BIN="''${LGPM_BIN:-${lgpmBin}}"
            exec ${standaloneE2eScript} "$@"
          '';
          multiNodeE2eScript = ./tests/integration_e2e/multi_node_e2e.sh;
          multiNodeE2eApp = pkgs.writeShellScript "multi-node-e2e" ''
            export PATH=${pkgs.lib.makeBinPath (e2eRuntime ++ [ pkgs.xxd ])}:$PATH
            export LIBP2P_MIX_RLN_LGX_DIR=${lgxDir}
            export LOGOSCORE_BIN="''${LOGOSCORE_BIN:-${logoscoreBin}}"
            export LGPM_BIN="''${LGPM_BIN:-${lgpmBin}}"
            exec ${multiNodeE2eScript} "$@"
          '';
        in {
          apps = pkgs.lib.optionalAttrs pkgs.stdenv.hostPlatform.isLinux {
            standalone-e2e = { type = "app"; program = toString standaloneE2eApp; };
            multi-node-e2e = { type = "app"; program = toString multiNodeE2eApp; };
          };
        }
      );

      existingApps = module.apps or {};
      mergedApps = forEachSystem (system:
        (existingApps.${system} or {}) // (perSystem.${system}.apps or {})
      );
    in module // { apps = mergedApps; };
}
