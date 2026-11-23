import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// Where something runs or came from: green on this computer, amber sent to
// Google, or muted when unavailable. `where` is "local", "cloud", or "none";
// `showDot` adds a 6 px status dot ahead of the label.
Rectangle {
    id: badge

    property string where: "local"
    property string text: where === "local" ? qsTr("On this computer")
                        : where === "cloud" ? qsTr("Sent to Google") : ""
    property bool filled: true
    property bool showDot: false

    readonly property color tone: where === "local" ? theme.positive
                                : where === "cloud" ? theme.cloud : palette.mid
    readonly property color fillTone: where === "local" ? theme.positiveSoft
                                    : where === "cloud" ? theme.cloudSoft
                                    : Ui.tint(palette.windowText, 0.08)
    readonly property color inkTone: where === "local" ? theme.positiveInk
                                   : where === "cloud" ? theme.cloudInk : palette.mid

    implicitWidth: row.implicitWidth + (filled ? 16 : 0)
    implicitHeight: 20
    radius: height / 2
    color: filled ? fillTone : "transparent"
    Accessible.name: text

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6
        Rectangle {
            visible: badge.showDot
            anchors.verticalCenter: parent.verticalCenter
            width: 6
            height: 6
            radius: 3
            color: badge.tone
        }
        Label {
            anchors.verticalCenter: parent.verticalCenter
            text: badge.text
            color: badge.filled ? badge.inkTone : badge.tone
            font.pixelSize: 11
            font.weight: Font.DemiBold
        }
    }
}
