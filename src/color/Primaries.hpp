// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace RiftHome::Color
{

//! Primaries and white point as CIE 1931 xy chromaticities.
struct Chromaticities {
    float rx = 0, ry = 0, gx = 0, gy = 0, bx = 0, by = 0, wx = 0, wy = 0;
    bool operator==(const Chromaticities &) const = default;
};

//! Row-major 3x3: out = M * in.
using Matrix3 = std::array<float, 9>;
constexpr Matrix3 kIdentity3{1, 0, 0, 0, 1, 0, 0, 0, 1};

Chromaticities bt709();
Chromaticities bt2020();

/*!
 * The protocol's named primaries (enum wp_color_manager_v1_primaries, as ITU-T H.273 defines them): sRGB/BT.709, PAL-M,
 * PAL, NTSC (SMPTE 170M), generic film, BT.2020, CIE 1931 XYZ, DCI-P3 (DCI white), Display P3, Adobe RGB. Nothing for
 * other values.
 */
std::optional<Chromaticities> namedPrimaries(uint32_t primariesNamed);

//! Linear RGB in these primaries to CIE XYZ (Y of the white = 1).
Matrix3 rgbToXyz(const Chromaticities &c);

/*!
 * Linear RGB in @p from to linear RGB in @p to. Where the white points differ the colours are adapted to @p to's white
 * (Bradford), as a viewer adapted to each display's white sees them.
 */
Matrix3 conversion(const Chromaticities &from, const Chromaticities &to);

//! The luminance weights of these primaries (the Y row of rgbToXyz): HLG's OOTF uses them.
std::array<float, 3> luminanceWeights(const Chromaticities &c);

} // namespace RiftHome::Color
