#!/bin/sh
# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: Apache-2.0
#
# Builds Rift Home's .deb (Ubuntu 26.10; Debian testing is tried by make-packages.sh) from the release's source
# tarball. Runs inside the build container that packaging/make-packages.sh makes; by hand, on Ubuntu with the build
# dependencies installed (see make-packages.sh for the list).
# Usage: build-deb.sh <source tarball> <out dir>
#   The tarball's folder is RiftHome-VERSION. The package is <out dir>/rift-home_VERSION-1_amd64.deb.
# Run as root (the build container): the files in the package are owned by root, so no fakeroot.
# Why no debhelper: one binary package from `cmake --install`, so the control file is written here, and the
# library dependencies are computed by dpkg-shlibdeps from the built programs.
set -eu
TARBALL=$(realpath "${1:?source tarball}")
OUT=$(realpath -m "${2:?out dir}")
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
tar -C "$W" -xzf "$TARBALL"
SRC=$(echo "$W"/RiftHome-*)
VER=${SRC##*/RiftHome-}
mkdir -p "$OUT"

cmake -S "$SRC" -B "$W/build" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=/usr \
	-DCMAKE_INSTALL_SYSCONFDIR=/etc >"$W/cmake.log" 2>&1 || { tail -n 30 "$W/cmake.log" >&2; exit 1; }
cmake --build "$W/build" >"$W/build.log" 2>&1 || { tail -n 30 "$W/build.log" >&2; exit 1; }
# The same check the Arch and Fedora packages run.
QT_QPA_PLATFORM=offscreen ctest --test-dir "$W/build" --output-on-failure >"$W/ctest.log" 2>&1 || { tail -n 30 "$W/ctest.log" >&2; exit 1; }
tail -n 3 "$W/ctest.log" | head -n 1

P="$W/pkg"
DESTDIR="$P" cmake --install "$W/build" >/dev/null
# Debian's way for licences: the copyright file, with the texts it names beside it.
D="$P/usr/share/doc/rift-home"
install -m 644 "$SRC/LICENSE" "$D/LICENSE"
install -d "$D/LICENSES"
install -m 644 "$SRC"/LICENSES/*.txt "$D/LICENSES/"
cat >"$D/copyright" <<COPY
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Rift Home
Upstream-Contact: Aditya Mehra <aix.m@outlook.com>
Source: https://github.com/AIIX/RiftHome

Files: *
Copyright: 2026 Aditya Mehra <aix.m@outlook.com>
License: Apache-2.0

Files: docs/* README.md data/controllers/*.glb data/rift-home.svg
Copyright: 2026 Aditya Mehra <aix.m@outlook.com>
License: CC-BY-4.0

Files: third_party/*
Copyright: see the SPDX headers in the files
License: BSL-1.0

Files: data/rooms/night-sky.hdr
Copyright: ambientCG (https://ambientcg.com)
License: CC0-1.0

Files: data/rooms/twilight-lake.hdr data/rooms/mountain-sunset.hdr
Copyright: EB Adventure Photoscans
License: LicenseRef-BlendKit-Royalty-Free
 BlendKit's royalty free licence (https://www.blendkit.com/docs/licenses/): not to be re-sold as assets.

License: Apache-2.0 and CC-BY-4.0 and BSL-1.0 and CC0-1.0 and LicenseRef-BlendKit-Royalty-Free
 The texts are in /usr/share/doc/rift-home/LICENSES/.
COPY
# No changelog of its own: the release notes are on the project's page.
find "$P" -type d -exec chmod 755 {} +
strip --strip-unneeded "$P/usr/bin/rift-home" "$P/usr/bin/rift-home-settings"

# Library dependencies, from the programs (dpkg-shlibdeps wants a debian/control in the folder it runs in).
mkdir -p "$W/shl/debian"
printf 'Source: rift-home\n\nPackage: rift-home\nArchitecture: any\n' >"$W/shl/debian/control"
(cd "$W/shl" && dpkg-shlibdeps -O -e"$P/usr/bin/rift-home" -e"$P/usr/bin/rift-home-settings" >"$W/shlibs.out" 2>"$W/shlibs.err") ||
	{ cat "$W/shlibs.err" >&2; exit 1; }
LIBDEPS=$(sed -n 's/^shlibs:Depends=//p' "$W/shlibs.out")
[ -n "$LIBDEPS" ] || { echo "dpkg-shlibdeps found no dependencies" >&2; exit 1; }
# What the programs start at run time: RiftLinux (its package is named riftlinux), Xwayland for X11 apps, dbus-run-session
# (the session start commands), polkit (Settings' password prompt). The QML modules Settings loads are separate
# packages on Debian, not libraries the programs link.
DEPENDS="$LIBDEPS, riftlinux, xwayland, dbus-bin, polkitd, qml6-module-qtquick, qml6-module-qtquick-controls, qml6-module-qtquick-effects, qml6-module-qtquick-layouts, qml6-module-qtquick-templates, qml6-module-qtquick-window"

mkdir -p "$P/DEBIAN"
SIZE=$(du -sk "$P" | cut -f1)
cat >"$P/DEBIAN/control" <<CTRL
Package: rift-home
Version: $VER-1
Architecture: amd64
Maintainer: Aditya Mehra <aix.m@outlook.com>
Installed-Size: $SIZE
Depends: $DEPENDS
Recommends: mpv, xdg-desktop-portal-gtk, network-manager, bluez, pulseaudio-utils, libnotify-bin
Suggests: firefox, kde-cli-tools, steam-installer
Section: x11
Priority: optional
Homepage: https://github.com/AIIX/RiftHome
Description: VR desktop for the Oculus Rift CV1 on RiftLinux
 Rift Home is a VR desktop for the Oculus Rift CV1 on RiftLinux: a Wayland
 compositor that shows your Linux apps as windows in a room around you. It has
 a launcher, a dock with a keyboard, a Settings app, HDR video and web pages,
 and runs as a session from the login screen or on top of your desktop.
CTRL
# The autostart file is in /etc: dpkg keeps changes made to it.
echo /etc/xdg/autostart/rift-home-firefox-restore.desktop >"$P/DEBIAN/conffiles"
dpkg-deb --build "$P" "$OUT/rift-home_${VER}-1_amd64.deb" >/dev/null
echo "$OUT/rift-home_${VER}-1_amd64.deb"
