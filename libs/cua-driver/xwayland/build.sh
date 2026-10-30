#!/bin/sh
# Builds Xwayland with agent-focus.patch and installs only the Xwayland binary
# into PREFIX/bin. The configuration matches Arch's xorg-xwayland package, so
# every compiled-in path (XKB, fonts, xkbcomp) is the system's.
set -eu

if [ "$#" -ne 1 ] || [ "${1#/}" = "$1" ]; then
    echo "usage: $0 /absolute/prefix" >&2
    exit 2
fi
prefix=$1
version=24.1.13
sha256=173aea3d6f79609164c04528e1c8e4c9b60fcd59391c3c9dad4667297d727fb6

# The binary replaces the system one only for Hyprland, so it must be the
# same release the system ships.
installed=$(pacman -Q xorg-xwayland 2>/dev/null | cut -d' ' -f2 | cut -d- -f1)
if [ -n "$installed" ] && [ "$installed" != "$version" ]; then
    echo "system xorg-xwayland is $installed, this patch is for $version" >&2
    exit 1
fi

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/cua-xwayland.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT HUP INT TERM

curl --fail --location --output "$work/source.tar.xz" \
    "https://www.x.org/releases/individual/xserver/xwayland-$version.tar.xz"
(cd "$work" && echo "$sha256  source.tar.xz" | sha256sum --check --status)
tar -xf "$work/source.tar.xz" -C "$work"
patch -d "$work/xwayland-$version" -p1 < "$here/agent-focus.patch"

# Driver reads this vendor string to know clicks on covered windows are safe.
meson setup "$work/build" "$work/xwayland-$version" \
    --prefix=/usr --buildtype=plain --auto-features=enabled -Db_pie=true \
    -Dipv6=true -Dxvfb=false -Dxdmcp=false -Dxcsecurity=true -Ddri3=true \
    -Dglamor=true -Dlibdecor=true \
    -Dxkb_dir=/usr/share/X11/xkb -Dxkb_output_dir=/var/lib/xkb \
    "-Dvendor_name=The X.Org Foundation (cua-xwayland-agent-focus)"
ninja -C "$work/build" -j "${CUA_BUILD_JOBS:-4}"
install -Dm755 "$work/build/hw/xwayland/Xwayland" "$prefix/bin/Xwayland"
echo "installed $prefix/bin/Xwayland"
