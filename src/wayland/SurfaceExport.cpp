// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#include "wayland/SurfaceExport.hpp"

#include "graphics/PixelFormat.hpp"
#include "wayland/LinuxDmabuf.hpp"
#include "wayland/Logging.hpp"
#include "wayland/Server.hpp"
#include "wayland/SurfaceProbe.hpp"
#include "wayland/wlr.hpp"
#include "xr/LayerBridge.hpp"

#include <drm_fourcc.h>
#include <cmath>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>

namespace RiftHome::Wayland
{

namespace
{

//! The fence of whoever writes the dmabuf now (implicit sync); -1 where the driver cannot tell.
int exportImplicitFence(int dmabufFd)
{
    dma_buf_export_sync_file request{};
    request.flags = DMA_BUF_SYNC_READ;
    request.fd = -1;
    if (ioctl(dmabufFd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &request) != 0) {
        return -1;
    }
    return request.fd;
}

} // namespace

SurfaceExport::SurfaceExport(Server &server, wlr_surface *surface, uint64_t id)
    : m_server(server)
    , m_surface(surface)
    , m_id(id)
{
    m_server.registerSurface(surface, id, this);
    m_commit.connect(&surface->events.commit, [this](void *) {
        onCommit();
    });
    m_destroy.connect(&surface->events.destroy, [this](void *) {
        delete this;
    });
}

SurfaceExport::~SurfaceExport()
{
    m_server.unregisterSurface(m_surface, m_id);
}

void SurfaceExport::tellScale()
{
    if (!m_server.scalesPerSurface()) {
        return; // headless and nested: the output's scale does it
    }
    if (!wlr_xdg_surface_try_from_wlr_surface(wlr_surface_get_root_surface(m_surface))) {
        return;
    }
    const double scale = m_server.options().appScale;
    wlr_fractional_scale_v1_notify_scale(m_surface, scale);
    wlr_surface_set_preferred_buffer_scale(m_surface, int(std::ceil(scale)));
    m_scaleTold = scale;
}

void SurfaceExport::onCommit()
{
    if (m_scaleTold != m_server.options().appScale) {
        tellScale();
    }
    if (m_surface->current.committed & WLR_SURFACE_STATE_BUFFER) {
        publishCurrent();
    }
}

void SurfaceExport::publishCurrent()
{
    Xr::LayerBridge *bridge = m_server.layerBridge();
    wlr_buffer *buffer = m_surface->current.buffer;
    // A buffer wlroots made no texture of (P010 video: its renderer cannot) is still passed on: the XR side imports it.
    if (!bridge || (!buffer && !wlr_surface_has_buffer(m_surface))) {
        return;
    }
    // Windows and their menus, Wayland's and X11's (Steam, Wine, games: X11 windows showed an empty
    // frame without them);
    // cursors and drag icons are neither.
    wlr_surface *root = wlr_surface_get_root_surface(m_surface);
    if (!wlr_xdg_surface_try_from_wlr_surface(root) && !wlr_xwayland_surface_try_from_wlr_surface(root)) {
        return;
    }

    auto handoff = std::make_unique<Xr::BufferHandoff>();
    wlr_dmabuf_attributes dmabuf{};
    if (!buffer) {
        // A shared-memory update wlroots already copied into the surface's texture (the client has its buffer back).
        wlr_texture *texture = wlr_surface_get_texture(m_surface);
        if (!texture) {
            return;
        }
        // In the client's own format where the renderer can read it (float for HDR), else 8-bit.
        const uint32_t formats[2] = {m_shmFormat ? m_shmFormat : DRM_FORMAT_ARGB8888, DRM_FORMAT_ARGB8888};
        bool read = false;
        for (uint32_t format : formats) {
            const int bytes = Graphics::pixelKind(format) == Color::PixelKind::Float16 ? 8 : 4;
            const uint32_t stride = texture->width * bytes;
            handoff->pixels = QByteArray(qsizetype(stride) * texture->height, Qt::Uninitialized);
            wlr_texture_read_pixels_options options{};
            options.data = handoff->pixels.data();
            options.format = format;
            options.stride = stride;
            if (wlr_texture_read_pixels(texture, &options)) {
                handoff->kind = Xr::BufferHandoff::Kind::Shm;
                // Same bytes as ARGB8888, but an opaque client's alpha byte means nothing: kept opaque.
                const bool opaque = m_shmFormat && !Graphics::hasAlpha(m_shmFormat);
                handoff->drmFormat = format == DRM_FORMAT_ARGB8888 && opaque ? DRM_FORMAT_XRGB8888 : format;
                handoff->width = int(texture->width);
                handoff->height = int(texture->height);
                handoff->stride = int(stride);
                read = true;
                break;
            }
            if (!m_readbackWarned) {
                m_readbackWarned = true;
                qCWarning(lcSurface).noquote() << "surface" << m_id << "updates cannot be read back as"
                                               << Graphics::drmFormatName(format) << "(trying 8-bit)";
            }
        }
        if (!read) {
            return;
        }
    } else if (LinuxDmabuf::dmabufOf(buffer, &dmabuf)) {
        handoff->kind = Xr::BufferHandoff::Kind::Dmabuf;
        handoff->drmFormat = dmabuf.format;
        handoff->modifier = dmabuf.modifier;
        handoff->width = dmabuf.width;
        handoff->height = dmabuf.height;
        const std::vector<uint64_t> modifiers = LinuxDmabuf::planeModifiers(buffer); // each plane's, if it came through ours
        for (int i = 0; i < dmabuf.n_planes; ++i) {
            handoff->planes.push_back({fcntl(dmabuf.fd[i], F_DUPFD_CLOEXEC, 0), dmabuf.offset[i], dmabuf.stride[i],
                                       size_t(i) < modifiers.size() ? modifiers[size_t(i)] : dmabuf.modifier});
        }
        // Locked until the XR side has copied it: the client gets it back (and its release point) only then.
        handoff->releaseToken = m_server.lockForXr(buffer);
        handoff->bufferId = m_server.bufferId(buffer, &handoff->newBuffer);
    } else {
        void *data = nullptr;
        uint32_t format = 0;
        size_t stride = 0;
        if (!wlr_buffer_begin_data_ptr_access(buffer, WLR_BUFFER_DATA_PTR_ACCESS_READ, &data, &format, &stride)) {
            return;
        }
        handoff->kind = Xr::BufferHandoff::Kind::Shm;
        handoff->drmFormat = format;
        m_shmFormat = format;
        handoff->width = buffer->width;
        handoff->height = buffer->height;
        handoff->stride = int(stride);
        handoff->pixels = QByteArray(static_cast<const char *>(data), qsizetype(stride) * buffer->height);
        wlr_buffer_end_data_ptr_access(buffer);
    }

    Color::SurfaceColorDescription color = probeColor(m_surface);
    if (handoff->drmFormat == DRM_FORMAT_P010) {
        // Firefox makes a new video surface on every fullscreen switch and tags it HDR only after its first frames
        // (else video was washed out for 0.3 s on average, up to 9 s): until it has a description of its own,
        // a window's new video surface takes the one its last video had.
        wlr_surface *root = wlr_surface_get_root_surface(m_surface);
        if (color.tagged) {
            m_ownDescription = true;
            m_server.rememberVideoDescription(root, color);
        } else if (!m_ownDescription) {
            if (const std::optional<Color::SurfaceColorDescription> last = m_server.videoDescription(root)) {
                color = *last;
            }
        }
    }
    handoff->plan = Color::planLayer(color, Graphics::pixelKind(handoff->drmFormat), Color::untaggedTransfer());
    handoff->alpha = Graphics::hasAlpha(handoff->drmFormat);

    if (m_server.explicitSync) {
        wlr_linux_drm_syncobj_surface_v1_state *state = wlr_linux_drm_syncobj_v1_get_surface_state(m_surface);
        if (state && state->acquire_timeline) {
            handoff->acquireFence = wlr_drm_syncobj_timeline_export_sync_file(state->acquire_timeline, state->acquire_point);
        }
    }
    if (handoff->acquireFence < 0 && handoff->kind == Xr::BufferHandoff::Kind::Dmabuf && !handoff->planes.empty()) {
        handoff->acquireFence = exportImplicitFence(handoff->planes[0].fd);
    }
    bridge->publishBuffer(m_id, std::move(handoff));
}

} // namespace RiftHome::Wayland
