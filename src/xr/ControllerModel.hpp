// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>

#include <optional>
#include <vector>

namespace RiftHome::Xr
{

struct ControllerVertex {
    float position[3]; //!< OpenXR grip space, metres
    float normal[3];
    float colour[3]; //!< the material's base colour, linear
    float roughness;
    float metallic;
};

struct ControllerMesh {
    std::vector<ControllerVertex> vertices;
    std::vector<uint32_t> indices;
    QString summary; //!< what was loaded and how it was fitted, for the log
};

/*!
 * A controller model from a binary glTF file (.glb), one a hand: Rift Home's own (ControllerLeft.glb and
 * ControllerRight.glb, designed in Blender). Every mesh's triangles with their material's base colour, roughness and
 * metalness (no textures).
 *
 * The model is drawn as glTF exports it: +Y up the handle, the trigger towards -X, any size. It is fitted into OpenXR's
 * grip space as RiftLinux places the grip on a CV1 Touch (-Z up the handle, -Y ahead, +X to the right), turned 30 degrees
 * about +X so its front (the trigger side) points where the laser goes (the aim ray); scaled to a Touch's 94 mm from the
 * handle's foot to the top of the head; the grip point on the handle's centre line 31 % of that height below the top,
 * the line found from the handle's cross-sections (its tilt kept). Empty
 * when the file cannot be read or has no triangles.
 */
std::optional<ControllerMesh> loadControllerModel(const QString &path);

} // namespace RiftHome::Xr
