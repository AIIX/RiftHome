<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: CC-BY-4.0
-->

# Known limitations

This is what this release does not do.

## Hardware

- **Oculus Rift CV1 with Touch controllers only.** Controllers of other headsets come in a later release. Rift Home
  has been tried on an Oculus Rift CV1 with Touch and sensors and an NVIDIA graphics card (driver 615). AMD and Intel
  graphics are untested.
- **Other OpenXR runtimes are untested.** Rift Home is made for RiftLinux's runtime.

## RiftLinux

- **RiftLinux 0.1.5 or later is needed.** HDR layers, frosted glass, app switching and the start of the runtime
  are RiftLinux's, and older versions do not have them.

## Graphics driver

- **NVIDIA memory growth.** With NVIDIA's driver 615, memory grows slowly while videos play. There is a workaround:
  see [Troubleshooting](troubleshooting.md).

## Apps

- **Chromium's WebXR runs with Chromium's system-call filter (seccomp) off.** NVIDIA's graphics driver, which
  Chromium starts in its XR process, needs a call the filter refuses. The browser's other sandbox stays on. It applies
  to a Chromium-based browser that Rift Home starts.
- **A running Chromium keeps its scale until it is restarted.** Changing "Apps' scale" does not change a browser that
  is already open.
- **Stremio's own player is SDR.** For HDR, play through its external player with mpv (see [Apps](apps.md#stremio)).
- **A Snap browser has no WebXR.** Its confinement keeps the runtime out of reach.

## Session and system

- **Wayland only.** Rift Home is a Wayland session. It does not run as an X11 session. X11 apps run through
  XWayland.
- **Packages for three distributions.** There are packages for Arch (and CachyOS, EndeavourOS, Manjaro), Fedora 44
  and later and Ubuntu 26.10. Other distributions build from source, which needs wlroots 0.20, Qt 6.11 and
  wayland-protocols 1.49. See the [README](../README.md).
- **The keyboard is a US layout.** The virtual keyboard has one layout. A physical keyboard uses the layout your
  system is set to.
