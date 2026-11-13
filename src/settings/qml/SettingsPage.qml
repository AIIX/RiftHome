// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: Apache-2.0
import QtQuick
import QtQuick.Controls

// A page: its title, then its cards, scrolling when taller than the window.
Flickable {
    id: page
    property string title
    default property alias content: column.data
    contentHeight: column.implicitHeight + 60
    clip: true
    boundsBehavior: Flickable.StopAtBounds
    ScrollBar.vertical: ScrollBar {}

    Column {
        id: column
        x: 36
        y: 30
        width: page.width - 72
        spacing: 18
        Text {
            text: page.title
            color: app.text
            font.pixelSize: 40
            font.bold: true
            bottomPadding: 6
        }
    }
}
