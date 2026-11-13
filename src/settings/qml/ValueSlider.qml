// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls

// A slider 0..100 that reports when let go (moved: while dragging).
Slider {
    id: slider
    from: 0
    to: 100
    stepSize: 1
    width: 360
    height: 48
    signal released(int value)
    onPressedChanged: if (!pressed) slider.released(Math.round(value))
    background: Rectangle {
        x: slider.leftPadding
        y: slider.topPadding + slider.availableHeight / 2 - height / 2
        width: slider.availableWidth
        height: 10
        radius: 5
        color: app.dark ? Qt.rgba(1, 1, 1, 0.16) : Qt.rgba(0, 0, 0, 0.12)
        Rectangle {
            width: slider.visualPosition * parent.width
            height: parent.height
            radius: 5
            color: app.accent
        }
    }
    handle: Rectangle {
        x: slider.leftPadding + slider.visualPosition * (slider.availableWidth - width)
        y: slider.topPadding + slider.availableHeight / 2 - height / 2
        width: 38
        height: 38
        radius: 19
        color: "white"
        border.color: app.line
    }
}
