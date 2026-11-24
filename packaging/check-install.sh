#!/usr/bin/env bash
set -euo pipefail

version=${1#v}
. /etc/os-release
case "$ID" in
    debian)
        apt-get update
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends /out/*.deb libglib2.0-bin
        test "$(dpkg-query -W -f='${Version}' dicons)" = "$version-1"
        ;;
    fedora)
        dnf install -y --setopt=install_weak_deps=False /out/*.rpm
        test "$(rpm -q --qf '%{VERSION}' dicons)" = "$version"
        ;;
    arch)
        pacman -Syu --noconfirm
        pacman -U --noconfirm /out/*.pkg.tar.zst
        test "$(pacman -Q dicons)" = "dicons $version-1"
        ;;
    *) echo "Unsupported package test distribution: $ID" >&2; exit 1 ;;
esac

/usr/bin/dicons --help >/dev/null
test "$(GSETTINGS_BACKEND=memory gsettings get dev.maydayv7.dicons icon-size)" = 48
test -f /usr/share/applications/dev.maydayv7.dicons.desktop
test -f /usr/share/icons/hicolor/scalable/apps/dev.maydayv7.dicons.svg
grep -Fx 'ExecStart="/usr/bin/dicons"' /usr/lib/systemd/user/dicons.service
