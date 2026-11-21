# Wayland Desktop Icons

> Show Files on the Desktop

## Features

- [x] Use `XDG_USER_DIR` for the Desktop directory
- [x] Start default application for selected file
- [x] Update icons when files are added, removed, renamed, or edited
- [x] Drag and Drop files from/to the Desktop
- [x] Drag icons or a selection to free grid cells
- [x] Drop desktop files onto a folder to move them inside
- [x] Mirror on connected monitors
- [x] Rename files and move to Trash
- [x] Right-click context menu, create Templates
- [x] Sort icons by name, modification time, size, or file type
- [x] Image and document thumbnails
- [x] Trash icon
- [ ] Clipboard copy and paste

### Keyboard shortcuts

| Key               | Action                                                        |
| ----------------- | ------------------------------------------------------------- |
| Enter             | Open the focused icon                                         |
| Menu or Shift F10 | Open the context menu for focused icon or desktop             |
| Ctrl H            | Toggle hidden items on all monitors                           |
| Ctrl + or Ctrl =  | Increase icon size (also Ctrl + scroll up)                    |
| Ctrl -            | Decrease icon size (also Ctrl + scroll down)                  |
| Ctrl A            | Select all icons                                              |
| Esc               | Clear the selection                                           |
| F2                | Edit the filename. Enter or clicking away saves. Esc cancels. |
| Delete            | Move all selected items to Trash                              |
| Ctrl Z            | Undo last action                                              |

## Supported Desktops

> See [GTK4 Layer Shell](https://github.com/wmww/gtk4-layer-shell)

Exclusively tested on Hyprland, but should be compatible with any `wlroots` based compositors.

### Layer order

The icons use the Wayland bottom layer with the namespace `dicons`.
They may be hidden by your wallpaper program due to ordering, so you might have to add a layer rule like so (following example is for Hyprland):

```lua
hl.layer_rule({ match = { namespace = "^noctalia-wallpaper$" }, order = 1 })
hl.layer_rule({ match = { namespace = "^dicons$" }, order = 0 })
```

## Getting Started

### Dependencies

- C++20, CMake >=3.16, Ninja, `pkg-config`
- GTK >=4.12 (`gtk4`)
- GLib >=2.74 (`gio-unix-2.0`)
- GTK4 Layer Shell (`gtk4-layer-shell-0`)
- `xdg-terminal-exec` for Open in Terminal
- Option file manager that handles `trash:///`
- Optional system thumbnailers (like [Papers](https://gitlab.gnome.org/GNOME/papers) provides for PDFs)

### Installation

```sh
git clone https://github.com/maydayv7/dicons
cd dicons
# TODO
```

### NixOS

This repo includes a [Home Manager](https://github.com/nix-community/home-manager) module. On NixOS, add this to your configuration:

```nix
programs.dconf.enable = true;
home-manager.sharedModules = [ inputs.dicons.homeManagerModules.default ];
home-manager.users.<name>.services.dicons = {
  enable = true;
  settings.show-trash = false;
};
```

### Standalone systemd user service

The installed package includes `dicons.service`. From a supported Wayland session, enable it for the current user:

```sh
systemctl --user daemon-reload
systemctl --user enable --now dicons.service
```

## Configuration

Preferences are stored in GSettings under `dev.maydayv7.dicons`.
The following shows the defaults:

```ini
[dev/maydayv7/dicons]
folders-first=true
icon-size=48  # 24-128
show-trash=true
show-thumbnails=true
sort-field='name'  # name/type/modified/size
sort-direction='ascending'  # ascending/descending
```

**Show Hidden Items** (Ctrl H) applies only to the current session.

### Settings without dconf

If dconf or its session service is unavailable at startup, preferences are read from `$XDG_CONFIG_HOME/dicons/settings.ini`.
To use this file explicitly, start `dicons` with `GSETTINGS_BACKEND=keyfile`.

```ini
[desktop]
icon-size=48
show-trash=true
```

### Custom styling

Create `style.css` in the same configuration directory and add the GTK CSS rules you want to override.
For example, this rule gives selected icons a fainter highlight:

```css
window.dicons gridview > child:selected .desktop-tile {
  background-color: alpha(@theme_selected_bg_color, 0.35);
}
```

The [default stylesheet](data/style.css) contains other selectors.
Restart `dicons` to apply changes.

## Development

Run `nix develop` to enter the development shell.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build
ctest --test-dir build --output-on-failure
./build/dicons
cmake --install build --prefix "$HOME/.local"
```

Use a separate Debug build to run the GLib tests under ASAN, LSAN and UBSAN.

```sh
cmake -S . -B build/sanitize -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDICONS_SANITIZERS=ON
cmake --build build/sanitize
ASAN_OPTIONS=detect_leaks=1 ctest --test-dir build/sanitize --output-on-failure
```
