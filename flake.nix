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
    treefmt = forLinux (system: (treefmt-nix.lib.evalModule nixpkgs.legacyPackages.${system} ./nix/format.nix).config.build);
  in {
    homeManagerModules.default = import ./nix/home-manager.nix self;

    packages = forLinux (system: {
      default = nixpkgs.legacyPackages.${system}.callPackage ./nix/package.nix {};
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
