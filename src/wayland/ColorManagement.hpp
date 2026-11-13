// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "color/ColorDescription.hpp"

#include <QStringList>

#include <cstdint>
#include <memory>

struct wl_display;
struct wlr_surface;

namespace RiftHome::Wayland
{

/*!
 * Rift Home's own color-management-v1 (version 2), instead of wlroots': wlroots 0.20's scene aborts on any
 * transfer function or primaries it does not map itself, and its manager can offer only those (no HLG, no Display
 * P3). Rift Home decodes every parametric description it offers on the way to the headset (window.comp): named and
 * power transfer functions, named primaries and chromaticities, luminances and mastering metadata. A surface's
 * description is double-buffered state (wlr_surface_synced, applied with wl_surface.commit, also through synchronized
 * subsurfaces). wlroots' scene never sees descriptions; it shows windows as sRGB on a desktop monitor (which shows
 * only the status page in Rift Home's own session).
 *
 * Offered: the perceptual intent; parametric descriptions with set_tf_power, set_primaries, set_luminances and
 * mastering metadata (no ICC, no extended target volume, no Windows-scRGB); every surface's preferred description is
 * @p preferred (sRGB with gamma 2.2, or PQ BT.2020 with --preferred pq).
 */
class ColorManagement
{
public:
    struct Preferred {
        uint32_t transfer = 2; //!< WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22
        uint32_t primaries = 1; //!< WP_COLOR_MANAGER_V1_PRIMARIES_SRGB
    };
    //! The global on @p display; it lives until the display is destroyed.
    static ColorManagement *create(wl_display *display, Preferred preferred);

    //! What a client has committed on @p surface (untagged when nothing, or no color management at all).
    static Color::SurfaceColorDescription descriptionOf(wlr_surface *surface);

    static QStringList offeredTransfers();
    static QStringList offeredPrimaries();

    struct Impl;

private:
    explicit ColorManagement(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> d;
};

} // namespace RiftHome::Wayland
