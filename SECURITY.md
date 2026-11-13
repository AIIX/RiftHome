<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: CC-BY-4.0
-->

# Security policy

How to report a security problem in Rift Home, and what happens after. Please report privately, as below, and not in
a public issue: a public report of an unfixed problem puts every user at risk.

## Supported versions

The latest release. Fixes are made for it and ship in a new release.

## What Rift Home does

Rift Home runs as the user who started it. It has no setuid parts and no system service. These areas matter most:

- Starting apps. Rift Home starts the programs you choose, as you, and passes them its environment.
- The password store. Rift Home forwards your Secret Service (the wallet or keyring) into its own D-Bus session, so
  that apps in the room can ask for stored passwords.
- Chromium's WebXR. It runs with Chromium's seccomp filter turned off, which weakens Chromium's sandbox for that
  browser while it runs in Rift Home.
- The Settings app and its password prompt, which asks for authorisation through polkit.

## In scope

- Rift Home's compositor, the Settings app, the session scripts, the login-screen session file and the package.
- How Rift Home starts apps, forwards the Secret Service and starts Chromium.

## Not in scope

- RiftLinux's runtime, drivers and packages: report those to
  [RiftLinux](https://github.com/AIIX/RiftLinux/blob/main/SECURITY.md).
- The apps you run in Rift Home (Firefox, Chromium, Steam and others), Flatpak and graphics drivers: report those to
  their own projects.
- An attacker who can already run programs as the logged-in user: such a person can already read your password store
  and start apps as you.

## How to report

Use GitHub's private vulnerability reporting: the repository's Security tab, then Report a vulnerability. Only the
maintainer sees it. The repository is [AIIX/RiftHome](https://github.com/AIIX/RiftHome).

Please include what you found and where (file, function or feature), how to reproduce it, what an attacker could do
with it, the Rift Home version (Settings, About) or commit, and whether and how you would like to be credited.

## After you report

- Reports are reviewed as time allows; there are no fixed response times.
- The maintainer may ask for more information.
- Whether a report is treated as a vulnerability, and when it is fixed, is the maintainer's decision.
- Fixes ship in normal releases. Whether a fix gets a release note or an advisory is the maintainer's decision; there
  is no CVE process by default.

Please keep the details private until a fix is released, and tell the maintainer first if you plan to publish
anything about it. If the problem is mentioned publicly, you can ask to be credited. There is no bug bounty.

## Rules for testing

Test only on your own computer, headset and data. No testing against anyone else's systems or devices, no
denial-of-service testing against anything but your own machine, no social engineering. Stop once the problem is
shown, and do not keep or share data you reached. This policy is not a legal agreement and authorises nothing that
is otherwise unlawful.
