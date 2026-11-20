# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: Apache-2.0
# Rift Home from its release's source (Fedora 44 and later). packaging/make-packages.sh builds the release's .rpm with
# this file and a local source tarball (the Source0 below is the public address).
Name:           rift-home
Version:        0.1.1
Release:        1%{?dist}
Summary:        A VR desktop for the Oculus Rift CV1 on RiftLinux
# The program is Apache-2.0, the documentation CC-BY-4.0, the bundled OpenXR header and the RiftLinux test source BSL-1.0.
License:        Apache-2.0 AND CC-BY-4.0 AND BSL-1.0 AND CC0-1.0 AND LicenseRef-BlendKit-Royalty-Free
URL:            https://github.com/AIIX/RiftHome
Source0:        %{url}/archive/v%{version}/%{name}-%{version}.tar.gz
# The tarball's folder is RiftHome-VERSION (as GitHub's tag archive).
ExclusiveArch:  x86_64

BuildRequires:  cmake >= 3.25
BuildRequires:  ninja-build
BuildRequires:  gcc-c++
BuildRequires:  cmake(Qt6Core) >= 6.11
BuildRequires:  cmake(Qt6Gui)
BuildRequires:  cmake(Qt6DBus)
BuildRequires:  cmake(Qt6Test)
BuildRequires:  cmake(Qt6Quick)
BuildRequires:  cmake(Qt6QuickControls2)
BuildRequires:  cmake(Qt6Network)
BuildRequires:  qt6-qtbase-private-devel
BuildRequires:  pkgconfig(wlroots-0.20)
BuildRequires:  pkgconfig(wayland-server)
BuildRequires:  pkgconfig(wayland-client)
BuildRequires:  pkgconfig(wayland-protocols) >= 1.49
BuildRequires:  pkgconfig(xkbcommon)
BuildRequires:  pkgconfig(libdrm)
BuildRequires:  pkgconfig(pixman-1)
BuildRequires:  pkgconfig(openxr)
BuildRequires:  pkgconfig(vulkan)
BuildRequires:  pkgconfig(polkit-agent-1)
BuildRequires:  pkgconfig(xcb)
BuildRequires:  glslc
BuildRequires:  libglvnd-devel

# RiftLinux's own package (the Rift CV1 driver and OpenXR runtime): Rift Home runs on it.
Requires:       riftlinux
# Applications that X11 programs and the login session need.
Requires:       xorg-x11-server-Xwayland
Requires:       dbus
Requires:       polkit
Recommends:     mpv
Recommends:     xdg-desktop-portal-gtk
Recommends:     NetworkManager
Recommends:     bluez
Recommends:     pulseaudio-utils
Recommends:     libnotify
Suggests:       firefox
Suggests:       kde-cli-tools
Suggests:       steam

%description
Rift Home is a VR desktop for the Oculus Rift CV1 on RiftLinux: a Wayland compositor that shows your Linux apps as
windows in a room around you. It has a launcher, a dock with a keyboard, a Settings app, HDR video and web pages, and
runs as a session from the login screen or on top of your desktop.

%prep
%autosetup -n RiftHome-%{version}

%build
%cmake -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
%cmake_build

%check
export QT_QPA_PLATFORM=offscreen
%ctest

%install
%cmake_install

%files
%license LICENSE LICENSES/*.txt
%{_bindir}/rift-home
%{_bindir}/rift-home-settings
%{_bindir}/rift-home-run
%{_bindir}/rift-home-start
%{_bindir}/rift-home-session
%{_bindir}/rift-home-console
%{_datadir}/applications/org.rifthome.RiftHome.desktop
%{_datadir}/wayland-sessions/rift-home.desktop
%{_datadir}/icons/hicolor/scalable/apps/rift-home.svg
%{_sysconfdir}/xdg/autostart/rift-home-firefox-restore.desktop
%{_datadir}/rift-home/
%{_docdir}/rift-home/

%changelog
* Fri Nov 20 2026 Aditya Mehra <aix.m@outlook.com> - 0.1.1-1
- Films in 4K and in their own shape, a window padlock, steadier windows with smoother edges

* Sun Nov 08 2026 Aditya Mehra <aix.m@outlook.com> - 0.1.0-1
- First release
