<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: CC-BY-4.0

Keep every section. If an item does not apply, tick it and write "(does not apply: reason)".
-->

## What it does

<!-- One or two sentences. -->

## Why

<!-- The problem it solves, and how you found it. -->

## Issue

Closes #

## How it was tested

<!--
Required. Say on which hardware and system (GPU, distribution, RiftLinux version), and paste the evidence: the end
of the build and of `ctest`, the relevant lines of the log, and for changes you can see in the headset, which apps
you tried and what you saw.
-->

```text
```

## Checklist

- [ ] It builds without new warnings, and `ctest --test-dir build` passes.
- [ ] Changes to windows, controllers, apps or colour were tried in the headset.
- [ ] Documentation (`docs/*.md`, `README.md`) is updated where behaviour changes.
- [ ] A fresh user still gets a working Rift Home from the package alone (no new setup step).
- [ ] Every commit is signed off (`git commit -s`).
- [ ] New and changed files have SPDX headers; `reuse lint` passes.
- [ ] Commit messages: one summary line with the area first, then a body that explains why.
- [ ] It does one thing, for the Rift CV1, and is not in CONTRIBUTING.md's "What we do not take".
