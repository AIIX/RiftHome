// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick

// A Wi-Fi network's strength as four bars.
Row {
    property int signal: 0
    spacing: 4
    height: 30
    Repeater {
        model: 4
        Rectangle {
            required property int index
            width: 7
            height: 9 + index * 7
            anchors.bottom: parent.bottom
            radius: 2
            color: parent.signal >= 15 + index * 22 ? app.text : (app.dark ? Qt.rgba(1, 1, 1, 0.22) : Qt.rgba(0, 0, 0, 0.18))
        }
    }
}
