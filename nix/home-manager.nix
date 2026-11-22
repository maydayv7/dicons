self: {
  config,
  lib,
  pkgs,
  ...
}: let
  cfg = config.services.dicons;
  target = config.wayland.systemd.target;
  unit = "${(pkgs.replaceVarsWith {
    src = ../data/dicons.service.in;
    name = "dicons.service";
    dir = "systemd/user";
    replacements = {
      dicons_executable = lib.getExe cfg.package;
      session_target = target;
    };
  })}/systemd/user/dicons.service";
in {
  options.services.dicons = {
    enable = lib.mkEnableOption "Enable Wayland Desktop Icons";
    package = lib.mkOption {
      type = lib.types.package;
      default = self.packages.${pkgs.stdenv.hostPlatform.system}.default;
      defaultText = lib.literalExpression "inputs.dicons.packages.\${pkgs.stdenv.hostPlatform.system}.default";
      description = "The dicons package to install and run";
    };
    settings = lib.mkOption {
      type = lib.types.attrsOf lib.hm.types.gvariant;
      default = {};
      example = {show-trash = false;};
      description = "Preferences for dicons";
    };
    style = lib.mkOption {
      type = lib.types.nullOr (lib.types.either lib.types.path lib.types.lines);
      default = null;
      example = lib.literalExpression "./dicons.css";
      description = "GTK CSS overrides";
    };
  };

  config = lib.mkIf cfg.enable {
    assertions = [(lib.hm.assertions.assertPlatform "services.dicons" pkgs lib.platforms.linux)];
    home.packages = [cfg.package];
    dconf.settings."dev/maydayv7/dicons" = lib.mkIf (cfg.settings != {}) cfg.settings;
    xdg.configFile = {
      "systemd/user/dicons.service".source = unit;
      "systemd/user/${target}.wants/dicons.service".source = unit;
      "dicons/style.css" = lib.mkIf (cfg.style != null) (
        if builtins.isPath cfg.style || lib.isStorePath cfg.style
        then {source = cfg.style;}
        else {text = cfg.style;}
      );
    };
  };
}
