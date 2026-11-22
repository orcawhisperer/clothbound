import QtQuick

import "Ui.js" as Ui

// A cover for books that came without one: cloth in a color picked from the
// title, with the title and author set in Literata between two rules.
Rectangle {
    id: generated

    property string title
    property string author

    readonly property var cloths: ["#34445e", "#5b3a46", "#3c5a48", "#5c4a33", "#463d5e", "#335659", "#5e4136", "#4a5233"]
    readonly property color cloth: cloths[Ui.hash(title + author) % cloths.length]
    readonly property color ink: "#f1e7d0"
    readonly property color gilt: "#d6b36a"

    color: cloth
    clip: true

    // The spine's shadow on the cloth.
    Rectangle {
        width: parent.width * 0.06
        height: parent.height
        color: Qt.darker(generated.cloth, 1.35)
    }

    Column {
        anchors.centerIn: parent
        anchors.horizontalCenterOffset: parent.width * 0.03
        width: parent.width * 0.78
        spacing: parent.height * 0.035

        Rectangle { width: parent.width; height: Math.max(1, generated.height * 0.006); color: generated.gilt }

        Text {
            width: parent.width
            text: generated.title
            color: generated.ink
            font.family: "Literata"
            font.pixelSize: Math.max(8, Math.round(generated.width * 0.105))
            font.weight: Font.DemiBold
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            maximumLineCount: 5
            elide: Text.ElideRight
            lineHeight: 1.05
        }

        Text {
            width: parent.width
            visible: generated.author !== ""
            text: generated.author.toUpperCase()
            color: generated.gilt
            font.family: "Literata"
            font.pixelSize: Math.max(6, Math.round(generated.width * 0.055))
            font.letterSpacing: generated.width * 0.006
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
            maximumLineCount: 2
            elide: Text.ElideRight
        }

        Rectangle { width: parent.width; height: Math.max(1, generated.height * 0.006); color: generated.gilt }
    }
}
