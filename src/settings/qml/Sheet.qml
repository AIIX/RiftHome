// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Effects

// A dialog over the window (a question, a password, the room chooser): frosted glass as the window is:
// the page behind it blurred and dimmed, the sheet a translucent pane over it; rounded, centred.
Popup {
    id: sheet
    property string title
    property string message
    default property alias content: body.data
    parent: Overlay.overlay
    anchors.centerIn: parent
    width: 640
    modal: true
    focus: true
    padding: 32
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
    onAboutToShow: app.openSheets += 1
    onAboutToHide: app.openSheets = Math.max(0, app.openSheets - 1)
    Overlay.modal: Item {
        // The page drawn blurred instead of sharp while the sheet is open (hideSource), so it reads through the sheet
        // as frosted glass; the window's own glass (and the room's blur behind it in the headset) stays.
        ShaderEffectSource {
            id: page
            sourceItem: app.contentItem
            hideSource: true
            visible: false
        }
        MultiEffect {
            anchors.fill: parent
            source: page
            autoPaddingEnabled: false
            blurEnabled: true
            blurMax: 64
            blur: 1.0
            saturation: -0.1
        }
        Rectangle {
            anchors.fill: parent
            radius: 22
            color: Qt.rgba(0, 0, 0, app.dark ? 0.28 : 0.10)
        }
    }
    background: Rectangle {
        radius: 24
        color: app.dark ? Qt.rgba(0.17, 0.175, 0.2, 0.72) : Qt.rgba(1, 1, 1, 0.66)
        border.color: app.dark ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(1, 1, 1, 0.8)
        // Presses on the sheet end here (its buttons' tap handlers still see them): without this a tap on a button also
        // reached what lay under the sheet (Cancel closed the password sheet and the network row
        // under it opened it again at once).
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: wheel => wheel.accepted = true
        }
    }
    contentItem: Column {
        objectName: "sheetContent" // App.tapsBlocked: taps inside a sheet stay on
        spacing: 20
        Text {
            width: parent.width
            text: sheet.title
            color: app.text
            font.pixelSize: 32
            font.bold: true
            wrapMode: Text.WordWrap
        }
        Text {
            width: parent.width
            visible: sheet.message.length > 0
            text: sheet.message
            color: app.subtext
            font.pixelSize: 23
            wrapMode: Text.WordWrap
        }
        Column {
            id: body
            width: parent.width
            spacing: 20
        }
    }
}
