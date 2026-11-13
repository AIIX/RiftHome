// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "wayland/SurfaceProbe.hpp"

#include "wayland/ColorManagement.hpp"
#include "wayland/LinuxDmabuf.hpp"

#include "graphics/PixelFormat.hpp"
#include "wayland/wlr.hpp"

namespace RiftHome::Wayland
{

QString BufferInfo::toString() const
{
    const QString size = QStringLiteral("%1x%2").arg(width).arg(height);
    switch (kind) {
    case Kind::None:
        return QStringLiteral("none");
    case Kind::Dmabuf:
        return QStringLiteral("dmabuf %1 modifier 0x%2 %3 %4 plane(s)")
            .arg(Graphics::drmFormatName(format))
            .arg(modifier, 16, 16, QLatin1Char('0'))
            .arg(size)
            .arg(planes);
    case Kind::Shm:
        return QStringLiteral("shm %1 %2").arg(Graphics::drmFormatName(format), size);
    case Kind::Texture:
        return QStringLiteral("texture %1 (client buffer released, format unknown)").arg(size);
    }
    return {};
}

BufferInfo probeBuffer(wlr_surface *surface)
{
    BufferInfo info;
    wlr_buffer *buffer = surface->current.buffer;
    if (!buffer && surface->buffer) {
        buffer = surface->buffer->source;
    }
    if (buffer) {
        wlr_dmabuf_attributes dmabuf{};
        if (LinuxDmabuf::dmabufOf(buffer, &dmabuf)) {
            info.kind = BufferInfo::Kind::Dmabuf;
            info.format = dmabuf.format;
            info.modifier = dmabuf.modifier;
            info.width = dmabuf.width;
            info.height = dmabuf.height;
            info.planes = dmabuf.n_planes;
            return info;
        }
        wlr_shm_attributes shm{};
        if (wlr_buffer_get_shm(buffer, &shm)) {
            info.kind = BufferInfo::Kind::Shm;
            info.format = shm.format;
            info.width = shm.width;
            info.height = shm.height;
            info.planes = 1;
            return info;
        }
    }
    if (surface->buffer) {
        info.kind = BufferInfo::Kind::Texture;
        info.width = surface->current.buffer_width;
        info.height = surface->current.buffer_height;
    }
    return info;
}

Color::SurfaceColorDescription probeColor(wlr_surface *surface)
{
    return ColorManagement::descriptionOf(surface); // Rift Home's own color-management-v1
}

bool usesExplicitSync(wlr_surface *surface)
{
    return wlr_linux_drm_syncobj_v1_get_surface_state(surface) != nullptr;
}

std::optional<QString> SurfaceWatch::update(wlr_surface *surface, bool explicitSyncAvailable)
{
    const BufferInfo buffer = probeBuffer(surface);
    if (buffer.kind == BufferInfo::Kind::None) {
        return std::nullopt;
    }
    const Color::SurfaceColorDescription color = probeColor(surface);
    const bool explicitSync = explicitSyncAvailable && usesExplicitSync(surface);
    if (m_probed && buffer == m_buffer && color == m_color && explicitSync == m_explicitSync) {
        return std::nullopt;
    }
    m_probed = true;
    m_buffer = buffer;
    m_color = color;
    m_explicitSync = explicitSync;

    const Color::PixelKind pixels =
        buffer.kind == BufferInfo::Kind::Texture ? Color::PixelKind::Unknown : Graphics::pixelKind(buffer.format);
    const Color::LayerPlan plan = Color::planLayer(color, pixels, Color::untaggedTransfer());
    return QStringLiteral("| buffer %1 | colour %2 | explicit sync %3 | layer %4")
        .arg(buffer.toString(), color.toString(), explicitSync ? QStringLiteral("yes") : QStringLiteral("no"), plan.toString());
}

} // namespace RiftHome::Wayland
