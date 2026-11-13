// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "spatial/WindowFrame.hpp"

#include <array>
#include <cmath>

namespace RiftHome::Spatial
{

/*!
 * The dock (the X button shows and hides it): a bar below the gaze with Home, the keyboard,
 * Follow all, Recline, game mode, the screenshot, Settings, the notifications, the network, the time and the volume. Dock-local metres, origin at its centre, x right, y up. Pure.
 */
enum class DockItem {
    None,
    Home,
    Keyboard,
    FollowAll,
    Recline, //!< the arrangement turned to where the user looks (lying back, on one side); again: upright
    Gamepad, //!< game mode: the controllers as a gamepad for a flatscreen game; hold the menu button to stop
    Screenshot, //!< a screenshot of the room, once the dock has gone (from B to here)
    Settings, //!< the Settings app
    Notifications, //!< the bell: the list of notifications above the dock, their count on it
    Network, //!< shows the connection (Wi-Fi strength, wired, none); tap: Settings' network page
    Time, //!< shows the time; not a button
    VolumeDown,
    Volume, //!< shows the volume; tap: mute on/off
    VolumeUp,
};

struct DockLayout {
    static constexpr float kButton = 0.075f; //!< a round button's pitch (about 6 degrees at 0.75 m)
    static constexpr float kPadding = 0.014f;
    static constexpr float kTimeWidth = 0.13f;
    static constexpr float kVolumeWidth = 0.09f;
    static constexpr float kSeparator = 0.03f;
    static constexpr std::array<DockItem, 13> kItems{DockItem::Home,          DockItem::Keyboard, DockItem::FollowAll,  DockItem::Recline,
                                                    DockItem::Gamepad,       DockItem::Screenshot, DockItem::Settings, DockItem::Notifications,
                                                    DockItem::Network,       DockItem::Time,     DockItem::VolumeDown, DockItem::Volume,
                                                    DockItem::VolumeUp};

    static float itemWidth(DockItem item)
    {
        switch (item) {
        case DockItem::Time:
            return kTimeWidth;
        case DockItem::Volume:
            return kVolumeWidth;
        default:
            return kButton;
        }
    }
    static float width()
    {
        float w = 2.0f * kPadding + 2.0f * kSeparator;
        for (DockItem item : kItems) {
            w += itemWidth(item);
        }
        return w;
    }
    static float height()
    {
        return kButton + 2.0f * kPadding;
    }
    //! Where an item is: left to right, a gap after the buttons (before the network and time) and after the time.
    static FrameRect rect(DockItem item)
    {
        float x = -width() / 2.0f + kPadding;
        for (DockItem i : kItems) {
            if (i == DockItem::Network || i == DockItem::VolumeDown) {
                x += kSeparator;
            }
            const float w = itemWidth(i);
            if (i == item) {
                return {x + w / 2.0f, 0.0f, w, kButton};
            }
            x += w;
        }
        return {};
    }
    //! The item at a dock-local point (None off the items and on the time).
    static DockItem itemAt(float px, float py)
    {
        for (DockItem item : kItems) {
            if (item != DockItem::Time && rect(item).grown(0.004f, 0.01f).contains(px, py)) {
                return item;
            }
        }
        return DockItem::None;
    }
    static bool contains(float px, float py)
    {
        return std::fabs(px) <= width() / 2.0f && std::fabs(py) <= height() / 2.0f;
    }
};

} // namespace RiftHome::Spatial
