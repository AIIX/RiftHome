<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: CC-BY-4.0
-->

# Troubleshooting

## No picture in the headset

The headset can serve one VR runtime at a time.

1. **Another VR program holds it.** SteamVR (`vrserver`, `vrcompositor`) or another RiftLinux session
   (`monado-service`) is running. Quit it (do not just kill it), then start Rift Home again. Starting from the desktop's
   menu checks for these and tells you in a desktop notification.
2. **You started from the desktop and it was asleep or locked.** Wake and unlock the desktop, then start again.
3. **The runtime is not running or did not start.** The monitor's status page in the login-screen session says "Starting",
   "Running", "Stopped" or "Did not start". If it says "Did not start", check that RiftLinux 0.1.5 or later is
   installed and that the headset works there (see [RiftLinux's documentation](https://github.com/AIIX/RiftLinux)).
4. **SteamVR started while Rift Home was running.** In the login-screen session Rift Home steps aside and gives
   SteamVR the headset. The status says "SteamVR has the headset". When SteamVR ends, Rift Home takes the headset back.

## A password or wallet prompt

An app asks for the password of the wallet or keyring. In Rift Home's session the login unlocks it. If a prompt still
shows, the wallet was probably not unlocked by your login (for example after an automatic login with no password).
Enter the wallet's password with the virtual keyboard. Settings has its own prompt for system changes ("Password required"), which asks for an administrator's
password.

## An app does not open, or opens on the monitor

- Press the app's icon once and wait a moment. A big app (Steam, a browser) takes time. If the window never opens,
  look in the log (see below).
- An app that is already running on the desktop may hand your request to that copy, which then opens on the desktop. Close it on the
  desktop first. This matters most for Firefox and Chromium with "Use my own app data".
- Start apps from Home. An app started from a terminal on the desktop opens on the desktop, not in Rift Home.
- On the login-screen session, the monitor shows a status page with a Terminal button. Start the app from there to
  see its error.

## HDR looks wrong

- Check that the video is HDR and that you play it in Firefox or mpv ([Apps](apps.md)).
- In Firefox on NVIDIA, hardware decoding needs the `nvidia-vaapi-driver` package and `nvidia-drm.modeset=1`.
- In mpv, a film that looks far too bright was played without the film's own metadata. Start it from Home, or give
  mpv `--target-colorspace-hint=yes --target-colorspace-hint-mode=source`.
- The runtime does the HDR mapping. It needs RiftLinux 0.1.5 or later.
- Stremio's own player is SDR: use its external player with mpv for HDR (see [Apps](apps.md#stremio)).

## Everything looks blurry

Fit the headset with the lens test: **Settings, Display & Room, Lenses, Adjust**. Move the headset up and down until
the cross's horizontal lines are clear, then use the lens slider until its vertical lines are clear
([Settings](settings.md#lenses)).

## Text is too small or too big

Change **Apps' scale** under Settings, Display & Room, Sharpness. Open apps change at once. A Chromium-based browser
changes after you restart it. Steam and other X11 apps use **Older apps, Size** and change at their next start. You can
also scale a single window with the scale handle in its bottom-right corner, or bring a window nearer with the grip
and thumbstick.

## Chromium's WebXR does not start

- Use a browser Rift Home started from Home. One that was already running on the desktop has no WebXR set up.
- A Snap browser cannot reach the runtime. Use the Flatpak or the package from your distribution.
- Quit the browser fully and start it again from Home. It keeps its scale and its WebXR setup from its start.
- Check that RiftLinux's runtime is running (the monitor's status page says "Running").

## Slowly growing memory with NVIDIA's driver 615

With NVIDIA's driver 615, Rift Home's memory grows by about 0.6 MB a minute while videos play. It stops when
no video plays. Setting the environment variable `__GL_4718a5=0` for the session stops it. This is an NVIDIA
setting, not Rift Home's. Use it at your own judgement. Set it so that it is in place before Rift Home and the
runtime start, for example with a line `__GL_4718a5=0` in `/etc/environment` (read at every login), then log in
again. If you do not play long videos, you can ignore this.

## Logs and how to report a bug

Rift Home writes its logs to `~/.local/state/rift-home/logs/`. Settings, About shows the Rift Home version and RiftLinux's version.

To report a bug, open an issue at [github.com/AIIX/RiftHome/issues](https://github.com/AIIX/RiftHome/issues) and say:

- what you did, what you expected and what happened;
- how you started Rift Home (login screen or desktop menu);
- the version of Rift Home and RiftLinux (Settings, About);
- your graphics card and driver;
- the newest file from `~/.local/state/rift-home/logs/`. Look it over first: it names your apps and files.

For problems that are about the headset itself (no tracking, a controller not found, a black screen even outside Rift
Home), look at RiftLinux's documentation and issues.
