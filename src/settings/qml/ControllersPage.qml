// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick

// What the Touch controllers' buttons do, a row per action as in KDE's Shortcuts: its default, and the user's own
// bindings (a button, or two together), set by pressing them on the controllers (Rift Home records them).
SettingsPage {
    id: page
    title: "Controllers"

    readonly property string recording: riftSettings.recordingAction

    // Column widths of the rows (the header's too).
    readonly property real nameWidth: Math.round(width * 0.2)
    readonly property real defaultWidth: Math.round(width * 0.19)

    component Cell: Rectangle {
        id: cell
        property string text
        property bool active: false
        property bool enabled_: true
        signal clicked
        height: Math.max(54, label.implicitHeight + 16)
        radius: 14
        color: active ? Qt.rgba(app.accent.r, app.accent.g, app.accent.b, 0.28) : (hover.hovered ? Qt.rgba(app.selected.r, app.selected.g, app.selected.b, app.selected.a * 1.6) : app.selected)
        border.width: active ? 2 : 0
        border.color: app.accent
        Text {
            id: label
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 14
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            text: cell.text
            color: app.text
            font.pixelSize: 23
            font.bold: cell.active
            wrapMode: Text.WordWrap
        }
        HoverHandler {
            id: hover
        }
        TapHandler {
            enabled: cell.enabled_ && !app.tapsBlocked(cell)
            onTapped: cell.clicked()
        }
    }

    // A small round or pill button beside a cell: the cross, reset, cancel.
    component SmallButton: Rectangle {
        id: small
        property string text
        signal clicked
        width: Math.max(54, label.implicitWidth + 28)
        height: 54
        radius: 27
        color: smallHover.hovered ? Qt.rgba(app.selected.r, app.selected.g, app.selected.b, app.selected.a * 1.6) : app.selected
        Text {
            id: label
            anchors.centerIn: parent
            text: small.text
            color: app.text
            font.pixelSize: 23
        }
        HoverHandler {
            id: smallHover
        }
        TapHandler {
            enabled: !app.tapsBlocked(small)
            onTapped: small.clicked()
        }
    }

    Text {
        width: parent ? parent.width : 0
        leftPadding: 6
        rightPadding: 6
        wrapMode: Text.WordWrap
        color: app.subtext
        font.pixelSize: 23
        text: "Press a button in the Custom column, then press the button on a controller, or two together (hold one, press "
              + "the other). The trigger never takes part: it clicks."
    }

    Card {
        SettingRow {
            label: "Controller buttons"
            detail: riftSettings.recordNote
            separator: false
            PillButton {
                text: "Reset all"
                onClicked: riftSettings.resetControls()
            }
        }
    }

    Card {
        Item {
            width: parent.width
            height: 56
            Text {
                x: 24
                width: page.nameWidth
                anchors.verticalCenter: parent.verticalCenter
                text: "Action"
                color: app.subtext
                font.pixelSize: 22
            }
            Text {
                x: 24 + page.nameWidth + 12
                width: page.defaultWidth
                anchors.verticalCenter: parent.verticalCenter
                text: "Default"
                color: app.subtext
                font.pixelSize: 22
            }
            Text {
                x: 24 + page.nameWidth + page.defaultWidth + 24
                anchors.verticalCenter: parent.verticalCenter
                text: "Custom"
                color: app.subtext
                font.pixelSize: 22
            }
            Rectangle {
                anchors.bottom: parent.bottom
                x: 24
                width: parent.width - 48
                height: 1
                color: app.line
            }
        }
        Repeater {
            model: riftSettings.controlRows
            delegate: Item {
                id: row
                required property var modelData
                required property int index
                readonly property bool recordingThis: page.recording === modelData.action
                readonly property string custom: modelData.bindings.length > 0 ? modelData.bindings.map(b => b.label).join(",  ") : "None"
                width: parent.width
                height: Math.max(app.rowHeight, defaultText.implicitHeight + 28, cell.height + 24)
                Text {
                    x: 24
                    width: page.nameWidth
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData.label
                    color: app.text
                    font.pixelSize: app.fontSize
                    wrapMode: Text.WordWrap
                }
                Text {
                    id: defaultText
                    x: 24 + page.nameWidth + 12
                    width: page.defaultWidth
                    anchors.verticalCenter: parent.verticalCenter
                    text: row.modelData.defaultText
                    color: app.subtext
                    font.pixelSize: 22
                    wrapMode: Text.WordWrap
                }
                Item {
                    id: custom
                    x: 24 + page.nameWidth + page.defaultWidth + 24
                    width: parent.width - x - 24
                    height: cell.height
                    anchors.verticalCenter: parent.verticalCenter
                    Cell {
                        id: cell
                        anchors.left: parent.left
                        anchors.right: buttons.left
                        anchors.rightMargin: buttons.visible ? 10 : 0
                        text: row.recordingThis ? "Press a button, or two together" : row.custom
                        active: row.recordingThis
                        onClicked: riftSettings.recordBinding(row.modelData.action)
                    }
                    Row {
                        id: buttons
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 10
                        visible: children.some(c => c.visible)
                        SmallButton {
                            visible: row.recordingThis
                            text: "Cancel"
                            onClicked: riftSettings.cancelRecording()
                        }
                        SmallButton {
                            visible: !row.recordingThis && row.modelData.bindings.length > 0
                            text: "\u00d7"
                            onClicked: riftSettings.clearControl(row.modelData.action)
                        }
                        SmallButton {
                            visible: !row.recordingThis && row.modelData.custom
                            text: "Reset"
                            onClicked: riftSettings.resetControl(row.modelData.action)
                        }
                    }
                }
                Rectangle {
                    visible: row.index < riftSettings.controlRows.length - 1
                    anchors.bottom: parent.bottom
                    x: 24
                    width: parent.width - 48
                    height: 1
                    color: app.line
                }
            }
        }
    }

    Text {
        width: parent ? parent.width : 0
        leftPadding: 24
        rightPadding: 24
        wrapMode: Text.WordWrap
        color: app.subtext
        font.pixelSize: 23
        text: "A button can do one thing: giving it to another action moves it. The grips grab windows, so they only work "
              + "together with another button. The right Oculus button is the headset's own menu. "
              + "In game mode the controllers are a gamepad; hold ☰ to leave it. "
              + "If nothing opens Home or the dock, ☰ opens Home anyway."
    }
}
