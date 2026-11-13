// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "color/ColorDescription.hpp"

#include <QString>

#include <cstdint>
#include <optional>

struct wlr_surface;

namespace RiftHome::Wayland
{

//! The buffer a surface shows now, as the client handed it over.
struct BufferInfo {
    enum class Kind {
        None, //!< no buffer in this state
        Dmabuf,
        Shm,
        Texture, //!< only wlroots' uploaded copy is left (the client's buffer was released)
    };
    Kind kind = Kind::None;
    uint32_t format = 0; //!< DRM FourCC
    uint64_t modifier = 0;
    int width = 0;
    int height = 0;
    int planes = 0;

    bool operator==(const BufferInfo &) const = default;
    QString toString() const;
};

BufferInfo probeBuffer(wlr_surface *surface);
//! The surface's color-management-v1 description; untagged when it has none.
Color::SurfaceColorDescription probeColor(wlr_surface *surface);
//! Whether the client syncs this surface with linux-drm-syncobj timelines.
bool usesExplicitSync(wlr_surface *surface);

/*!
 * Remembers what a surface showed last. update() returns a log line (buffer, colour description, explicit sync and
 * the layer it becomes) when one of them changed, nothing otherwise.
 */
class SurfaceWatch
{
public:
    std::optional<QString> update(wlr_surface *surface, bool explicitSyncAvailable);

private:
    bool m_probed = false;
    BufferInfo m_buffer;
    Color::SurfaceColorDescription m_color;
    bool m_explicitSync = false;
};

} // namespace RiftHome::Wayland
