{
  lib,
  stdenv,
  cmake,
  ninja,
  pkg-config,
  wrapGAppsHook4,
  glib,
  gtk4,
  gtk4-layer-shell,
  xdg-terminal-exec,
}:
stdenv.mkDerivation {
  name = "dicons";
  src = lib.fileset.toSource {
    root = ../.;
    fileset = lib.fileset.unions [../CMakeLists.txt ../LICENSE ../data ../src ../tests];
  };

  strictDeps = true;
  doCheck = true;

  nativeBuildInputs = [cmake ninja pkg-config wrapGAppsHook4];
  buildInputs = [glib gtk4 gtk4-layer-shell];

  preFixup = ''
    gappsWrapperArgs+=(--suffix PATH : "${xdg-terminal-exec}/bin")
  '';

  meta.mainProgram = "dicons";
}
