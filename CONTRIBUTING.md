<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: CC-BY-4.0
-->

# Contributing to Rift Home

Thank you for helping. Rift Home is a VR desktop for one headset, the Oculus Rift CV1, so most help comes as bug
reports from people with the hardware, and as changes tested on it. This document says what a useful report contains
and what a change needs before it is merged.

## Quick links

- Something does not work? Open a bug report: see [Reporting a problem](#reporting-a-problem).
- An idea? Open a feature request before writing code: see [Proposing a feature](#proposing-a-feature).
- A security problem? Do not open a public issue: see [SECURITY.md](SECURITY.md).
- A change ready? See [Sending a change](#sending-a-change).

Everyone in the project's spaces follows the [Code of Conduct](CODE_OF_CONDUCT.md).

## Reporting a problem

A report we can act on has these parts:

1. What you did, step by step: which app, what you did in it, when the problem appeared.
2. What you expected, and what happened instead.
3. Your setup:
   - the Rift Home version (Settings, About);
   - the RiftLinux version (Settings, About also shows it);
   - how you started Rift Home: from the login screen, or from the desktop's app menu;
   - the distribution (for example CachyOS);
   - the graphics card and driver version (for example NVIDIA RTX 3080, driver 615.78).
4. The log: the newest file in `~/.local/state/rift-home/logs/`. It is called `session-<time>.log` when you started
   from the login screen and `desktop-<time>.log` when you started from the app menu. Attach it, and the apps' logs
   folder next to it, or paste the part around the problem.
5. What you already tried (optional, but it saves a round trip).

The logs contain your user name in file paths. That is fine to share; remove anything you prefer to keep private.

Problems with the headset itself, outside Rift Home, belong to RiftLinux: no picture, no tracking, controllers not
paired or not found. Open those at [github.com/AIIX/RiftLinux/issues](https://github.com/AIIX/RiftLinux/issues).
If you are not sure, start with [Troubleshooting](docs/troubleshooting.md).

## Proposing a feature

Open a feature request first and describe what you want and why. Rift Home has a few fixed rules (below); a short
discussion before the code saves work on both sides.

## Sending a change

### How

1. Fork the repository, make a branch from `main`, and open a pull request against `main`.
2. One change per pull request. For a large change, open a draft pull request early.
3. Fill in the pull request template; its checklist is checked by the maintainer.

### Sign-off (required)

Every commit carries a `Signed-off-by:` line with your name and e-mail (`git commit -s`). With it you certify the
[Developer Certificate of Origin 1.1](https://developercertificate.org/): that you wrote the change or have the right
to submit it under the licence of the files it changes. Only a person can certify it; if a change has several
authors, each of them signs off, and one sign-off matches the commit's author. A check on every pull request lists
commits without it. To add it to commits you already made:

```bash
git rebase --exec 'git commit --amend --no-edit -s' origin/main
git push --force-with-lease
```

### Commit messages

- One summary line, the area first, as in the existing history, for example `xr:`, `wayland:`, `spatial:`,
  `settings:`, `graphics:`, `color:`, `packaging:`, `docs:`.
- Then a body that explains why: what was wrong, how you found it, how you checked the fix.
- Plain words; a reader of the history should understand the change without anything else.

### Code

- Follow the surrounding code: Qt naming, 4-space indent. No formatting changes to code you did not otherwise change.
- Comments say why, in plain words, and stand on their own.
- Every new file has SPDX headers, for example:

  ```cpp
  // SPDX-FileCopyrightText: 2026 Your Name <you@example.org>
  // SPDX-License-Identifier: Apache-2.0
  ```

  Markdown files carry the same two lines in an HTML comment at the top, with `CC-BY-4.0`. YAML files carry them as
  `#` comments. `reuse lint` must pass.
- The build has no new warnings.
- Documentation lives in `docs/*.md` (the user guide) and `README.md`. Change them when behaviour changes.

### Testing

Build, run the tests and check the licence headers:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
ninja -C build
ctest --test-dir build
reuse lint
```

Without a display, run the tests with `QT_QPA_PLATFORM=offscreen`.

Say in the pull request how you tested, and on which hardware. A change to windows, controllers, apps or colour
needs a run in the headset: say which apps you tried and what you saw. Include the version of RiftLinux you used.

## What we do not take

- Other headsets. Rift Home supports the Rift CV1 only, on RiftLinux's runtime.
- A required setup step. Installing the package must be enough.
- Telemetry, phone-home or anything that sends data without the user asking for it.
- Mass reformatting, or unrelated changes in one pull request.

## Licence

The code is under the Apache License 2.0. The documentation and artwork are under Creative Commons Attribution 4.0
International. The files in `third_party/` are under the Boost Software License 1.0. Contributions are accepted under
the licence of the file they change; the sign-off certifies that you may contribute them. See [LICENSE](LICENSE) and
[LICENSES/](LICENSES/).

## RiftLinux

Rift Home runs on [RiftLinux](https://github.com/AIIX/RiftLinux), which has its own repository and its own
contribution rules. A change to the runtime or the headset's drivers goes there.
