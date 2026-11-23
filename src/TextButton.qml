import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// A button with a word on it: filled with the accent when it's the thing to
// do, quiet otherwise.
AbstractButton {
    id: button

    property bool primary: false
    property string glyph
    property string hint
    property var look: null

    readonly property color inkColor: look ? (look.fg || look.ink || theme.text) : theme.text
    readonly property color mutedColor: look ? (look.muted || theme.textTertiary) : theme.textTertiary
    readonly property color accentColor: look ? (look.link || theme.accent) : theme.accent
    readonly property color accentFgColor: look ? Ui.readableOn(accentColor) : theme.accentForeground

    implicitHeight: 38
    implicitWidth: row.implicitWidth + 32
    hoverEnabled: true
    focusPolicy: Qt.NoFocus
    opacity: enabled ? 1 : 0.42

    background: Rectangle {
        radius: Math.min(8, theme.radius)
        color: button.primary
             ? (button.enabled && button.pressed ? Qt.darker(button.accentColor, 1.15)
                : button.enabled && button.hovered ? Qt.lighter(button.accentColor, 1.08) : button.accentColor)
             : (button.enabled && button.pressed ? Ui.tint(button.inkColor, 0.16)
                : button.enabled && button.hovered ? Ui.tint(button.inkColor, 0.1) : Ui.tint(button.inkColor, 0.06))
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    contentItem: Item {
        Row {
            id: row
            anchors.centerIn: parent
            spacing: 8
            Glyph {
                visible: button.glyph !== ""
                anchors.verticalCenter: parent.verticalCenter
                width: 18
                height: 18
                path: button.glyph
                color: button.primary ? button.accentFgColor : button.inkColor
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: button.text
                color: button.primary ? button.accentFgColor : button.inkColor
                font.pixelSize: 14
                font.weight: button.primary ? Font.DemiBold : Font.Normal
            }
            Label {
                visible: button.hint !== ""
                anchors.verticalCenter: parent.verticalCenter
                text: button.hint
                color: button.primary ? button.accentFgColor : button.mutedColor
                opacity: 0.7
                font.pixelSize: 12
                font.family: "monospace"
            }
        }
    }
}
