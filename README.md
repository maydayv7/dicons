# Wayland Desktop Icons

> Show Files from a Directory on the Desktop

## Features

- [x] Show content from a folder as Icons on the Desktop
- [x] Use xdg_user_dir to use the default desktop directory
- [x] Start default application for the active file
- [x] Update the Icons on changes to the directory (added/removed files)
- [x] Drag and Drop Files from/to the Desktop
- [x] Launching Apps from the Desktop
- [x] Multi-Monitor Support
- [ ] Thumbnails for Images/Documents
- [ ] Sort Rows of List Store based on Name/Type/Date
- [ ] Context-Menu for creating files

## Supported Desktops

> Quoted from [gtk-layer-shell](https://github.com/wmww/gtk-layer-shell)

This application only works on Wayland, and only on Wayland compositors that
support the Layer Shell protocol. Layer shell **is supported** on:

- wlroots based compositors (such as **Sway**)
- Mir-based compositors (some may not enable the protocol by default and require
  `--add-wayland-extension zwlr_layer_shell_v1`)

Layer shell **is not supported** on:

- Gnome-on-Wayland
- Any X11 desktop

## Getting Started

### Dependencies

- CMake >=3.16, Ninja, and pkg-config
- gtk+-3.0
- gio-unix-2.0 (GLib)
- gtk-layer-shell

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
./build/dicons
cmake --install build --prefix "$HOME/.local"
```
