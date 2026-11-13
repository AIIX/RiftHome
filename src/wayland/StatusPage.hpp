// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <QImage>
#include <QRectF>
#include <QString>

struct wlr_buffer;

namespace RiftHome::Wayland
{

/*!
 * The monitor's page while Rift Home is its own session (a simple UI with
 * exit controls and status, not a full 2D desktop): what the headset and the runtime
 * are doing, the time, and two buttons, Leave Rift Home and Terminal (a terminal on the monitor for commands).
 */
struct StatusState {
    QString headset; //!< "Lent to the runtime", "Not connected", ...
    bool headsetOk = false;
    QString runtime; //!< "Running", "Starting", "Stopped", ...
    bool runtimeOk = false;
    int windows = 0; //!< windows in the headset
    QString time;
    bool confirmLeave = false; //!< Leave was asked once: the button asks again
    bool operator==(const StatusState &) const = default;
};

QImage drawStatusPage(const StatusState &state, int width, int height);
//! The buttons, in the page's pixels.
QRectF leaveButton(int width, int height);
QRectF terminalButton(int width, int height);

//! A wlroots buffer holding @p image (RGBA8888): what a scene node shows. Dropped with wlr_buffer_drop.
wlr_buffer *bufferFromImage(const QImage &image);

} // namespace RiftHome::Wayland
