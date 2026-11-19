{
  description = "Wayland Desktop Icons";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-26.05";

  outputs = {
    self,
    nixpkgs,
  }: let
    forLinux = nixpkgs.lib.genAttrs ["x86_64-linux" "aarch64-linux"];
  in {
    packages = forLinux (
      system: let
        pkgs = nixpkgs.legacyPackages.${system};
      in {
        default = pkgs.stdenv.mkDerivation {
          name = "dicons";
          src = self;
          strictDeps = true;
          doCheck = true;

          nativeBuildInputs = [
            pkgs.cmake
            pkgs.ninja
            pkgs.pkg-config
            pkgs.wrapGAppsHook4
          ];

          buildInputs = [
            pkgs.glib
            pkgs.gtk4
            pkgs.gtk4-layer-shell
          ];

          preFixup = ''
            gappsWrapperArgs+=(--suffix PATH : "${pkgs.xdg-terminal-exec}/bin")
          '';

          meta.mainProgram = "dicons";
        };
      }
    );

    devShells = forLinux (
      system: let
        pkgs = nixpkgs.legacyPackages.${system};
      in {
        default = pkgs.mkShell {
          inputsFrom = [self.packages.${system}.default];
        };
      }
    );
  };
}
