// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls

// Rift Home's look: the window frames' style, the room around (a photo), how bright it is, how dark in cinema, and the
// controllers' pointers.
SettingsPage {
    id: page
    title: "Display & Room"
    Component.onCompleted: {
        riftSettings.findPhotos()
        if (previewSheet === "rooms")
            roomSheet.openOn(currentRoom.room ? currentRoom.room.group : "riftHome")
    }

    component Choice: Rectangle {
        id: choice
        property string label
        property bool chosen
        property color swatch
        signal picked
        width: 240
        height: 150
        radius: 20
        color: app.card
        border.color: chosen ? app.accent : app.line
        border.width: chosen ? 4 : 1
        Rectangle {
            x: 24; y: 22
            width: parent.width - 48
            height: 64
            radius: 14
            color: choice.swatch
            border.color: app.line
        }
        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            text: choice.label
            color: app.text
            font.pixelSize: 24
        }
        TapHandler {
            enabled: !app.tapsBlocked(choice)
            onTapped: choice.picked()
        }
    }

    // "HDR" in the corner of a room's picture.
    component HdrBadge: Rectangle {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        width: 58
        height: 28
        radius: 8
        color: Qt.rgba(0, 0, 0, 0.55)
        Text {
            anchors.centerIn: parent
            text: "HDR"
            color: "white"
            font.pixelSize: 16
            font.bold: true
        }
    }

    component RoomTile: Rectangle {
        required property var modelData
        width: 250
        height: 186
        radius: 18
        color: "transparent"
        border.color: riftSettings.roomPhoto === modelData.path ? app.accent : "transparent"
        border.width: 4
        Rectangle {
            x: 8; y: 8
            width: parent.width - 16
            height: 128
            radius: 12
            color: "#000000"
            clip: true
            Image {
                anchors.fill: parent
                visible: modelData.path.length > 0
                source: modelData.path.length > 0 ? "image://room/" + modelData.path : ""
                sourceSize.width: 360
                fillMode: Image.PreserveAspectCrop
                asynchronous: true
            }
            HdrBadge {
                visible: !!modelData.hdr
            }
        }
        Text {
            x: 12
            y: 142
            width: parent.width - 24
            text: modelData.name
            color: app.text
            font.pixelSize: 22
            elide: Text.ElideRight
        }
        TapHandler {
            enabled: !app.tapsBlocked(parent)
            onTapped: riftSettings.roomPhoto = modelData.path
        }
    }

    // Opens Pictures/RiftHome in the file manager (made if missing): pictures put there show here at once.
    component AddRoomsTile: Rectangle {
        id: addTile
        width: 250
        height: 186
        radius: 18
        color: "transparent"
        Rectangle {
            x: 8; y: 8
            width: parent.width - 16
            height: 128
            radius: 12
            color: "transparent"
            border.color: app.line
            border.width: 2
            Text {
                anchors.centerIn: parent
                text: "+"
                color: app.subtext
                font.pixelSize: 64
            }
        }
        Text {
            x: 12
            y: 142
            width: parent.width - 24
            text: "Add your own"
            color: app.text
            font.pixelSize: 22
            elide: Text.ElideRight
        }
        TapHandler {
            enabled: !app.tapsBlocked(addTile)
            onTapped: riftSettings.openUserRooms()
        }
    }

    Card {
        heading: "Window frames"
        Row {
            padding: 20
            spacing: 20
            Choice {
                label: "Dark"
                swatch: "#2a2c33"
                chosen: riftSettings.frameTheme === "dark"
                onPicked: riftSettings.frameTheme = "dark"
            }
            Choice {
                label: "Light"
                swatch: "#f4f4f6"
                chosen: riftSettings.frameTheme === "light"
                onPicked: riftSettings.frameTheme = "light"
            }
        }
    }

    // How much the windows curve around you (moved windows turned toward them far too much).
    Card {
        heading: "Windows"
        SettingRow {
            label: "Curve around you"
            detail: (riftSettings.windowCurve <= 0.02 ? "Flat: side by side like a wall, all facing the same way."
                     : riftSettings.windowCurve >= 0.98 ? "Around you: each window turns to face you."
                     : Math.round(riftSettings.windowCurve * 100) + " %: windows turn partly toward you.")
                    + " For windows you open or move from now on."
            ValueSlider {
                stepSize: 5
                value: riftSettings.windowCurve * 100
                anchors.verticalCenter: parent.verticalCenter
                onReleased: v => riftSettings.windowCurve = v / 100
            }
        }
    }

    Card {
        heading: "Room"
        // The room around you now and the way to change it: the rooms have a chooser of their own, so this page stays
        // short however many wallpapers there are.
        Item {
            id: currentRoom
            readonly property var room: riftSettings.photos.find(p => p.path === riftSettings.roomPhoto) || null
            readonly property string name: riftSettings.roomPhoto.length === 0 ? "None"
                : room ? room.name : riftSettings.roomPhoto.replace(/^.*\//, "").replace(/\.[^.]*$/, "")
            readonly property string kind: riftSettings.roomPhoto.length === 0 ? "No room: darkness around you."
                : room && room.hdr ? "HDR panorama, in real light."
                : room && room.group === "wallpapers" ? "A desktop wallpaper."
                : room && room.group === "yours" ? "Your picture."
                : "One of Rift Home's rooms."
            width: parent.width
            height: 208
            Rectangle {
                id: currentThumb
                x: 20
                y: 20
                width: 300
                height: 168
                radius: 14
                color: "#000000"
                clip: true
                Image {
                    anchors.fill: parent
                    visible: riftSettings.roomPhoto.length > 0
                    source: riftSettings.roomPhoto.length > 0 ? "image://room/" + riftSettings.roomPhoto : ""
                    sourceSize.width: 360
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                }
                HdrBadge {
                    visible: !!currentRoom.room && !!currentRoom.room.hdr
                }
            }
            Column {
                anchors.left: currentThumb.right
                anchors.leftMargin: 26
                anchors.right: parent.right
                anchors.rightMargin: 20
                anchors.verticalCenter: currentThumb.verticalCenter
                spacing: 6
                Text {
                    width: parent.width
                    text: currentRoom.name
                    color: app.text
                    font.pixelSize: 28
                    font.bold: true
                    elide: Text.ElideRight
                }
                Text {
                    width: parent.width
                    text: currentRoom.kind
                    color: app.subtext
                    font.pixelSize: 23
                    wrapMode: Text.WordWrap
                    bottomPadding: 12
                }
                PillButton {
                    text: "Change room"
                    primary: true
                    onClicked: roomSheet.openOn(currentRoom.room ? currentRoom.room.group : "riftHome")
                }
            }
        }
        SettingRow {
            label: "Brightness"
            detail: Math.round(riftSettings.roomBrightness * 100) + " %"
            ValueSlider {
                from: 20
                value: riftSettings.roomBrightness * 100
                anchors.verticalCenter: parent.verticalCenter
                onReleased: v => riftSettings.roomBrightness = v / 100
            }
        }
        SettingRow {
            // A 360-degree room turned for a better view.
            visible: riftSettings.roomPhoto.length > 0
            label: "Turn"
            detail: riftSettings.roomTurn === 0 ? "As taken" : Math.abs(riftSettings.roomTurn) + "° " + (riftSettings.roomTurn > 0 ? "right" : "left")
            ValueSlider {
                from: -180
                to: 180
                stepSize: 5
                value: riftSettings.roomTurn
                anchors.verticalCenter: parent.verticalCenter
                onReleased: v => riftSettings.roomTurn = v
            }
        }
    }
    Card {
        heading: "Room during a film"
        Row {
            padding: 20
            spacing: 20
            Choice {
                label: "Dark"
                swatch: "#000000"
                chosen: riftSettings.cinemaRoom === "dark"
                onPicked: riftSettings.cinemaRoom = "dark"
            }
            Choice {
                label: "Dim"
                swatch: "#2b2620"
                chosen: riftSettings.cinemaRoom === "dim"
                onPicked: riftSettings.cinemaRoom = "dim"
            }
            Choice {
                label: "Soft"
                swatch: "#6a5d4c"
                chosen: riftSettings.cinemaRoom === "soft"
                onPicked: riftSettings.cinemaRoom = "soft"
            }
        }
    }
    // A small round choice (a colour, a size, a time): highlighted when chosen.
    component Pick: Rectangle {
        id: pick
        property string label
        property bool chosen
        property color swatch: "transparent"
        signal picked
        width: Math.max(112, caption.implicitWidth + 40)
        height: 58
        radius: 29
        color: chosen ? app.selected : "transparent"
        border.color: chosen ? app.accent : app.line
        border.width: chosen ? 3 : 1
        Row {
            anchors.centerIn: parent
            spacing: 10
            Rectangle {
                visible: pick.swatch.a > 0
                width: 26
                height: 26
                radius: 13
                color: pick.swatch
                border.color: app.line
                anchors.verticalCenter: parent.verticalCenter
            }
            Text {
                id: caption
                text: pick.label
                color: app.text
                font.pixelSize: 22
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        TapHandler {
            enabled: !app.tapsBlocked(pick)
            onTapped: pick.picked()
        }
    }

    Card {
        heading: "Pointer"
        Column {
            width: parent.width
            padding: 20
            spacing: 18
            Flow {
                width: parent.width - 40
                spacing: 12
                Repeater {
                    model: [{ key: "ice", name: "Ice", colour: "#d9ebff" }, { key: "white", name: "White", colour: "#ffffff" },
                        { key: "green", name: "Green", colour: "#6eff96" }, { key: "amber", name: "Amber", colour: "#ffb45a" },
                        { key: "pink", name: "Pink", colour: "#ff78d7" }]
                    delegate: Pick {
                        required property var modelData
                        label: modelData.name
                        swatch: modelData.colour
                        chosen: riftSettings.pointerColour === modelData.key
                        onPicked: riftSettings.pointerColour = modelData.key
                    }
                }
            }
            Row {
                spacing: 12
                Text {
                    text: "Thickness"
                    color: app.text
                    font.pixelSize: 23
                    width: 190
                    anchors.verticalCenter: parent.verticalCenter
                }
                Repeater {
                    model: [{ key: "thin", name: "Thin" }, { key: "normal", name: "Normal" }, { key: "thick", name: "Thick" }]
                    delegate: Pick {
                        required property var modelData
                        label: modelData.name
                        chosen: riftSettings.pointerThickness === modelData.key
                        onPicked: riftSettings.pointerThickness = modelData.key
                    }
                }
            }
            Row {
                spacing: 12
                Text {
                    text: "Hide when still"
                    color: app.text
                    font.pixelSize: 23
                    width: 190
                    anchors.verticalCenter: parent.verticalCenter
                }
                Repeater {
                    model: [{ seconds: 0, name: "Never" }, { seconds: 3, name: "After 3 s" }, { seconds: 10, name: "After 10 s" }]
                    delegate: Pick {
                        required property var modelData
                        label: modelData.name
                        chosen: riftSettings.pointerHideAfter === modelData.seconds
                        onPicked: riftSettings.pointerHideAfter = modelData.seconds
                    }
                }
            }
        }
    }
    // How big apps' windows open: their text is that much bigger in the room.
    // Apps' scale (apps drew at 1x and enlarged windows showed their pixels) and the runtime's picture
    // size (supersampling: everything is drawn bigger than the panel, then averaged down to it), for GPUs that can.
    Card {
        heading: "Sharpness"
        SettingRow {
            label: "Apps' scale"
            detail: Math.round(riftSettings.appScale * 100) + " %: apps draw their text and buttons this big, with that many more pixels. Open apps change at once."
            ValueSlider {
                from: 100
                to: 300
                stepSize: 25
                value: riftSettings.appScale * 100
                anchors.verticalCenter: parent.verticalCenter
                onReleased: v => riftSettings.appScale = v / 100
            }
        }
        SettingRow {
            label: "Picture quality"
            detail: riftSettings.composition + " % of the headset's pixels, averaged down to them (165 % standard; more needs a faster GPU). From the next start."
            ValueSlider {
                from: 165
                to: 200
                stepSize: 5
                value: riftSettings.composition
                anchors.verticalCenter: parent.verticalCenter
                onReleased: v => riftSettings.composition = v
            }
        }
    }
    // The lens test, as Oculus' "Adjust Your Lenses": lines to make sharp by where the headset sits and the lens slider.
    Card {
        heading: "Lenses"
        SettingRow {
            label: "Lens adjustment"
            detail: riftSettings.lensTestNote.length > 0 ? riftSettings.lensTestNote
                                                         : "Green lines in the headset: move it on your face and the lens slider until they are clear. Any button ends it."
            separator: false
            PillButton {
                text: "Adjust"
                anchors.verticalCenter: parent.verticalCenter
                onClicked: riftSettings.showLensTest()
            }
        }
    }
    // X11 apps draw at desktop size unless told (Steam barely readable): their size here.
    Card {
        heading: "Older apps"
        Column {
            width: parent.width
            padding: 20
            spacing: 14
            Text {
                width: parent.width - 40
                text: "Steam and other X11 apps, from their next start (quit Steam fully and open it again)."
                color: app.subtext
                font.pixelSize: 23
                wrapMode: Text.WordWrap
            }
            Row {
                spacing: 12
                Text {
                    text: "Size"
                    color: app.text
                    font.pixelSize: 23
                    width: 190
                    anchors.verticalCenter: parent.verticalCenter
                }
                Repeater {
                    model: [{ scale: 1.0, name: "100 %" }, { scale: 1.5, name: "150 %" }, { scale: 2.0, name: "200 %" }]
                    delegate: Pick {
                        required property var modelData
                        label: modelData.name
                        chosen: Math.abs(riftSettings.x11Scale - modelData.scale) < 0.01
                        onPicked: riftSettings.x11Scale = modelData.scale
                    }
                }
            }
        }
    }
    // The room chooser: Rift Home's rooms, yours from Pictures/RiftHome and the desktop's wallpapers,
    // one kind at a time in a grid that scrolls (the thumbstick, or the bar at its side). A room shows around you as
    // soon as it is tapped; Done closes.
    Sheet {
        id: roomSheet
        property string group: "riftHome"
        readonly property var rooms: (group === "riftHome" ? [{ path: "", name: "None", group: "riftHome" }] : [])
            .concat(riftSettings.photos.filter(p => p.group === group))
        function openOn(kind) {
            group = kind || "riftHome"
            riftSettings.findPhotos() // pictures added since the page opened
            open()
            Qt.callLater(() => roomGrid.positionViewAtIndex(Math.max(0, roomGrid.model.findIndex(r => r.path === riftSettings.roomPhoto)), GridView.Contain))
        }
        width: 1150
        title: "Choose a room"
        // The kinds of room, one at a time; Done beside them.
        Item {
            width: parent.width
            height: 54
            Row {
                spacing: 12
                Repeater {
                    model: [{ group: "riftHome", label: "Rift Home" },
                        { group: "yours", label: "Yours" },
                        { group: "wallpapers", label: "Desktop wallpapers" }]
                    delegate: PillButton {
                        required property var modelData
                        text: modelData.label
                        primary: roomSheet.group === modelData.group
                        onClicked: {
                            roomSheet.group = modelData.group
                            roomGrid.positionViewAtBeginning()
                        }
                    }
                }
            }
            PillButton {
                anchors.right: parent.right
                text: "Done"
                primary: true
                onClicked: roomSheet.close()
            }
        }
        // Two lines whatever the kind: the sheet keeps its size between them.
        Text {
            width: parent.width
            height: 58
            text: roomSheet.group === "yours"
                ? "Your pictures go in " + riftSettings.userRoomsName + " and show here at once (Add your own opens it). A 2:1 panorama surrounds you; any other picture becomes a room in front of you."
                : roomSheet.group === "wallpapers"
                ? (roomSheet.rooms.length > 0 ? "This computer's desktop wallpapers: tap one to see it around you. A wallpaper becomes a room in front of you, its colours all around."
                                              : "No desktop wallpapers were found on this computer.")
                : "Tap a room to see it around you at once. None leaves you in darkness; HDR panoramas surround you in real light."
            color: app.subtext
            font.pixelSize: 23
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }
        GridView {
            id: roomGrid
            width: parent.width
            height: 430 // two rows and the start of the third: more below shows
            cellWidth: Math.floor((width - 18) / 4)
            cellHeight: 206
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            model: roomSheet.group === "yours" ? [{ add: true }].concat(roomSheet.rooms) : roomSheet.rooms
            ScrollBar.vertical: ScrollBar {
                policy: roomGrid.contentHeight > roomGrid.height ? ScrollBar.AlwaysOn : ScrollBar.AlwaysOff
                width: 16
                contentItem: Rectangle {
                    implicitWidth: 10
                    radius: 5
                    color: app.subtext
                    opacity: 0.85
                }
            }
            delegate: Item {
                id: cell
                required property var modelData
                width: roomGrid.cellWidth
                height: roomGrid.cellHeight
                AddRoomsTile {
                    visible: !!cell.modelData.add
                }
                RoomTile {
                    visible: !cell.modelData.add
                    modelData: cell.modelData.add ? { path: "-", name: "" } : cell.modelData
                }
            }
        }
    }
    Text {
        width: parent.width
        text: "Change room offers Rift Home's rooms, your pictures from " + riftSettings.userRoomsName + " and the desktop's wallpapers. A 2:1 panorama surrounds you (HDR ones in real light); any other picture becomes a room in front of you. In cinema mode the room goes dark, dim or stays softly lit. A hidden pointer comes back as soon as you move the controller or press a button."
        color: app.subtext
        font.pixelSize: 23
        wrapMode: Text.WordWrap
        leftPadding: 18
    }
}
