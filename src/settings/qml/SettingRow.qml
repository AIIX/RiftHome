// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick

// A row in a card: a label (and a smaller line under it) on the left, a control on the right; tappable.
Item {
    id: row
    property string label
    property string detail
    property color labelColor: app.text
    property bool separator: true
    property bool tappable: false
    default property alias trailing: slot.data
    signal tapped
    width: parent ? parent.width : 0
    height: Math.max(app.rowHeight, texts.implicitHeight + 24)

    Rectangle {
        anchors.fill: parent
        anchors.margins: 4
        radius: 16
        color: app.selected
        visible: row.tappable && hover.hovered
    }
    Column {
        id: texts
        anchors.left: parent.left
        anchors.leftMargin: 24
        anchors.right: slot.left
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        spacing: 2
        Text {
            width: parent.width
            text: row.label
            color: row.labelColor
            font.pixelSize: app.fontSize
            elide: Text.ElideRight
        }
        Text {
            width: parent.width
            visible: row.detail.length > 0
            text: row.detail
            color: app.subtext
            font.pixelSize: 23
            wrapMode: Text.WordWrap
        }
    }
    Row {
        id: slot
        anchors.right: parent.right
        anchors.rightMargin: 24
        anchors.verticalCenter: parent.verticalCenter
        spacing: 16
    }
    Rectangle {
        visible: row.separator
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.leftMargin: 24
        anchors.right: parent.right
        height: 1
        color: app.line
    }
    HoverHandler {
        id: hover
        enabled: row.tappable
    }
    TapHandler {
        enabled: row.tappable && !app.tapsBlocked(row)
        onTapped: row.tapped()
    }
}
