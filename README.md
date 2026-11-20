# Wayland Desktop Icons

> Show Files on the Desktop

## Features

- [x] Use `XDG_USER_DIR` for the Desktop directory
- [x] Start default application for selected file
- [x] Update icons when files are added, removed, renamed, or edited
- [x] Drag and Drop files from/to the Desktop
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
| Menu or Shift+F10 | Open the context menu for focused icon or desktop             |
| Ctrl+H            | Toggle hidden items on all monitors                           |
| Ctrl+A            | Select all icons                                              |
| Esc               | Clear the selection                                           |
| F2                | Edit the filename. Enter or clicking away saves. Esc cancels. |
| Delete            | Move all selected items to Trash                              |
| Ctrl+Z            | Undo last Delete                                              |

## Configuration

Preferences are stored in GSettings under `dev.maydayv7.dicons`.

On NixOS, enable `programs.dconf.enable = true;`. The following Home Manager example shows the defaults:

```nix
dconf.settings."dev/maydayv7/dicons" = {
  show-trash = true;
  show-thumbnails = true;
  sort-field = "name"; # name/type/modified/size
  sort-direction = "ascending"; # ascending/descending
  folders-first = true;
};
```

**Show Hidden Items** and **Ctrl+H** apply only to the current session.

## Supported Desktops

> See [GTK4 Layer Shell](https://github.com/wmww/gtk4-layer-shell)

Exclusively tested on Hyprland, but should be compatible with any `wlroots` based compositors.

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
nix build
./result/bin/dicons
```

### Development

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
