// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "color/ColorDescription.hpp"

#include <QString>

#include <cstdint>

namespace RiftHome::Graphics
{

//! The DRM FourCC as its four characters (e.g. "AB30"), as wlroots and drm_info print it.
QString drmFormatName(uint32_t fourcc);

//! The sample type of a DRM format; Unknown for formats Rift Home does not take (YUV among them, for now).
Color::PixelKind pixelKind(uint32_t fourcc);

//! Whether the format carries alpha (ARGB) or not (XRGB: the layer is opaque).
bool hasAlpha(uint32_t fourcc);

} // namespace RiftHome::Graphics
