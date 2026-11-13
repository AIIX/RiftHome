<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: CC-BY-4.0
-->

# Rift Home

Rift Home is a VR desktop for Linux on the Oculus Rift CV1. Your apps open as windows in a room around you, and you
point, click, grab and move them with the Touch controllers.

## Features

- Ordinary Linux apps as windows in the room, moved, scaled, resized, tilted, curved, pinned or set to follow you.
- A Home launcher with search, a card for every running app, and a dock with the keyboard, notifications,
  screenshots, volume and the time.
- HDR video in Firefox and mpv, and cinema mode with the room dimmed.
- Rooms around you: Rift Home's own, your own 360-degree panoramas and photos, or your desktop's wallpapers.
- A virtual keyboard that opens by itself when you click a text field, and notifications from your apps.
- Switching between Rift Home and other VR apps on RiftLinux's runtime.
- Game mode: the controllers as a gamepad for flat-screen games. Steam and Proton games work.
- Recline for bed or sofa: your windows turn to where you look.
- A Settings app for Wi-Fi and wired network, Bluetooth, sound, the time, the room, the pointer and the controller
  buttons, with a lens test to fit the headset.
- Runs as its own login session or on top of your desktop.

## Requirements

- Oculus Rift CV1 with Touch controllers and sensors
- RiftLinux 0.1.5 or later
- Arch (CachyOS, EndeavourOS, Manjaro), Fedora 44 or later, or Ubuntu 26.10 for the packages (other distributions build
  from source)
- NVIDIA graphics with driver 615 is the tested setup; AMD and Intel are untested

## Install

Install RiftLinux first, then download the package for your distribution from the releases page and run:

```
sudo pacman -U rift-home-<version>-1-x86_64.pkg.tar.zst
sudo dnf install ./rift-home-<version>-1.fc44.x86_64.rpm
sudo apt install ./rift-home_<version>-1_amd64.deb
```

The first line is for Arch, the second for Fedora, the third for Ubuntu.

Then choose "Rift Home" on the login screen, or start "Rift Home" from your desktop's app menu.

## Links

- Releases and source: https://github.com/AIIX/RiftHome
- Documentation: https://github.com/AIIX/RiftHome/tree/main/docs
- Getting started: https://github.com/AIIX/RiftHome/blob/main/docs/getting-started.md
- Troubleshooting: https://github.com/AIIX/RiftHome/blob/main/docs/troubleshooting.md
- Known limitations: https://github.com/AIIX/RiftHome/blob/main/docs/known-limitations.md
