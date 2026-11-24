import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// A keyboard key badge: compact inline ("Ctrl K", "↵") or a tactile 28×22
// physical keycap with a 2 px bottom lip (`keycap: true`). Set `onDark: true`
// when sitting on an always-dark HUD over a light reading page.
Rectangle {
    id: hint

    property string text
    property bool onAccent: false
    property bool keycap: false
    property bool onDark: false

    readonly property color borderTone: onDark ? Qt.rgba(1, 1, 1, keycap ? 0.18 : 0.14)
                                      : onAccent ? Ui.tint(palette.highlightedText, 0.22)
                                      : Ui.tint(palette.windowText, keycap ? 0.14 : 0.12)
    readonly property color fillTone: onDark ? Qt.rgba(1, 1, 1, keycap ? 0.06 : 0.04)
                                    : onAccent ? Ui.tint(palette.highlightedText, 0.14)
                                    : Ui.tint(palette.windowText, keycap ? 0.04 : 0.05)
    readonly property color textTone: onDark ? "#E4E4E7"
                                    : onAccent ? palette.highlightedText
                                    : keycap ? palette.windowText : palette.mid

    implicitWidth: keycap ? Math.max(28, label.implicitWidth + 12) : Math.max(18, label.implicitWidth + 10)
    implicitHeight: keycap ? 22 : 18
    radius: Ui.radius(keycap ? 5 : 4, theme.radius)
    color: fillTone
    border.width: 1
    border.color: borderTone

    // Tactile 2 px bottom edge on physical keycaps.
    Rectangle {
        visible: hint.keycap
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: Math.max(1, hint.radius * 0.4)
        anchors.rightMargin: Math.max(1, hint.radius * 0.4)
        anchors.bottomMargin: 1
        height: 1
        color: hint.borderTone
    }

    Label {
        id: label
        anchors.centerIn: parent
        anchors.verticalCenterOffset: hint.keycap ? -0.5 : 0
        text: hint.text
        color: hint.textTone
        font.pixelSize: 11
        font.weight: hint.keycap ? Font.DemiBold : Font.Medium
        font.features: { "tnum": 1 }
    }
}
