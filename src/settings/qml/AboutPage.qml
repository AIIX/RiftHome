// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick

// What this is.
SettingsPage {
    title: "About"
    Card {
        Repeater {
            id: rows
            model: system.about
            delegate: SettingRow {
                required property var modelData
                required property int index
                label: modelData.label
                separator: index < rows.count - 1
                Text {
                    text: modelData.value
                    color: app.subtext
                    font.pixelSize: 23
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
    }
}
