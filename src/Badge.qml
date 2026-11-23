import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// Where something runs or came from, as a small dot and words: green on
// this computer, amber sent to Google. `where` is "local", "cloud", or
// "none" (muted); `text` overrides the default wording.
Rectangle {
    id: badge

    property string where: "local"
    property string text: where === "local" ? qsTr("On this computer")
                        : where === "cloud" ? qsTr("Sent to Google") : ""
    property bool filled: true

    readonly property color tone: where === "local" ? theme.positive
                                : where === "cloud" ? theme.cloud : palette.mid

    implicitWidth: row.implicitWidth + (filled ? 14 : 0)
    implicitHeight: 20
    radius: height / 2
    color: filled ? Ui.tint(tone, 0.12) : "transparent"
    Accessible.name: text

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 6
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 6
            height: 6
            radius: 3
            color: badge.tone
        }
        Label {
            anchors.verticalCenter: parent.verticalCenter
            text: badge.text
            color: badge.tone
            font.pixelSize: 11
            font.weight: Font.Medium
        }
    }
}
