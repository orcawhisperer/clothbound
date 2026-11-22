import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui

// One choice in a small menu, with a check when it's the current one.
AbstractButton {
    id: row

    property string hint

    width: parent ? parent.width : 200
    implicitHeight: 34
    hoverEnabled: true
    focusPolicy: Qt.NoFocus

    background: Rectangle {
        radius: Math.min(6, theme.radius)
        color: row.hovered ? Ui.tint(theme.text, 0.08) : "transparent"
    }
    contentItem: Item {
        Glyph {
            id: mark
            anchors.left: parent.left
            anchors.leftMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            width: 16
            height: 16
            stroke: 2.2
            path: Icons.check
            color: theme.accent
            visible: row.checked
        }
        Label {
            anchors.left: parent.left
            anchors.leftMargin: 32
            anchors.right: hintLabel.left
            anchors.verticalCenter: parent.verticalCenter
            text: row.text
            color: theme.text
            font.pixelSize: 13
            elide: Text.ElideRight
        }
        Label {
            id: hintLabel
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            text: row.hint
            color: theme.textTertiary
            font.pixelSize: 12
            font.family: "monospace"
        }
    }
}
