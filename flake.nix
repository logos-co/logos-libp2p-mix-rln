{
  description = "Logos Mix Intermediate Module (libp2p + Mix + RLN)";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder/0.3.0";
    liblogos_rln_module.url = "git+https://github.com/richard-ramos/logos-rln-modules?ref=feat/mix-wire-binding&rev=4f1f610a05d6108a88b6b9a4f365f6830365ae21&dir=logos-rln-module";
    # Upstream wallet includes configurable gas limits and the matching fee cap.
    liblogos_rln_module.inputs.liblogos_lez_rln_module.inputs.logos-execution-zone.url =
      "github:logos-blockchain/logos-execution-zone/f0778a4316daa4065ff18a77f4f98706149c240e";

    # Pin the standalone facade from upstream main.
    libp2p-mix-rln.url = "github:logos-co/nim-libp2p-mix-ffi/5767c6c2a121ad5ea7052822a5aaa8260b92f2e3";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    let
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

    in module;
}
