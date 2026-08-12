{
  description = "Logos Mixnet Module (libp2p + Mix + RLN)";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";

    # The Nim FFI facade lives at logos-co/nim-libp2p-mix-rln. Not yet pushed
    # to the org — override on the CLI with `--override-input libp2p-mix-rln
    # path:/path/to/nim-libp2p-mix-rln` (and the two zerokit overrides that
    # repo's README documents) until it's public.
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
