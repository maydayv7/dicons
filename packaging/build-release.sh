#!/usr/bin/env bash
set -euo pipefail

tag=${1:?expected a vX.Y.Z release tag}
[[ $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "Invalid release tag: $tag" >&2; exit 1; }
version=${tag#v}
. /etc/os-release
case "$ID" in
    debian)
        apt-get update
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
            cmake ninja-build g++ pkg-config libgtk4-layer-shell-dev libgtk-4-dev libglib2.0-dev \
            dpkg-dev file dbus-daemon xvfb xauth fonts-dejavu-core
        generator=DEB
        ;;
    fedora)
        dnf install -y --setopt=install_weak_deps=False cmake ninja-build gcc-c++ pkgconf-pkg-config gtk4-layer-shell-devel gtk4-devel \
            glib2-devel rpm-build shadow-utils util-linux dbus-daemon xorg-x11-server-Xvfb xorg-x11-xauth dejavu-sans-fonts
        generator=RPM
        ;;
    arch)
        pacman -Syu --noconfirm --needed cmake ninja git glib2-devel gtk4 gtk4-layer-shell \
            hicolor-icon-theme dbus xorg-server-xvfb xorg-xauth ttf-dejavu
        ;;
    *) echo "Unsupported package build distribution: $ID" >&2; exit 1 ;;
esac

useradd -m builder
cd /home/builder
export GTK_A11Y=none GDK_BACKEND=x11 GSK_RENDERER=cairo DICONS_TEST_DISPLAY=1 DICONS_TEST_PRIVATE_CLIPBOARD=1
export CMAKE_BUILD_PARALLEL_LEVEL=${CMAKE_BUILD_PARALLEL_LEVEL:-4}
as_builder=(runuser -u builder --)
with_display=(dbus-run-session -- xvfb-run -a)

if [[ $ID == arch ]]; then
    install -o builder /packaging/arch/PKGBUILD .
    cp /source.tar.gz "dicons-$version.tar.gz"
    read -r DICONS_SOURCE_SHA256 _ < <(sha256sum /source.tar.gz)
    export DICONS_SOURCE_SHA256
    export DICONS_RELEASE_TAG="$tag"
    "${as_builder[@]}" "${with_display[@]}" makepkg --cleanbuild --noconfirm
    cp "dicons-$version-1-x86_64.pkg.tar.zst" /out/
else
    "${as_builder[@]}" tar -xzf /source.tar.gz
    "${as_builder[@]}" cmake -S dicons -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr -DDICONS_PACKAGE_VERSION="$version" -DCPACK_GENERATOR="$generator"
    "${as_builder[@]}" cmake --build build
    "${as_builder[@]}" "${with_display[@]}" ctest --test-dir build --output-on-failure
    "${as_builder[@]}" cpack --config build/CPackConfig.cmake -B packages
    cp packages/*."${generator,,}" /out/
fi
