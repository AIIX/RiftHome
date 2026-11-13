#!/bin/sh
# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: Apache-2.0
#
# Makes Rift Home's release assets from a source tree (the public tree of a release, or this repository):
#   rift-home-VERSION.tar.gz                  the source, in a folder RiftHome-VERSION (as GitHub's tag archive)
#   rift-home-VERSION-1-x86_64.pkg.tar.zst    the Arch package (Arch, CachyOS, EndeavourOS, Manjaro), built by
#                                             makepkg with packaging/arch/PKGBUILD in a clean Arch container
#   rift-home-VERSION-1.fc44.x86_64.rpm       the Fedora 44 package, built by rpmbuild with packaging/fedora/rift-home.spec
#                                             in a clean Fedora 44 container
#   rift-home_VERSION-1_amd64.deb             the Ubuntu 26.10 package, built by packaging/ubuntu/build-deb.sh in a clean
#                                             Ubuntu 26.10 container
#   PKGBUILD, rift-home.spec                  the recipes (the PKGBUILD with the tarball's checksum)
#   SHA256SUMS
# Each package is then installed in another clean container of its distribution (RiftLinux's package is not in any
# distribution's repositories: Arch counts it as installed, Fedora and Ubuntu get an empty stand-in package of that
# name) and its programs are started once (--help). The .deb is also tried on Debian testing, which only reports.
# A failure stops the run. Needs Docker.
# Usage: make-packages.sh <source dir> <dest dir>
#   The version is the one in CMakeLists.txt's project(). A source dir with .git: its HEAD, as git archive gives it.
set -eu
SRC=$(realpath "${1:?source dir}")
DEST=$(realpath -m "${2:?dest dir}")
[ -f "$SRC/CMakeLists.txt" ] && [ -f "$SRC/packaging/arch/PKGBUILD" ] && [ -f "$SRC/packaging/fedora/rift-home.spec" ] &&
	[ -f "$SRC/packaging/ubuntu/build-deb.sh" ] || { echo "$SRC is not Rift Home's source" >&2; exit 1; }
command -v docker >/dev/null 2>&1 || { echo "docker is needed" >&2; exit 1; }
VER=$(sed -n 's/^project(RiftHome VERSION \([0-9][0-9.]*\) .*/\1/p' "$SRC/CMakeLists.txt")
[ -n "$VER" ] || { echo "no version in $SRC/CMakeLists.txt" >&2; exit 1; }
PKGVER=$(sed -n 's/^pkgver=//p' "$SRC/packaging/arch/PKGBUILD")
[ "$PKGVER" = "$VER" ] || { echo "PKGBUILD's pkgver $PKGVER is not CMakeLists.txt's $VER" >&2; exit 1; }
SPECVER=$(sed -n 's/^Version:[[:space:]]*//p' "$SRC/packaging/fedora/rift-home.spec")
[ "$SPECVER" = "$VER" ] || { echo "rift-home.spec's Version $SPECVER is not CMakeLists.txt's $VER" >&2; exit 1; }
mkdir -p "$DEST"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
TARBALL="rift-home-$VER.tar.gz"

echo "== Source tarball $TARBALL"
if [ -e "$SRC/.git" ]; then
	git -C "$SRC" archive --format=tar.gz --prefix="RiftHome-$VER/" -o "$DEST/$TARBALL" HEAD
else
	tar -C "$(dirname "$SRC")" --transform "s|^$(basename "$SRC")|RiftHome-$VER|" --sort=name --owner=0 --group=0 \
		--numeric-owner -czf "$DEST/$TARBALL" "$(basename "$SRC")"
fi

echo "== Arch package"
# The build image: Arch with the package's build and run dependencies (RiftLinux's is not in Arch's repositories),
# made once and kept.
IMAGE=rifthome-pkg-arch
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	cp "$SRC/packaging/arch/PKGBUILD" "$STAGE/PKGBUILD"
	docker run --name "$IMAGE-make" -v "$STAGE:/in:ro" archlinux:latest bash -c \
		'. /in/PKGBUILD && pacman -Syu --noconfirm --needed base-devel "${makedepends[@]}" ${depends[@]/riftlinux/} >/dev/null &&
		 useradd -m builder' >/dev/null
	docker commit "$IMAGE-make" "$IMAGE" >/dev/null
	docker rm "$IMAGE-make" >/dev/null
fi
A="$STAGE/arch"
mkdir -p "$A"
cp "$DEST/$TARBALL" "$A/"
SUM=$(sha256sum "$A/$TARBALL" | cut -d' ' -f1)
sed -e "s|^source=.*|source=(\"$TARBALL\")|" -e "s|^sha256sums=.*|sha256sums=('$SUM')|" \
	"$SRC/packaging/arch/PKGBUILD" >"$A/PKGBUILD"
docker run --rm -v "$A:/build" "$IMAGE" sh -c \
	'chown -R builder /build && cd /build && su builder -c "makepkg --nodeps --noconfirm -f" >/build/makepkg.log 2>&1;
	 echo "makepkg exit $?"; chown -R '"$(id -u):$(id -g)"' /build' | tail -1
PKG="$A/rift-home-$VER-1-x86_64.pkg.tar.zst"
[ -f "$PKG" ] || { tail -n 30 "$A/makepkg.log" >&2; exit 1; }
cp "$PKG" "$DEST/"
# The release's recipe: the public source address, with this tarball's checksum (GitHub's tag archive is the same
# tree; its own checksum may differ, so packagers building from it check it themselves).
sed -e "s|^sha256sums=.*|sha256sums=('$SUM')|" "$SRC/packaging/arch/PKGBUILD" >"$DEST/PKGBUILD"
echo "  $DEST/$(basename "$PKG")"

echo "== Install test in a clean Arch container"
docker run --rm -v "$DEST:/pkg:ro" archlinux:latest bash -c \
	'pacman -Syu --noconfirm >/dev/null &&
	 pacman -U --noconfirm --assume-installed riftlinux "/pkg/'"$(basename "$PKG")"'" >/dev/null &&
	 QT_QPA_PLATFORM=offscreen rift-home --help >/dev/null && QT_QPA_PLATFORM=offscreen rift-home-settings --help >/dev/null &&
	 for f in /usr/bin/rift-home-start /usr/bin/rift-home-session /usr/bin/rift-home-console /usr/bin/rift-home-run \
	          /usr/share/wayland-sessions/rift-home.desktop /usr/share/applications/org.rifthome.RiftHome.desktop \
	          /usr/share/rift-home/controllers/ControllerLeft.glb /usr/share/rift-home/rooms/twilight-lake.hdr /etc/xdg/autostart/rift-home-firefox-restore.desktop \
	          /usr/share/licenses/rift-home/LICENSE; do test -e "$f" || { echo "missing $f"; exit 1; }; done &&
	 echo "  installed, programs start, files in place"'


echo "== Fedora package"
# The build image: Fedora 44 with the spec's build dependencies, made once and kept.
IMAGE=rifthome-pkg-fedora
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	cp "$SRC/packaging/fedora/rift-home.spec" "$STAGE/rift-home.spec"
	docker run --name "$IMAGE-make" -v "$STAGE:/in:ro" fedora:44 sh -c \
		'dnf -y install rpm-build dnf-plugins-core >/dev/null && dnf -y builddep /in/rift-home.spec >/dev/null' >/dev/null
	docker commit "$IMAGE-make" "$IMAGE" >/dev/null
	docker rm "$IMAGE-make" >/dev/null
fi
F="$STAGE/fedora"
mkdir -p "$F/SOURCES" "$F/SPECS"
cp "$DEST/$TARBALL" "$F/SOURCES/"
cp "$SRC/packaging/fedora/rift-home.spec" "$F/SPECS/"
# rpmbuild runs the spec's %check (ctest). The stand-in for RiftLinux's package (see the install test) is made here,
# where rpmbuild is, and stays out of the release.
docker run --rm -v "$F:/rpm" "$IMAGE" sh -c \
	'rpmbuild -bb --define "_topdir /rpm" /rpm/SPECS/rift-home.spec >/rpm/rpmbuild.log 2>&1; echo "rpmbuild exit $?";
	 printf "Name: riftlinux\nVersion: 0.1.5\nRelease: 1\nSummary: Stand-in for the install test\nLicense: BSL-1.0\nBuildArch: noarch\n%%description\nStand-in.\n%%files\n" >/rpm/stub.spec
	 rpmbuild -bb --define "_topdir /rpm" /rpm/stub.spec >/rpm/stub.log 2>&1 || echo "stand-in package failed";
	 chown -R '"$(id -u):$(id -g)"' /rpm' | tail -2
RPM="rift-home-$VER-1.fc44.x86_64.rpm"
[ -f "$F/RPMS/x86_64/$RPM" ] || { tail -n 30 "$F/rpmbuild.log" >&2; exit 1; }
cp "$F/RPMS/x86_64/$RPM" "$DEST/"
cp "$SRC/packaging/fedora/rift-home.spec" "$DEST/rift-home.spec"
echo "  $DEST/$RPM"

echo "== Install test in a clean Fedora 44 container"
mkdir -p "$STAGE/fedora-stub"
cp "$F"/RPMS/noarch/riftlinux-*.rpm "$STAGE/fedora-stub/"
docker run --rm -v "$DEST:/pkg:ro" -v "$STAGE/fedora-stub:/stub:ro" fedora:44 bash -c \
	'dnf -y --setopt=install_weak_deps=False install /stub/riftlinux-*.rpm "/pkg/'"$RPM"'" >/dev/null &&
	 for p in $(rpm -q --recommends rift-home | cut -d" " -f1) $(rpm -q --suggests rift-home | cut -d" " -f1); do
		[ "$p" != steam ] || continue    # steam is RPM Fusion'"'"'s, not Fedora'"'"'s: only suggested
		dnf -q repoquery --available "$p" | grep -q . || { echo "no package $p"; exit 1; }; done &&
	 QT_QPA_PLATFORM=offscreen rift-home --help >/dev/null && QT_QPA_PLATFORM=offscreen rift-home-settings --help >/dev/null &&
	 for f in /usr/bin/rift-home-start /usr/bin/rift-home-session /usr/bin/rift-home-console /usr/bin/rift-home-run \
	          /usr/share/wayland-sessions/rift-home.desktop /usr/share/applications/org.rifthome.RiftHome.desktop \
	          /usr/share/rift-home/controllers/ControllerLeft.glb /usr/share/rift-home/rooms/twilight-lake.hdr /etc/xdg/autostart/rift-home-firefox-restore.desktop \
	          /usr/share/licenses/rift-home/LICENSE; do test -e "$f" || { echo "missing $f"; exit 1; }; done &&
	 echo "  installed, programs start, files in place"'

echo "== Ubuntu package"
# The build image: Ubuntu 26.10 with the build dependencies (build-deb.sh's), made once and kept.
IMAGE=rifthome-pkg-ubuntu
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
	docker run --name "$IMAGE-make" ubuntu:26.10 sh -c \
		'apt-get update >/dev/null && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
		 build-essential cmake ninja-build pkg-config dpkg-dev file qt6-base-dev qt6-base-private-dev qt6-declarative-dev \
		 libwlroots-0.20-dev libwayland-dev wayland-protocols libxkbcommon-dev libdrm-dev libpixman-1-dev libopenxr-dev \
		 libvulkan-dev glslc libpolkit-agent-1-dev libxcb1-dev libegl-dev libgl-dev >/dev/null' >/dev/null
	docker commit "$IMAGE-make" "$IMAGE" >/dev/null
	docker rm "$IMAGE-make" >/dev/null
fi
U="$STAGE/ubuntu"
mkdir -p "$U/out"
cp "$DEST/$TARBALL" "$U/"
cp "$SRC/packaging/ubuntu/build-deb.sh" "$U/"
# The stand-in for RiftLinux's package (see the install test): made here, stays out of the release.
docker run --rm -v "$U:/build" "$IMAGE" sh -c \
	'sh /build/build-deb.sh /build/'"$TARBALL"' /build/out >/build/build-deb.log 2>&1; echo "build-deb exit $?";
	 mkdir -p /build/stub/DEBIAN &&
	 printf "Package: riftlinux\nVersion: 0.1.5\nArchitecture: all\nMaintainer: Aditya Mehra <aix.m@outlook.com>\nDescription: stand-in for the install test\n" >/build/stub/DEBIAN/control &&
	 dpkg-deb --build /build/stub /build/out/riftlinux-stub.deb >/dev/null;
	 chown -R '"$(id -u):$(id -g)"' /build' | tail -2
DEB="rift-home_${VER}-1_amd64.deb"
[ -f "$U/out/$DEB" ] || { tail -n 30 "$U/build-deb.log" >&2; exit 1; }
cp "$U/out/$DEB" "$DEST/"
echo "  $DEST/$DEB"

echo "== Install test in a clean Ubuntu 26.10 container"
# The same check for every Debian-family image: the stand-in provides riftlinux, apt resolves everything else. $2: names
# of suggested packages that image does not have (Debian has firefox-esr, not firefox).
deb_test() {
	docker run --rm -v "$DEST:/pkg:ro" -v "$U/out:/stub:ro" "$1" bash -c \
		'apt-get update >/dev/null && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
		 /stub/riftlinux-stub.deb "/pkg/'"$DEB"'" >/dev/null &&
		 for p in $(dpkg-query -W -f="\${Recommends}, \${Suggests}" rift-home | tr "," "\n" | sed "s/^ *//; s/[ (|].*//"); do
			case " '"${2:-}"' " in *" $p "*) continue ;; esac
			[ -z "$p" ] || apt-cache policy "$p" | grep -q "Candidate: [^(]" || { echo "no package $p"; exit 1; }; done &&
		 QT_QPA_PLATFORM=offscreen rift-home --help >/dev/null && QT_QPA_PLATFORM=offscreen rift-home-settings --help >/dev/null &&
		 for f in /usr/bin/rift-home-start /usr/bin/rift-home-session /usr/bin/rift-home-console /usr/bin/rift-home-run \
		          /usr/share/wayland-sessions/rift-home.desktop /usr/share/applications/org.rifthome.RiftHome.desktop \
		          /usr/share/rift-home/controllers/ControllerLeft.glb /usr/share/rift-home/rooms/twilight-lake.hdr /etc/xdg/autostart/rift-home-firefox-restore.desktop \
		          /usr/share/doc/rift-home/copyright; do test -e "$f" || { echo "missing $f"; exit 1; }; done &&
		 echo "  installed, programs start, files in place"'
}
deb_test ubuntu:26.10

echo "== The same .deb on Debian testing (reported, not required)"
# Debian testing moves; a failure here is information for the release text, not a failed release.
deb_test debian:testing "firefox steam-installer" || echo "  Debian testing: the .deb did not install or start (see above)"

echo "== SHA256SUMS"
(cd "$DEST" && sha256sum "$TARBALL" "$(basename "$PKG")" "$RPM" "$DEB" PKGBUILD rift-home.spec >SHA256SUMS && cat SHA256SUMS)
