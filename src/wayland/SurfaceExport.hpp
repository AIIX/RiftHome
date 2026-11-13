// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "wayland/Listener.hpp"

#include <cstdint>

struct wlr_surface;

namespace RiftHome::Wayland
{

class Server;

/*!
 * Hands a surface's new buffers to the XR side: dmabufs as duplicated file descriptors with the buffer locked
 * until the XR side has copied it, shared memory as a copy; with the client's acquire fence and the layer plan. Only
 * surfaces of windows (xdg toplevels, popups, X11 windows and menus, and their subsurfaces). Deletes itself with the
 * surface.
 *
 * Shared memory: wlroots copies a same-sized update straight into the surface's texture and releases the client's
 * buffer before the commit signal (current.buffer is then null), so updates after the first are read back from that
 * texture (else Dolphin and Konsole never changed in the headset, which looked like dead clicks).
 */
class SurfaceExport
{
public:
    SurfaceExport(Server &server, wlr_surface *surface, uint64_t id);
    ~SurfaceExport();
    SurfaceExport(const SurfaceExport &) = delete;
    SurfaceExport &operator=(const SurfaceExport &) = delete;

    //! The surface's current buffer to the XR side, as if just committed (Server::publishCurrentBuffers).
    void publishCurrent();
    //! Rift Home's own session (DRM): the apps' scale told to the surface itself (Server::Options::appScale; there the
    //! room's windows are on no output, which would tell it). Not for X11 surfaces (Xft.dpi).
    void tellScale();

private:
    void onCommit();

    Server &m_server;
    wlr_surface *m_surface;
    uint64_t m_id;
    Listener m_commit;
    Listener m_destroy;
    uint32_t m_shmFormat = 0; //!< the client's shared-memory format, for reading its updates back from the texture
    bool m_readbackWarned = false;
    bool m_ownDescription = false; //!< a video surface that has had a colour description of its own
    double m_scaleTold = 0.0; //!< the apps' scale last told (0: not yet)
};

} // namespace RiftHome::Wayland
