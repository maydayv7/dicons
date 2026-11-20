{
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
}
