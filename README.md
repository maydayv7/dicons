# Wayland Desktop Icons

> Show Files on the Desktop

## Features

- [x] Use XDG_USER_DIR for the Desktop directory
- [x] Start default application for selected file
- [x] Update icons when files are added, removed, renamed, or edited
- [x] Drag and Drop files from/to the Desktop
- [x] Mirror on connected monitors
- [x] Rename files and move to Trash
- [x] Right-click context menu, create Templates
- [x] Sort icons by name, modification time, size, or file type
- [ ] File thumbnails

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
