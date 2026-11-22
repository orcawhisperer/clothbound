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

    implicitHeight: 38
    implicitWidth: row.implicitWidth + 32
    hoverEnabled: true
    focusPolicy: Qt.NoFocus

    background: Rectangle {
        radius: Math.min(8, theme.radius)
        color: button.primary ? (button.pressed ? Qt.darker(theme.accent, 1.15) : button.hovered ? Qt.lighter(theme.accent, 1.08) : theme.accent)
             : button.pressed ? Ui.tint(theme.text, 0.16) : button.hovered ? Ui.tint(theme.text, 0.1) : Ui.tint(theme.text, 0.06)
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
                color: button.primary ? theme.accentForeground : theme.text
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: button.text
                color: button.primary ? theme.accentForeground : theme.text
                font.pixelSize: 14
                font.weight: button.primary ? Font.DemiBold : Font.Normal
            }
            Label {
                visible: button.hint !== ""
                anchors.verticalCenter: parent.verticalCenter
                text: button.hint
                color: button.primary ? theme.accentForeground : theme.textTertiary
                opacity: 0.7
                font.pixelSize: 12
                font.family: "monospace"
            }
        }
    }
}
