import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// A button with a word on it: filled with the accent when it's the thing to
// do, quiet otherwise. `hint` shows its key as a small badge.
AbstractButton {
    id: button

    property bool primary: false
    property string glyph
    property string hint
    // Kept for older callers; the reading theme now arrives as the palette.
    property var look: null

    readonly property color inkColor: palette.buttonText
    readonly property color mutedColor: palette.mid
    readonly property color accentColor: palette.highlight
    readonly property color accentFgColor: palette.highlightedText

    implicitHeight: 32
    implicitWidth: row.implicitWidth + 28
    hoverEnabled: true
    focusPolicy: Qt.NoFocus
    opacity: enabled ? 1 : 0.42
    Accessible.name: text

    background: Rectangle {
        radius: Math.min(7, theme.radius)
        color: button.primary
             ? (button.enabled && button.pressed ? Qt.darker(button.accentColor, 1.12)
                : button.enabled && button.hovered ? Qt.lighter(button.accentColor, 1.06) : button.accentColor)
             : (button.enabled && button.pressed ? Ui.tint(button.inkColor, 0.14)
                : button.enabled && button.hovered ? Ui.tint(button.inkColor, 0.10) : Ui.tint(button.inkColor, 0.06))
        border.width: button.visualFocus ? 2 : 0
        border.color: button.palette.link
        Behavior on color { ColorAnimation { duration: 100 } }
    }

    contentItem: Item {
        Row {
            id: row
            anchors.centerIn: parent
            spacing: 7
            Glyph {
                visible: button.glyph !== ""
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                path: button.glyph
                color: button.primary ? button.accentFgColor : button.inkColor
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: button.text
                color: button.primary ? button.accentFgColor : button.inkColor
                font.pixelSize: 13
                font.weight: button.primary ? Font.DemiBold : Font.Medium
            }
            KeyHint {
                visible: button.hint !== ""
                anchors.verticalCenter: parent.verticalCenter
                text: button.hint
                onAccent: button.primary
            }
        }
    }
}
