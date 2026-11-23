import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui

// One choice in a small menu, with a check when it's the current one.
AbstractButton {
    id: row

    property string hint
    // Kept for older callers; the reading theme now arrives as the palette.
    property var look: null

    readonly property color inkColor: palette.windowText
    readonly property color mutedColor: palette.mid
    readonly property color accentColor: palette.link

    width: parent ? parent.width : 200
    implicitHeight: 30
    hoverEnabled: true
    focusPolicy: Qt.NoFocus
    Accessible.name: text

    background: Rectangle {
        radius: Math.min(6, theme.radius)
        color: row.pressed ? Ui.tint(row.inkColor, 0.12) : row.hovered ? Ui.tint(row.inkColor, 0.07) : "transparent"
    }
    contentItem: Item {
        Glyph {
            id: mark
            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 14
            height: 14
            stroke: 2.2
            path: Icons.check
            color: row.accentColor
            visible: row.checked
        }
        Label {
            anchors.left: parent.left
            anchors.leftMargin: 28
            anchors.right: hintLabel.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: row.text
            color: row.inkColor
            font.pixelSize: 13
            font.weight: row.checked ? Font.Medium : Font.Normal
            elide: Text.ElideRight
        }
        KeyHint {
            id: hintLabel
            anchors.right: parent.right
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            text: row.hint
            visible: row.hint !== ""
            width: visible ? implicitWidth : 0
        }
    }
}
