// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls

// The time, the time zone, automatic time (systemd), and the dock clock's format (Rift Home).
SettingsPage {
    id: page
    title: "Date & Time"
    property date now: new Date()

    Timer {
        interval: 1000
        repeat: true
        running: page.visible
        triggeredOnStart: true
        onTriggered: page.now = new Date()
    }
    Timer {
        interval: 5000
        repeat: true
        running: page.visible
        triggeredOnStart: true
        onTriggered: dateTime.refresh()
    }

    Column {
        width: parent.width
        spacing: 2
        Text {
            text: Qt.formatTime(page.now, riftSettings.use24Hour ? "HH:mm" : "h:mm AP")
            color: app.text
            font.pixelSize: 72
            font.weight: Font.Light
        }
        Text {
            text: Qt.formatDate(page.now, "dddd d MMMM yyyy")
            color: app.subtext
            font.pixelSize: 26
            bottomPadding: 10
        }
    }

    Card {
        SettingRow {
            label: "Set automatically"
            detail: dateTime.automatic ? (dateTime.synchronized ? "From the internet's time servers" : "Not synchronised yet") : "Off"
            Toggle {
                checked: dateTime.automatic
                onToggled: on => dateTime.setAutomatic(on)
            }
        }
        SettingRow {
            label: "Time zone"
            detail: dateTime.timeZone.replace(/_/g, " ")
            tappable: true
            onTapped: { dateTime.loadTimeZones(); search.text = ""; zones.open() }
            Text {
                text: "›"
                color: app.subtext
                font.pixelSize: 40
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        SettingRow {
            label: "24-hour clock"
            detail: "In the dock and here"
            separator: false
            Toggle {
                checked: riftSettings.use24Hour
                onToggled: on => riftSettings.use24Hour = on
            }
        }
    }

    Text {
        width: parent.width
        text: dateTime.message.length > 0 ? dateTime.message
              : auth.available ? "Changing the time zone or automatic time asks for an administrator's password."
                               : "Changing the time zone or automatic time asks for an administrator's password (on the desktop's screen)."
        color: dateTime.message.length > 0 ? app.danger : app.subtext
        font.pixelSize: 23
        wrapMode: Text.WordWrap
        leftPadding: 18
    }

    Sheet {
        id: zones
        title: "Time zone"
        height: 680
        TextField {
            id: search
            width: parent.width
            height: 60
            font.pixelSize: 24
            placeholderText: "Search (city or region)"
            color: app.text
            background: Rectangle {
                radius: 14
                color: app.dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.05)
                border.color: search.activeFocus ? app.accent : app.line
                border.width: 2
            }
        }
        ListView {
            width: parent.width
            height: 420
            clip: true
            model: dateTime.timeZones.filter(z => search.text.length === 0 || z.toLowerCase().replace(/_/g, " ").indexOf(search.text.toLowerCase()) >= 0)
            ScrollBar.vertical: ScrollBar {}
            delegate: SettingRow {
                required property string modelData
                width: ListView.view.width
                label: modelData.replace(/_/g, " ")
                labelColor: modelData === dateTime.timeZone ? app.accent : app.text
                tappable: true
                onTapped: { dateTime.setTimeZone(modelData); zones.close() }
            }
        }
    }
}
