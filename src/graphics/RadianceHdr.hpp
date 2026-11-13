// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QByteArray>
#include <QString>

#include <optional>
#include <vector>

namespace RiftHome::Graphics
{

/*!
 * Radiance HDR images (".hdr", RGBE with run-length scanlines), the format free HDR panoramas come in (Poly Haven's,
 * CC0): Rift Home's room in real light, no image library needed. Pure.
 */
struct HdrImage {
    int width = 0;
    int height = 0;
    std::vector<float> rgb; //!< linear light, row by row, 3 floats a pixel, as the file's (relative) radiance
};

//! The image in @p data, or nothing when it is not a Radiance HDR file this reads (RGBE, -Y H +X W).
std::optional<HdrImage> parseRadianceHdr(const QByteArray &data);
std::optional<HdrImage> loadRadianceHdr(const QString &path);

/*!
 * The scale that puts the image's typical light (its log-average luminance) at @p key: a room's walls and floor come
 * out as a normal scene, its windows and lamps stay brighter than white (HDR highlights the runtime maps).
 */
float exposureFor(const HdrImage &image, float key = 0.18f);

/*!
 * As RGBA 16-bit floats (DRM ABGR16161616F byte order: R, G, B, A halves), times @p scale, halved in size until at most
 * @p maxWidth wide (box filter), alpha 1. @p width and @p height get the result's size.
 */
//! @p highlightLimit (0: none): values above 4 roll off smoothly towards it (a sun or a bulb does not reach 1000s).
QByteArray toHalfRgba(const HdrImage &image, float scale, int maxWidth, int &width, int &height, float highlightLimit = 0.0f);

} // namespace RiftHome::Graphics
