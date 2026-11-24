#!/usr/bin/env bash
# Run on x86-64 Debian 13
# Arguments: version, type-2 runtime, output directory
set -euo pipefail
version=${1:?version}
runtime=$(realpath "${2:?runtime}")
out=$(realpath "${3:?output directory}")
src=$(cd "$(dirname "$0")/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
cd "$work"
echo "2fca8b443c92510f1483a883f60061ad09b46b978b2631c807cd873a47ec260d  $runtime" | sha256sum -c -
cmake -S "$src" -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DDICONS_APPIMAGE=ON -DBUILD_TESTING=OFF
cmake --build build --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
root=$work/AppDir
lib=$root/usr/lib
share=$root/usr/share
DESTDIR="$root" cmake --install build --prefix /usr --strip
rm "$share/systemd/user/dicons.service"
cp build/gschemas.compiled "$root/usr/bin/"
install -m755 "$src/packaging/appimage/AppRun" "$root/AppRun"
id=dev.maydayv7.dicons
ln -s "usr/share/applications/$id.desktop" "$root/$id.desktop"
ln -s "usr/share/icons/hicolor/scalable/apps/$id.svg" "$root/$id.svg"
ln -s "$id.svg" "$root/.DirIcon"
printf 'Categories=Utility;\n' >> "$share/applications/$id.desktop"

multiarch=$(gcc -print-multiarch)
modules=/usr/lib/$multiarch
install -Dm755 "$src/packaging/appimage/gio-launch-desktop" "$root/usr/libexec/gio-launch-desktop"
cp "$modules/glib-2.0/gio-launch-desktop" "$root/usr/bin/"
pixbuf=$modules/gdk-pixbuf-2.0/2.10.0
mkdir -p "$lib/gio"
cp "$modules/gio/modules/libdconfsettings.so" "$lib/gio/"
cp "$pixbuf"/loaders/*.so "$lib/"
sed -E 's@^"[^"]*/([^"/]+\.so)"$@"\1"@' "$pixbuf/loaders.cache" > "$share/loaders.cache"
cp -a /usr/share/icons/Adwaita "$share/icons/"
cp /usr/share/icons/hicolor/index.theme "$share/icons/hicolor/"
cp -a /usr/share/mime "$share/"
cp -a /usr/lib/locale "$lib/"
cp -a /usr/share/X11/xkb "$share/"
cp /usr/share/glib-2.0/schemas/*.xml "$share/glib-2.0/schemas/"
glib-compile-schemas --strict "$share/glib-2.0/schemas"

ldd "$root/usr/bin/dicons" "$lib/gio/libdconfsettings.so" "$lib"/*.so > libraries
if grep -F 'not found' libraries; then exit 1; fi
awk '$2 == "=>" && $3 ~ /^\// {print $3}' libraries | sort -u > paths
while IFS= read -r path; do cp -L "$path" "$lib/"; done < paths
cp -L /lib64/ld-linux-x86-64.so.2 "$root/usr/bin/"

mkdir -p "$share/doc"
install -Dm644 "$src/packaging/appimage/runtime.LICENSE" "$share/licenses/appimage-runtime/LICENSE"
cp -a /usr/share/common-licenses "$share/"
cp -L --parents /usr/share/doc/*/copyright "$root"
mksquashfs "$root" payload -noappend -all-root -comp zstd -no-progress
image="$out/dicons-$version-x86_64.AppImage"
cat "$runtime" payload > "$work/dicons.AppImage"
install -m755 "$work/dicons.AppImage" "$image"
