// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick

// A rounded group of rows, with an optional heading above it.
Column {
    id: card
    property string heading
    default property alias rows: inner.data
    width: parent ? parent.width : 0
    spacing: 8
    Text {
        visible: card.heading.length > 0
        text: card.heading.toUpperCase()
        color: app.subtext
        font.pixelSize: 22
        font.letterSpacing: 1
        leftPadding: 18
    }
    Rectangle {
        width: card.width
        height: inner.implicitHeight
        radius: 20
        color: app.card
        Column {
            id: inner
            width: parent.width
        }
    }
}
