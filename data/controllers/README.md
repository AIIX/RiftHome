<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: CC-BY-4.0
-->

# Controller models

`ControllerLeft.glb` and `ControllerRight.glb`: Rift Home's own design, made from scratch in Blender: a graphite controller with its own button layout and grip on each hand (they are not mirror
images). Rift Home draws them where the Touch controllers are (src/xr/ControllerModel, ControllerRenderer).

How a model is drawn (glTF as Blender exports it): +Y up the handle, the trigger towards -X, any size. Rift Home scales it
to 94 mm from the handle's foot to the top of the head and puts OpenXR's grip point on the handle's centre line, 31 % of
that below the top. Each part keeps its material's base colour, roughness and metalness.

Looked for, first found: `$RIFT_HOME_CONTROLLER_MODELS`, `<state dir>/controllers`, `~/.local/share/rift-home/controllers`,
`/usr/share/rift-home/controllers`, then this folder (development builds).
