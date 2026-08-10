{
  description = "Logos Mixnet Module (libp2p + Mix + RLN)";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";

    # Placeholder: this input MUST point at a Nim FFI facade that exposes
    # nim-libp2p, nim-libp2p-mix and mix-rln-spam-protection-plugin through a
    # C header (lib/libp2p_mix_rln.h) and shared object (libp2p_mix_rln.{so,dylib,dll}),
    # analogous to how logos-libp2p-module consumes nim-libp2p's `cbind` package.
    #
    # That facade does not exist yet — creating it is the next task after this
    # scaffold lands. Track: https://github.com/logos-co/logos-libp2p-mix-rln/issues
    #
    # Until then, this flake will fail to evaluate. Comment out the module {} block
    # below to build only metadata/tests/config artifacts.
    libp2p-mix-rln.url = "github:logos-co/nim-libp2p-mix-rln";
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
