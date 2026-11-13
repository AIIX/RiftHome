<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: CC-BY-4.0
-->

# Getting started

This page takes you from an installed RiftLinux to your first minutes in Rift Home.

## What you need

- **An Oculus Rift CV1** with its Touch controllers and sensors, set up and working with RiftLinux. If you have not
  done that yet, follow [RiftLinux's getting started](https://github.com/AIIX/RiftLinux) first.
- **RiftLinux 0.1.5 or later.** Rift Home uses the runtime's HDR layers, frosted glass, app switching and its
  start-up. Older versions lack them.
- **A graphics card with a Vulkan driver.** The tested setup is an NVIDIA card with driver 615. AMD and Intel
  graphics are untested.
- **Linux on a 64-bit computer.** There are packages for Arch (CachyOS, EndeavourOS, Manjaro), Fedora 44 and later
  and Ubuntu 26.10. Other distributions build from source (wlroots 0.20, Qt 6.11 or later and wayland-protocols 1.49
  are needed): see the [README](../README.md).
- **Wayland.** Rift Home is its own Wayland session, or it runs on top of your desktop. It does not run as an X11
  session.

## Install

Install RiftLinux first. Then download the package for your distribution from the
[releases page](https://github.com/AIIX/RiftHome/releases) and install it.

Arch, CachyOS, EndeavourOS and Manjaro:

```sh
sudo pacman -U rift-home-<version>-1-x86_64.pkg.tar.zst
```

Fedora 44 and later:

```sh
sudo dnf install ./rift-home-<version>-1.fc44.x86_64.rpm
```

Ubuntu 26.10:

```sh
sudo apt install ./rift-home_<version>-1_amd64.deb
```

The package puts Rift Home's programs in the system, adds a "Rift Home" session to the login screen and a "Rift Home"
entry to the desktop's app menu.

## Start Rift Home

There are two ways. Pick one.

**From the login screen.** Log out, choose the "Rift Home" session (most login screens have a session menu in a
corner) and log in. Rift Home is then your whole session, with no desktop underneath. Your monitor shows a small status
page with the state of the headset and two buttons, "Leave Rift Home" and "Terminal". When you leave Rift Home you
return to the login screen.

**From the desktop.** Start "Rift Home" from the desktop's app menu. Rift Home runs on top of your desktop and
leaves it as it is. The desktop must be awake and unlocked when you start it. When you leave Rift Home you return to
the desktop.

Either way, put the headset on once you see the status "Starting" or "Running" on the monitor or a notification that
Rift Home is starting. Only one VR program can have the headset at a time. If SteamVR or another VR session is
running, quit it first (see [Troubleshooting](troubleshooting.md)).

## Your first minutes

1. **Home.** The launcher opens in front of you: round app icons in rows, a search field above them, page dots below
   and a card for each running app under those. Settings is the first icon.
2. **Point.** Each controller shows a ray. Point at something and it highlights. Pull the trigger to click.
3. **Open an app.** Point at an icon and pull the trigger. The app starts and its window opens in front of you. Home
   closes by itself.
4. **Move the window.** Point at the window and hold the grip (the button under your middle finger). The window
   follows your hand. Push the thumbstick forward to push it farther away, pull it back to bring it nearer. Let go of
   the grip to leave it where it is.
5. **Open the dock.** Press X on the left controller. A bar appears below where you look. It has Home, the keyboard,
   Settings, notifications, the volume and more. Press X again to hide it.
6. **Come back to Home.** Press Y or the menu button on the left controller.
7. **Leave.** Open Settings, go to Power and choose "Leave". Rift Home asks first, then closes the apps in it and
   ends the session. From the login-screen session, the "Leave Rift Home" button on the monitor does the same.

Window places are remembered, so the next start puts your apps back where you left them. Apps you had open can reopen
by themselves too (Settings, Power, "Reopen my apps").

## Where things are stored

| What | Where |
| --- | --- |
| Rift Home's settings, window places and logs | `~/.local/state/rift-home/` (`settings.json`, `window-placements.json`, `logs/`) |
| Apps' own data when Rift Home does not use your profile | `~/.local/share/rift-home/apps` |
| Rooms that come with Rift Home | `/usr/share/rift-home/rooms` |
| Your own rooms | `~/Pictures/RiftHome` |
| Screenshots | `~/Pictures/RiftHome/screenshots` |
| Controller models | `/usr/share/rift-home/controllers` |

## Next

- [Using Rift Home](using-rift-home.md) for everything you can do with windows, Home and the dock.
- [Controllers](controllers.md) for the buttons.
- [Settings](settings.md) for the options.
