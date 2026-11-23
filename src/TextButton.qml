import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// A button with a word on it: filled with the accent when `primary`, subtle
// neutral fill otherwise, transparent text when `ghost`. `size` is "compact"
// (28 px), "normal" (30 px), or "large" (34 px). `hint` shows its key badge.
AbstractButton {
    id: button

    property bool primary: false
    property bool ghost: false
    property string size: "normal"
    property string glyph
    property string hint
    // Kept for reading-surface callers passing a Themes.palette() look object.
    property var look: null

    readonly property color inkColor: look ? look.fg : palette.buttonText
    readonly property color mutedColor: look ? look.muted : palette.mid
    readonly property color accentColor: look ? look.link : palette.highlight
    readonly property color accentFgColor: look ? Ui.readableOn(look.link)
                                         : (Qt.colorEqual(palette.highlightedText, "#000000") ? "#0b0d12" : palette.highlightedText)
    readonly property int hPad: size === "compact" ? 10 : (primary ? 14 : 12)

    implicitHeight: size === "compact" ? 28 : size === "large" ? 34 : 30
    implicitWidth: row.implicitWidth + hPad * 2
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    opacity: enabled ? 1 : 0.42
    Accessible.name: text

    background: Rectangle {
        radius: Ui.radius(7, theme.radius)
        color: button.primary
             ? (button.enabled && button.pressed ? Qt.darker(button.accentColor, 1.12)
                : button.enabled && button.hovered ? Qt.lighter(button.accentColor, 1.06) : button.accentColor)
             : button.ghost
             ? (button.enabled && button.pressed ? Ui.tint(button.inkColor, 0.14)
                : button.enabled && button.hovered ? Ui.tint(button.inkColor, 0.08) : "transparent")
             : (button.enabled && button.pressed ? Ui.tint(button.inkColor, 0.16)
                : button.enabled && button.hovered ? Ui.tint(button.inkColor, 0.12) : Ui.tint(button.inkColor, 0.08))
        Behavior on color { ColorAnimation { duration: 100 } }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            visible: button.visualFocus
            radius: Ui.radius(10, theme.radius)
            color: "transparent"
            border.width: 2
            border.color: button.accentColor
        }
    }

    contentItem: Item {
        Row {
            id: row
            anchors.centerIn: parent
            spacing: 6
            Glyph {
                visible: button.glyph !== ""
                anchors.verticalCenter: parent.verticalCenter
                width: button.size === "compact" ? 14 : 15
                height: button.size === "compact" ? 14 : 15
                path: button.glyph
                color: button.primary ? button.accentFgColor
                     : button.ghost && !button.hovered ? button.mutedColor : button.inkColor
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: button.text
                color: button.primary ? button.accentFgColor
                     : button.ghost && !button.hovered ? button.mutedColor : button.inkColor
                font.pixelSize: button.size === "compact" ? 12 : 13
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
