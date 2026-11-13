// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Settings, as visionOS's: pages on the left, the page on the right; dark or light as Rift Home's frames. Sizes are for
// the headset (1000 window pixels a metre at 1.6 m): text 26 px, rows 78 px.
ApplicationWindow {
    id: app
    width: 1280
    height: 820
    visible: !startHidden // the session's password agent waits without a window
    title: "Settings"
    color: "transparent"

    readonly property bool dark: riftSettings.frameTheme !== "light"
    // Light: frosted glass (translucent enough to look like glass).
    readonly property color glass: dark ? Qt.rgba(0.085, 0.09, 0.105, 0.94) : Qt.rgba(0.97, 0.97, 0.98, 0.70)
    readonly property color side: dark ? Qt.rgba(1, 1, 1, 0.035) : Qt.rgba(0, 0, 0, 0.03)
    readonly property color card: dark ? Qt.rgba(1, 1, 1, 0.075) : Qt.rgba(1, 1, 1, 0.55)
    readonly property color text: dark ? "#f2f2f5" : "#1c1c1e"
    // Secondary text: plainly readable on the CV1 (it was 58 %).
    readonly property color subtext: dark ? Qt.rgba(0.95, 0.95, 0.96, 0.72) : Qt.rgba(0.11, 0.11, 0.12, 0.72)
    readonly property color line: dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.08)
    readonly property color selected: dark ? Qt.rgba(1, 1, 1, 0.13) : Qt.rgba(0, 0, 0, 0.07)
    readonly property color accent: dark ? "#4c9eff" : "#0a6cff"
    readonly property color danger: "#ff5a52"
    readonly property int fontSize: 26
    readonly property int rowHeight: 78

    property int page: startPage
    // Sheets open now. Qt gives tap handlers under a modal popup the press too, so a tap on a sheet's button also tapped
    // what lay under it (Cancel closed the password sheet, the network row under it opened it again).
    // While a sheet is open, taps outside sheets are off; they are judged at the press, so closing the sheet on the
    // release does not let the one underneath through.
    property int openSheets: 0
    function tapsBlocked(item) {
        if (openSheets === 0)
            return false
        for (let p = item; p; p = p.parent) {
            if (p.objectName === "sheetContent")
                return false
        }
        return true
    }

    readonly property var pages: [
        { name: "Wi-Fi & Network", icon: "wifi" },
        { name: "Bluetooth", icon: "bluetooth" },
        { name: "Sound", icon: "sound" },
        { name: "Date & Time", icon: "clock" },
        { name: "Display & Room", icon: "room" },
        { name: "Keyboard", icon: "keyboard" },
        { name: "Controllers", icon: "controller" },
        { name: "Power", icon: "power" },
        { name: "About", icon: "about" }
    ]

    background: Rectangle {
        color: app.glass
        radius: 22
    }

    RowLayout {
        anchors.fill: parent
        spacing: 0

        // The pages.
        Rectangle {
            Layout.fillHeight: true
            Layout.preferredWidth: 340
            color: app.side
            radius: 22

            Column {
                anchors.fill: parent
                anchors.margins: 22
                spacing: 6

                Text {
                    text: "Settings"
                    color: app.text
                    font.pixelSize: 44
                    font.bold: true
                    bottomPadding: 18
                    leftPadding: 10
                }
                Repeater {
                    model: app.pages
                    delegate: Rectangle {
                        required property var modelData
                        required property int index
                        width: parent.width
                        height: 70
                        radius: 16
                        color: app.page === index ? app.selected : (hover.hovered ? Qt.rgba(app.selected.r, app.selected.g, app.selected.b, 0.5 * app.selected.a) : "transparent")
                        Row {
                            anchors.verticalCenter: parent.verticalCenter
                            anchors.left: parent.left
                            anchors.leftMargin: 14
                            spacing: 16
                            Image {
                                source: "image://icon/" + (app.dark ? "dark/" : "light/") + modelData.icon
                                sourceSize.width: 40
                                sourceSize.height: 40
                                width: 40
                                height: 40
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                text: modelData.name
                                color: app.text
                                font.pixelSize: app.fontSize
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }
                        HoverHandler {
                            id: hover
                        }
                        TapHandler {
                            enabled: !app.tapsBlocked(parent)
                            onTapped: app.page = index
                        }
                    }
                }
            }
        }

        // polkit's password prompt for Settings' own changes (Authentication): over the page while asked.
        Sheet {
            id: authSheet
            title: "Password required"
            message: auth.message + (auth.user.length > 0 ? "\n\nAs " + auth.user + "." : "")
            closePolicy: Popup.NoAutoClose
            onOpened: answer.forceActiveFocus()
            Connections {
                target: auth
                function onChanged() {
                    if (auth.pending && !authSheet.opened) {
                        answer.text = ""
                        authSheet.open()
                    } else if (!auth.pending && authSheet.opened) {
                        authSheet.close()
                    }
                }
            }
            TextField {
                id: answer
                width: parent.width
                height: 64
                font.pixelSize: 26
                echoMode: auth.echo ? TextInput.Normal : TextInput.Password
                placeholderText: auth.prompt.length > 0 ? auth.prompt : "Password"
                color: app.text
                background: Rectangle {
                    radius: 14
                    color: app.dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.05)
                    border.color: answer.activeFocus ? app.accent : app.line
                    border.width: 2
                }
                onAccepted: { auth.respond(text); text = "" }
            }
            Text {
                width: parent.width
                visible: auth.error.length > 0
                text: auth.error
                color: app.danger
                font.pixelSize: 22
                wrapMode: Text.WordWrap
            }
            Row {
                spacing: 14
                PillButton {
                    text: "Authenticate"
                    primary: true
                    onClicked: { auth.respond(answer.text); answer.text = "" }
                }
                PillButton {
                    text: "Cancel"
                    onClicked: auth.cancel()
                }
            }
        }

        // The page.
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: app.page
            NetworkPage {}
            BluetoothPage {}
            SoundPage {}
            TimePage {}
            DisplayPage {}
            KeyboardPage {}
            ControllersPage {}
            PowerPage {}
            AboutPage {}
        }
    }
}
