import QtQuick

import "Ui.js" as Ui

// A cover for books that came without one: editorial cloth in a curated light
// or dark pair picked from the title, with Literata title on top, a short rule,
// and uppercase author along the bottom (01_library.html).
Rectangle {
    id: generated

    property string title
    property string author

    readonly property var cloths: [
        { bg: "#1F3550", fg: "#E9D8A6" },
        { bg: "#6B2E2A", fg: "#F1E4D0" },
        { bg: "#CDBE9A", fg: "#2A251D" },
        { bg: "#2F4A3A", fg: "#E6DCC3" },
        { bg: "#3D3350", fg: "#E7DCC8" },
        { bg: "#9A6A30", fg: "#F7EBD6" },
        { bg: "#22403F", fg: "#E3D6B8" },
        { bg: "#50443A", fg: "#EADFCB" },
        { bg: "#2B2B36", fg: "#D9C7A0" },
        { bg: "#B9C4C9", fg: "#1E2A30" },
        { bg: "#5B6B2E", fg: "#F0E9CF" },
        { bg: "#7A3F55", fg: "#F3E2E6" }
    ]
    readonly property var pair: cloths[Ui.hash((title || "") + (author || "")) % cloths.length]
    readonly property color cloth: pair.bg
    readonly property color ink: pair.fg
    readonly property real padX: Math.max(5, Math.round(width * 0.085))
    readonly property real padY: Math.max(6, Math.round(height * 0.075))
    readonly property bool tiny: width < 52

    color: cloth
    clip: true

    // Top-aligned title in Literata (clears top corner badges on library cards).
    Text {
        anchors.top: parent.top
        anchors.topMargin: generated.tiny ? generated.padY : Math.min(32, Math.max(generated.padY, Math.round(generated.height * 0.145)))
        anchors.left: parent.left
        anchors.leftMargin: generated.padX + 2
        anchors.right: parent.right
        anchors.rightMargin: generated.padX
        text: generated.title
        textFormat: Text.PlainText
        color: generated.ink
        font.family: "Literata"
        font.pixelSize: Math.max(7, Math.round(generated.width * (generated.tiny ? 0.16 : 0.105)))
        font.weight: Font.DemiBold
        wrapMode: Text.WordWrap
        maximumLineCount: generated.tiny ? 3 : 4
        elide: Text.ElideRight
        lineHeight: 1.12
    }

    // Short editorial rule separating title and author.
    Rectangle {
        visible: !generated.tiny
        anchors.left: parent.left
        anchors.leftMargin: generated.padX + 2
        anchors.verticalCenter: parent.verticalCenter
        anchors.verticalCenterOffset: Math.round(generated.height * 0.12)
        width: Math.max(16, Math.round(generated.width * 0.25))
        height: 1
        color: generated.ink
        opacity: 0.58
    }

    // Bottom-aligned tracked uppercase author.
    Text {
        visible: !generated.tiny && generated.author !== ""
        anchors.bottom: parent.bottom
        anchors.bottomMargin: generated.padY
        anchors.left: parent.left
        anchors.leftMargin: generated.padX + 2
        anchors.right: parent.right
        anchors.rightMargin: generated.padX
        text: generated.author.toUpperCase()
        textFormat: Text.PlainText
        color: generated.ink
        opacity: 0.85
        font.pixelSize: Math.max(6, Math.round(generated.width * 0.06))
        font.letterSpacing: Math.max(0.5, generated.width * 0.0085)
        wrapMode: Text.WordWrap
        maximumLineCount: 2
        elide: Text.ElideRight
    }
}
