// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick

// An on/off switch, large enough for a controller's ray.
Rectangle {
    id: toggle
    property bool checked
    signal toggled(bool on)
    width: 84
    height: 48
    radius: height / 2
    color: checked ? app.accent : (app.dark ? Qt.rgba(1, 1, 1, 0.18) : Qt.rgba(0, 0, 0, 0.14))
    Behavior on color { ColorAnimation { duration: 120 } }
    Rectangle {
        width: 40
        height: 40
        radius: 20
        y: 4
        x: toggle.checked ? toggle.width - width - 4 : 4
        color: "white"
        Behavior on x { NumberAnimation { duration: 120 } }
    }
    TapHandler {
        enabled: !app.tapsBlocked(toggle)
        onTapped: toggle.toggled(!toggle.checked)
    }
}
