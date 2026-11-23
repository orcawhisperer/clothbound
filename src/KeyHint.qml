import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// A keyboard key, drawn as a small rounded badge: "Space", "Ctrl K".
Rectangle {
    id: hint

    property string text
    property bool onAccent: false

    implicitWidth: Math.max(18, label.implicitWidth + 10)
    implicitHeight: 18
    radius: Math.min(4, theme.radius)
    color: onAccent ? Ui.tint(palette.highlightedText, 0.14) : Ui.tint(palette.windowText, 0.07)
    border.width: 1
    border.color: onAccent ? Ui.tint(palette.highlightedText, 0.18) : Ui.tint(palette.windowText, 0.10)

    Label {
        id: label
        anchors.centerIn: parent
        text: hint.text
        color: hint.onAccent ? hint.palette.highlightedText : hint.palette.mid
        font.pixelSize: 11
        font.weight: Font.Medium
        font.features: { "tnum": 1 }
    }
}
