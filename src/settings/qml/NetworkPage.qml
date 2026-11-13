// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls

// Wi-Fi and wired networks (NetworkManager through nmcli): join with a password typed on the virtual keyboard.
SettingsPage {
    id: page
    title: "Wi-Fi & Network"
    property var chosen: ({})
    property string detailsConnection
    property string detailsDevice

    function openDetails(connection, device) {
        page.detailsConnection = connection
        page.detailsDevice = device
        detailsError.text = ""
        network.loadDetails(connection, device)
        details.open()
    }

    // A connection's IPv4 settings, from what NetworkManager has (filled in when they arrive).
    Connections {
        target: network
        function onDetailsChanged() {
            const d = network.details
            manualChoice.manual = d.manual
            addressField.text = d.address || ""
            prefixField.text = String(d.prefix || 24)
            gatewayField.text = d.gateway || ""
            dnsAuto.checked_ = d.automaticDns
            dnsField.text = d.dns || ""
        }
    }

    // Read while the page shows; a fresh Wi-Fi scan when it opens and every 20 s.
    Timer {
        interval: 4000
        repeat: true
        running: page.visible
        triggeredOnStart: true
        property int ticks: 0
        onTriggered: {
            network.refresh(ticks % 5 === 0)
            ticks++
        }
    }

    Text {
        visible: network.loaded && !network.available
        width: parent.width
        text: "NetworkManager is not running: networks cannot be shown."
        color: app.subtext
        font.pixelSize: 23
        wrapMode: Text.WordWrap
    }

    Card {
        visible: network.available && network.wifiHardware
        SettingRow {
            label: "Wi-Fi"
            detail: network.wifiEnabled ? "" : "Off"
            separator: network.wifiEnabled && network.wifiNetworks.length > 0
            Toggle {
                checked: network.wifiEnabled
                onToggled: on => network.setWifiEnabled(on)
            }
        }
        Repeater {
            model: network.wifiEnabled ? network.wifiNetworks : []
            delegate: SettingRow {
                required property var modelData
                required property int index
                label: modelData.ssid
                labelColor: modelData.active ? app.accent : app.text
                detail: network.busy === modelData.ssid ? "Connecting…" : modelData.active ? "Connected" : modelData.known ? "Saved" : ""
                separator: index < network.wifiNetworks.length - 1
                tappable: true
                onTapped: {
                    page.chosen = modelData
                    if (modelData.active || modelData.known || !modelData.secure) {
                        actions.open()
                    } else {
                        password.text = ""
                        joinSheet.open()
                    }
                }
                Image {
                    visible: modelData.secure
                    source: "image://icon/" + (app.dark ? "dark/" : "light/") + "lock"
                    sourceSize.width: 30
                    sourceSize.height: 30
                    anchors.verticalCenter: parent.verticalCenter
                    opacity: 0.8
                }
                SignalBars {
                    signal: modelData.signal
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
    }

    Text {
        visible: network.message.length > 0
        width: parent.width
        text: network.message
        color: network.message.startsWith("Could not") ? app.danger : app.subtext
        font.pixelSize: 23
        wrapMode: Text.WordWrap
        leftPadding: 18
    }

    Card {
        heading: "Wired"
        visible: network.available
        Repeater {
            id: wired
            model: network.devices.filter(d => !d.wifi)
            delegate: SettingRow {
                required property var modelData
                required property int index
                label: modelData.connection.length > 0 ? modelData.connection : "Ethernet"
                detail: modelData.device + (modelData.address.length > 0 ? " · " + modelData.address : "")
                separator: index < wired.count - 1
                tappable: modelData.connection.length > 0
                onTapped: page.openDetails(modelData.connection, modelData.device)
                Text {
                    text: modelData.connected ? "Connected" : modelData.state
                    color: modelData.connected ? app.accent : app.subtext
                    font.pixelSize: 23
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    visible: modelData.connection.length > 0
                    text: "›"
                    color: app.subtext
                    font.pixelSize: 40
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
        SettingRow {
            visible: wired.count === 0
            label: "No wired connection"
            separator: false
        }
    }

    // VPNs NetworkManager knows (Proton VPN's, imported ones): on and off here too.
    Card {
        heading: "VPN"
        visible: network.available && network.vpns.length > 0
        Repeater {
            id: vpnList
            model: network.vpns
            delegate: SettingRow {
                required property var modelData
                required property int index
                label: modelData.name
                detail: (modelData.wireguard ? "WireGuard" : "VPN") + (modelData.active ? " · connected" : "")
                separator: index < vpnList.count - 1
                Toggle {
                    checked: modelData.active
                    anchors.verticalCenter: parent.verticalCenter
                    onToggled: on => network.setVpn(modelData.uuid, on)
                }
            }
        }
    }

    // A network chosen: what can be done with it.
    Sheet {
        id: actions
        title: page.chosen.ssid || ""
        message: page.chosen.active ? "Connected." : page.chosen.known ? "Saved network." : "Open network: no password."
        Row {
            spacing: 14
            PillButton {
                visible: !page.chosen.active
                text: "Join"
                primary: true
                onClicked: { network.join(page.chosen.ssid, ""); actions.close() }
            }
            PillButton {
                visible: !!page.chosen.active
                text: "Details"
                onClicked: {
                    actions.close()
                    const wifi = network.devices.find(d => d.wifi)
                    page.openDetails(wifi ? wifi.connection : page.chosen.ssid, wifi ? wifi.device : "")
                }
            }
            PillButton {
                visible: !!page.chosen.active
                text: "Disconnect"
                onClicked: { network.disconnectWifi(); actions.close() }
            }
            PillButton {
                visible: !!page.chosen.known
                text: "Forget"
                destructive: true
                onClicked: { network.forget(page.chosen.ssid); actions.close() }
            }
            PillButton {
                text: "Cancel"
                onClicked: actions.close()
            }
        }
    }

    component Field: TextField {
        id: field
        height: 60
        font.pixelSize: 24
        color: app.text
        background: Rectangle {
            radius: 14
            color: app.dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.05)
            border.color: field.activeFocus ? app.accent : app.line
            border.width: 2
        }
    }

    // Connection details: automatic or manual IPv4, DNS servers.
    Sheet {
        id: details
        width: 760
        title: page.detailsConnection
        message: network.details.currentAddress ? "Now: " + network.details.currentAddress
                                                  + (network.details.currentGateway ? " · gateway " + network.details.currentGateway : "")
                                                  + (network.details.currentDns ? " · DNS " + network.details.currentDns : "") : ""
        Row {
            id: manualChoice
            property bool manual: false
            spacing: 12
            PillButton {
                text: "Automatic (DHCP)"
                primary: !manualChoice.manual
                onClicked: manualChoice.manual = false
            }
            PillButton {
                text: "Manual"
                primary: manualChoice.manual
                onClicked: manualChoice.manual = true
            }
        }
        Row {
            visible: manualChoice.manual
            spacing: 12
            Field {
                id: addressField
                width: 330
                placeholderText: "Address (192.168.0.20)"
            }
            Text {
                text: "/"
                color: app.subtext
                font.pixelSize: 30
                anchors.verticalCenter: parent.verticalCenter
            }
            Field {
                id: prefixField
                width: 90
                placeholderText: "24"
                inputMethodHints: Qt.ImhDigitsOnly
            }
        }
        Field {
            id: gatewayField
            visible: manualChoice.manual
            width: parent.width
            placeholderText: "Gateway (192.168.0.1)"
        }
        Row {
            spacing: 14
            Toggle {
                id: dnsAuto
                property bool checked_: true
                checked: checked_
                onToggled: on => checked_ = on
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: "Automatic DNS"
                color: app.text
                font.pixelSize: 23
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        Field {
            id: dnsField
            visible: !dnsAuto.checked
            width: parent.width
            placeholderText: "DNS servers (1.1.1.1, 9.9.9.9)"
        }
        Text {
            id: detailsError
            width: parent.width
            visible: text.length > 0
            color: app.danger
            font.pixelSize: 22
            wrapMode: Text.WordWrap
        }
        Row {
            spacing: 14
            PillButton {
                text: "Apply"
                primary: true
                onClicked: applyConfirm.open()
            }
            PillButton {
                text: "Cancel"
                onClicked: details.close()
            }
        }
    }

    Sheet {
        id: applyConfirm
        title: "Apply to " + page.detailsConnection + "?"
        message: "The connection restarts with the new settings. A wrong address or gateway can cut this computer off the network until it is set back."
        Row {
            spacing: 14
            PillButton {
                text: "Apply"
                destructive: true
                onClicked: {
                    applyConfirm.close()
                    const problem = network.applyDetails(page.detailsConnection, manualChoice.manual, addressField.text, parseInt(prefixField.text),
                                                         gatewayField.text, dnsAuto.checked, dnsField.text)
                    detailsError.text = problem
                    if (problem.length === 0)
                        details.close()
                }
            }
            PillButton {
                text: "Cancel"
                onClicked: applyConfirm.close()
            }
        }
    }

    // A new secured network: its password (the virtual keyboard opens on the field).
    Sheet {
        id: joinSheet
        title: "Join " + (page.chosen.ssid || "")
        message: "Enter the network's password."
        onOpened: password.forceActiveFocus()
        TextField {
            id: password
            width: parent.width
            height: 64
            font.pixelSize: 26
            echoMode: show.checked ? TextInput.Normal : TextInput.Password
            placeholderText: "Password"
            color: app.text
            background: Rectangle {
                radius: 14
                color: app.dark ? Qt.rgba(1, 1, 1, 0.08) : Qt.rgba(0, 0, 0, 0.05)
                border.color: password.activeFocus ? app.accent : app.line
                border.width: 2
            }
            onAccepted: if (text.length >= 8) { network.join(page.chosen.ssid, text); joinSheet.close() }
        }
        Row {
            spacing: 14
            Toggle {
                id: show
                property bool checked_: false
                checked: checked_
                onToggled: on => checked_ = on
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                text: "Show password"
                color: app.text
                font.pixelSize: 23
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        Row {
            spacing: 14
            PillButton {
                text: "Join"
                primary: true
                enabled_: password.text.length >= 8
                onClicked: { network.join(page.chosen.ssid, password.text); joinSheet.close() }
            }
            PillButton {
                text: "Cancel"
                onClicked: joinSheet.close()
            }
        }
    }
}
