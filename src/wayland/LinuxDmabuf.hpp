// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QString>

#include <cstdint>
#include <vector>

struct wl_display;
struct wlr_buffer;
struct wlr_dmabuf_attributes;
struct wlr_renderer;

namespace RiftHome::Wayland
{

/*!
 * Rift Home's own linux-dmabuf (zwp_linux_dmabuf_v1, version 4), instead of wlroots' (Firefox's HDR
 * video crashed Firefox with it). Two differences:
 *
 * - P010 (10-bit YUV 4:2:0) is offered (linear) and taken whatever its modifiers: Firefox sends HDR video only as P010
 *   buffers of their own, tagged PQ, and only to a compositor that takes P010; wlroots' renderer cannot import it (the
 *   XR side imports it through Vulkan, plane by plane).
 * - Each plane keeps its own modifier: NVIDIA's VA-API decoder gives P010's two planes different block-linear layouts
 *   for some sizes (video previews while scrolling), and wlroots posted a protocol error for that, which ended Firefox.
 *   KWin takes them. wlroots' dmabuf attributes have one modifier (plane 0's); the others come from planeModifiers().
 *
 * Other buffers are checked as wlroots checks them: imported into the renderer once.
 */
class LinuxDmabuf
{
public:
    //! The global on @p display, with @p renderer's dmabuf formats; it lives until the display is destroyed.
    static bool create(wl_display *display, wlr_renderer *renderer);
    //! Each plane's modifier of a buffer made here; empty for any other buffer.
    static std::vector<uint64_t> planeModifiers(wlr_buffer *buffer);
    /*!
     * A dmabuf's attributes, for Rift Home's own use (the XR side imports it): a buffer made here, P010 too, else
     * whatever wlroots says. wlroots itself is not given P010 buffers as dmabufs: its GL renderer cannot use them,
     * and for each one it was given NVIDIA's EGL kept about 30 KB for the buffer's life (Firefox keeps every
     * video frame buffer it made, so Rift Home's heap grew with them).
     */
    static bool dmabufOf(wlr_buffer *buffer, wlr_dmabuf_attributes *attributes);
    //! Buffers made here that are still alive and who keeps them (the stats line: they must not pile up).
    static QString heldSummary();
};

} // namespace RiftHome::Wayland
