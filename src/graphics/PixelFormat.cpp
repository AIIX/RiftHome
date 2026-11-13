// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "graphics/PixelFormat.hpp"

#include <drm_fourcc.h>

namespace RiftHome::Graphics
{

QString drmFormatName(uint32_t fourcc)
{
    QString name;
    for (int i = 0; i < 4; ++i) {
        const char c = char((fourcc >> (8 * i)) & 0xff);
        name += (c >= 0x20 && c < 0x7f) ? QChar::fromLatin1(c) : QChar(u'?');
    }
    return name.trimmed();
}

Color::PixelKind pixelKind(uint32_t fourcc)
{
    switch (fourcc) {
    case DRM_FORMAT_XRGB8888:
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_XBGR8888:
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_RGBX8888:
    case DRM_FORMAT_RGBA8888:
    case DRM_FORMAT_BGRX8888:
    case DRM_FORMAT_BGRA8888:
        return Color::PixelKind::Unorm8;
    case DRM_FORMAT_XRGB2101010:
    case DRM_FORMAT_ARGB2101010:
    case DRM_FORMAT_XBGR2101010:
    case DRM_FORMAT_ABGR2101010:
    case DRM_FORMAT_RGBX1010102:
    case DRM_FORMAT_RGBA1010102:
    case DRM_FORMAT_BGRX1010102:
    case DRM_FORMAT_BGRA1010102:
    case DRM_FORMAT_P010: // 10-bit YUV 4:2:0 video (Firefox's HDR video, VA-API): converted to RGB in the window pass
        return Color::PixelKind::Unorm10;
    case DRM_FORMAT_XRGB16161616F:
    case DRM_FORMAT_ARGB16161616F:
    case DRM_FORMAT_XBGR16161616F:
    case DRM_FORMAT_ABGR16161616F:
        return Color::PixelKind::Float16;
    default:
        return Color::PixelKind::Unknown;
    }
}

bool hasAlpha(uint32_t fourcc)
{
    switch (fourcc) {
    case DRM_FORMAT_ARGB8888:
    case DRM_FORMAT_ABGR8888:
    case DRM_FORMAT_RGBA8888:
    case DRM_FORMAT_BGRA8888:
    case DRM_FORMAT_ARGB2101010:
    case DRM_FORMAT_ABGR2101010:
    case DRM_FORMAT_RGBA1010102:
    case DRM_FORMAT_BGRA1010102:
    case DRM_FORMAT_ARGB16161616F:
    case DRM_FORMAT_ABGR16161616F:
        return true;
    default:
        return false;
    }
}

} // namespace RiftHome::Graphics
