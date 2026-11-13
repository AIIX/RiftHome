// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick

// Bluetooth (BlueZ through bluetoothctl): on/off, your devices, a search for new ones to pair.
SettingsPage {
    id: page
    title: "Bluetooth"
    property var chosen: ({})

    Timer {
        interval: page.visible && bluetooth.scanning ? 2000 : 5000
        repeat: true
        running: page.visible
        triggeredOnStart: true
        onTriggered: bluetooth.refresh()
    }

    Text {
        visible: bluetooth.loaded && !bluetooth.available
        width: parent.width
        text: "No Bluetooth adapter (or BlueZ is not running)."
        color: app.subtext
        font.pixelSize: 23
        wrapMode: Text.WordWrap
    }

    Card {
        visible: bluetooth.available
        SettingRow {
            label: "Bluetooth"
            detail: bluetooth.powered ? "" : "Off"
            separator: false
            Toggle {
                checked: bluetooth.powered
                onToggled: on => bluetooth.setPowered(on)
            }
        }
    }

    Card {
        heading: "My devices"
        visible: bluetooth.available && bluetooth.powered
        Repeater {
            id: mine
            model: bluetooth.devices.filter(d => d.paired)
            delegate: SettingRow {
                required property var modelData
                required property int index
                label: modelData.name
                labelColor: modelData.connected ? app.accent : app.text
                detail: bluetooth.busy === modelData.address ? bluetooth.message : modelData.connected ? "Connected" : "Not connected"
                separator: index < mine.count - 1
                tappable: true
                onTapped: { page.chosen = modelData; actions.open() }
            }
        }
        SettingRow {
            visible: mine.count === 0
            label: "No paired devices"
            separator: false
        }
    }

    Card {
        heading: "Other devices"
        visible: bluetooth.available && bluetooth.powered
        Repeater {
            id: others
            model: bluetooth.devices.filter(d => !d.paired)
            delegate: SettingRow {
                required property var modelData
                required property int index
                label: modelData.name
                detail: bluetooth.busy === modelData.address ? bluetooth.message : ""
                separator: true
                PillButton {
                    text: "Pair"
                    anchors.verticalCenter: parent.verticalCenter
                    enabled_: bluetooth.busy.length === 0
                    onClicked: bluetooth.pair(modelData.address)
                }
            }
        }
        SettingRow {
            label: bluetooth.scanning ? "Searching…" : "Search for devices"
            detail: bluetooth.scanning ? "Put the device in pairing mode." : ""
            separator: false
            PillButton {
                text: bluetooth.scanning ? "Searching" : "Search"
                primary: !bluetooth.scanning
                enabled_: !bluetooth.scanning
                anchors.verticalCenter: parent.verticalCenter
                onClicked: bluetooth.scan()
            }
        }
    }

    Text {
        visible: bluetooth.message.length > 0 && bluetooth.busy.length === 0
        width: parent.width
        text: bluetooth.message
        color: app.danger
        font.pixelSize: 22
        wrapMode: Text.WordWrap
        leftPadding: 18
    }

    Sheet {
        id: actions
        title: page.chosen.name || ""
        message: page.chosen.connected ? "Connected." : "Paired, not connected."
        Row {
            spacing: 14
            PillButton {
                visible: !page.chosen.connected
                text: "Connect"
                primary: true
                onClicked: { bluetooth.connectDevice(page.chosen.address); actions.close() }
            }
            PillButton {
                visible: !!page.chosen.connected
                text: "Disconnect"
                onClicked: { bluetooth.disconnectDevice(page.chosen.address); actions.close() }
            }
            PillButton {
                text: "Forget"
                destructive: true
                onClicked: { bluetooth.forget(page.chosen.address); actions.close() }
            }
            PillButton {
                text: "Cancel"
                onClicked: actions.close()
            }
        }
    }
}
