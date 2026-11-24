{
  description = "Wayland Desktop Icons";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";
    treefmt-nix = {
      url = "github:numtide/treefmt-nix";
      inputs.nixpkgs.follows = "nixpkgs";
    };
  };

  outputs = {
    self,
    nixpkgs,
    treefmt-nix,
  }: let
    forLinux = nixpkgs.lib.genAttrs ["x86_64-linux" "aarch64-linux"];
    treefmt = forLinux (system:
      (treefmt-nix.lib.evalModule nixpkgs.legacyPackages.${system} {
        projectRootFile = "flake.nix";
        programs = {
          alejandra.enable = true;
          clang-format.enable = true;
          cmake-format.enable = true;
        };
        settings.formatter.cmake-format.options = [
          "--tab-size=4"
          "--line-width=120"
          "--dangle-parens=true"
          "--enable-sort=false"
        ];
      }).config.build);
  in {
    homeManagerModules.default = import ./packaging/home-manager.nix self;

    packages = forLinux (system: let
      pkgs = nixpkgs.legacyPackages.${system};
    in {
      default = pkgs.stdenv.mkDerivation {
        name = "dicons";
        src = nixpkgs.lib.fileset.toSource {
          root = ./.;
          fileset = nixpkgs.lib.fileset.unions [
            ./CMakeLists.txt
            ./LICENSE
            ./data
            ./packaging/CPack.cmake
            ./src
            ./tests
          ];
        };

        strictDeps = true;
        doCheck = true;

        nativeBuildInputs = with pkgs; [
          cmake
          ninja
          pkg-config
          wrapGAppsHook4
        ];

        buildInputs = with pkgs; [
          glib
          gtk4
          gtk4-layer-shell
        ];

        preFixup = ''
          gappsWrapperArgs+=(--suffix PATH : "${pkgs.xdg-terminal-exec}/bin")
        '';

        meta.mainProgram = "dicons";
      };
    });

    formatter = forLinux (system: treefmt.${system}.wrapper);

    checks = forLinux (system: {
      format = treefmt.${system}.check self;
      sanitizers = self.packages.${system}.default.overrideAttrs (prev: {
        name = "dicons-sanitizers";
        cmakeBuildType = "Debug";
        cmakeFlags = (prev.cmakeFlags or []) ++ ["-DDICONS_SANITIZERS=ON"];
        dontStrip = true;
      });
    });

    devShells = forLinux (
      system: let
        pkgs = nixpkgs.legacyPackages.${system};
      in {
        default = pkgs.mkShell {
          inputsFrom = [self.packages.${system}.default];
          packages = [self.formatter.${system}];
        };
      }
    );
  };
}
