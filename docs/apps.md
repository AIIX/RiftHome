<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: CC-BY-4.0
-->

# Apps

Rift Home lists the apps installed on your computer (their desktop entries) in Home and starts them as Wayland
windows. This page says what Rift Home does for the apps that need special care.

## Your own settings or Rift Home's

Apps you start from Rift Home use one of two sets of settings:

- **Your own** (the default). Apps use your settings and profiles: Firefox's bookmarks and logins, the browser's
  sign-ins, each app's configuration. Close the same app on the desktop first, because two copies cannot share one
  profile.
- **Rift Home's own.** Turn **Settings, Power, Use my own app data** off. Apps then keep their settings in
  `~/.local/share/rift-home/apps`, apart from your desktop's, and your desktop's settings stay untouched. Flatpak
  apps you installed still show up as installed, and a Flatpak app's own data in `~/.var/app` is the same either
  way. The switch applies to apps you start after changing it.

## Firefox

Firefox shows HDR video in Rift Home: it is started with the preferences that turn on HDR and with the video
decoding that suits your graphics card.

- In Rift Home's own profile the preferences stay. If Firefox has no profile there yet, Rift Home makes one the
  way Firefox's first start would.
- In your own profile Rift Home sets them only while its Firefox runs, writes down what was there first, and puts it
  back once Firefox has closed. If a Firefox is open on the desktop with the same profile, Rift Home says so. Close
  it there first.
- A Firefox from Flatpak or Snap always uses your own profile, because their sandboxes ignore Rift Home's own app data.
- Hardware video decoding on NVIDIA needs the `nvidia-vaapi-driver` package and the kernel setting
  `nvidia-drm.modeset=1`. On AMD and Intel, Firefox decides for itself.

## Chromium, Chrome and Brave

Chromium-based browsers (Chromium, Google Chrome, Brave and the like, also from Flatpak) are prepared for you:

- **WebXR.** Rift Home turns on Chromium's OpenXR support and points it at RiftLinux's runtime. Your browser settings stay
  as they are, and you need no flags. A WebXR page can then start a VR session. Chromium's system-call filter
  (seccomp) is off for a browser Rift Home starts: Chromium starts the graphics driver in its XR process, NVIDIA's
  driver makes a call there that the filter refuses, and the session fails ("XRSession creation failed"). Its other
  sandbox stays on. Tested with Chromium 157 from Flathub (`org.chromium.Chromium`). See
  [Known limitations](known-limitations.md).
- **Title bar.** Chromium draws its own title bar and border by default, which would sit inside Rift Home's own window
  frame. Rift Home switches it to "Use system title bar and borders" while it runs. With your own profile, the previous
  value comes back after the browser has closed.
- **Scale.** Rift Home tells Chromium the apps' scale (Settings, Display & Room, Sharpness) when it starts it. Chromium
  keeps that scale until you restart it.
- **Snap.** A Snap's confinement keeps the runtime out of reach, so WebXR is not set up for it.

## mpv

mpv starts with `--target-colorspace-hint=yes --target-colorspace-hint-mode=source` added, so that HDR video is
described by the film's own metadata and shown as it was made. Your mpv configuration is not changed. If you give
the options yourself, Rift Home leaves them as you wrote them. Rift Home was tested with mpv's `gpu-next` video output
(`--vo=gpu-next`).

For the best picture from every mpv, wherever it is started (a file manager, Stremio's external player), put these in
`~/.config/mpv/mpv.conf`. They change how finely mpv draws the film, not what it shows: mpv's sharpest built-in
upscaler for films with fewer pixels than the screen (the cinema screen is 4K), the colour planes enlarged as finely,
decoding on the GPU, and HDR described by the film's own metadata.

```
vo=gpu-next
profile=high-quality
cscale=ewa_lanczossharp
hwdec=auto-safe
target-colorspace-hint=yes
target-colorspace-hint-mode=source
```

## Stremio

Stremio's own player shows video in SDR. For HDR, play the film in an external player: choose Stremio's "Play in
External Player" and pick mpv. The film opens in mpv as a window in the room, in HDR (the headset shows it as HDR).

Rift Home adds two options to an mpv it starts itself, so that the film is described by its own brightness metadata.
An mpv that Stremio starts does not get them: it describes the film by Rift Home's full HDR range instead, and the
film can look darker than it was made. For the same picture as from Home, put the options in your mpv configuration
(`~/.config/mpv/mpv.conf`):

```
target-colorspace-hint=yes
target-colorspace-hint-mode=source
```

## Steam and Proton games

Steam and most games run as X11 apps through XWayland. They draw at the desktop's size by default and are hard to read in the
headset, so Rift Home tells them to draw larger: **Settings, Display & Room, Older apps** (200 percent by default).
It applies when Steam starts, so quit Steam fully and open it again after a change.

Proton games run the same way. VR games started from Rift Home use RiftLinux's runtime, also inside Steam's
container. For Proton VR games Rift Home uses xrizer (an OpenVR layer on OpenXR) when it is installed in
`/opt/xrizer`, so they do not need SteamVR. A game that goes full screen takes the place in front of you.
For gamepad games see game mode in [Using Rift Home](using-rift-home.md).

## Flatpak apps

Flatpak apps start like any other. Rift Home gives their sandbox the Wayland connection and the video decoding settings
they need. A Flatpak Chromium also gets, for that start only, what WebXR needs from RiftLinux's runtime: its socket,
the runtime file and read-only access to the runtime library. You set no Flatpak overrides yourself, and your own are
left alone.

## Password prompts

Apps often ask for a password to open the login's wallet or keyring (Chromium, Proton VPN, Flatpak apps). In Rift
Home's own session the login unlocks it for you: KDE's wallet when it is installed, otherwise GNOME's keyring.
Rift Home's apps use that store, so no app asks for the wallet's password. Run on top of a desktop, Rift Home uses
the one your desktop has already unlocked.
If a prompt does appear, see [Troubleshooting](troubleshooting.md).
