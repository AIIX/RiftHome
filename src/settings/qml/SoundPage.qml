// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick

// Outputs and inputs (PipeWire): which one is used, how loud, muted.
SettingsPage {
    id: page
    title: "Sound"

    Timer {
        interval: 3000
        repeat: true
        running: page.visible
        triggeredOnStart: true
        onTriggered: sound.refresh()
    }

    component DeviceCard: Card {
        id: card
        property bool output
        property var devices: []
        property string current
        readonly property var currentDevice: devices.find(d => d.name === current)
        Repeater {
            model: card.devices
            delegate: SettingRow {
                required property var modelData
                label: modelData.description
                tappable: true
                onTapped: sound.setDefault(card.output, modelData.name)
                Text {
                    visible: modelData.name === card.current
                    text: "✓"
                    color: app.accent
                    font.pixelSize: 30
                    font.bold: true
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
        SettingRow {
            visible: !!card.currentDevice
            label: card.output ? "Volume" : "Input level"
            detail: card.currentDevice ? card.currentDevice.volume + " %" : ""
            ValueSlider {
                value: card.currentDevice ? card.currentDevice.volume : 0
                anchors.verticalCenter: parent.verticalCenter
                onReleased: v => sound.setVolume(card.output, card.current, v)
            }
        }
        SettingRow {
            visible: !!card.currentDevice
            label: "Mute"
            separator: false
            Toggle {
                checked: card.currentDevice ? card.currentDevice.muted : false
                onToggled: on => sound.setMuted(card.output, card.current, on)
            }
        }
    }

    DeviceCard {
        heading: "Output"
        output: true
        devices: sound.outputs
        current: sound.defaultOutput
    }
    DeviceCard {
        heading: "Input"
        output: false
        devices: sound.inputs
        current: sound.defaultInput
    }
}
