// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick

// A rounded button: plain, primary (accent) or destructive (red).
Rectangle {
    id: button
    property string text
    property bool primary: false
    property bool destructive: false
    property bool enabled_: true
    signal clicked
    width: Math.max(140, label.implicitWidth + 48)
    height: 54
    radius: 27
    opacity: enabled_ ? 1.0 : 0.45
    color: primary ? app.accent : destructive ? Qt.rgba(1, 0.35, 0.32, hover.hovered ? 0.3 : 0.18)
                                              : (hover.hovered ? Qt.rgba(app.selected.r, app.selected.g, app.selected.b, app.selected.a * 1.6) : app.selected)
    Text {
        id: label
        anchors.centerIn: parent
        text: button.text
        color: button.primary ? "white" : button.destructive ? app.danger : app.text
        font.pixelSize: 24
        font.bold: button.primary
    }
    HoverHandler {
        id: hover
    }
    TapHandler {
        enabled: button.enabled_ && !app.tapsBlocked(button)
        onTapped: button.clicked()
    }
}
