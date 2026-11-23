import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui

// One work in the library grid: its cover (stacked when there are several
// editions), how far it's read, and its title.
Item {
    id: card

    required property int index
    required property string key
    required property int bookId
    required property string title
    required property string author
    required property string editionLine
    required property int editions
    required property string cover
    required property real progress
    required property bool finished
    required property string format
    required property string problem
    required property string series
    required property real seriesIndex

    readonly property bool current: GridView.isCurrentItem
    readonly property bool focused: current && GridView.view.activeFocus
    readonly property real pad: 14

    signal activated()
    signal detailsRequested()

    width: GridView.view.cellWidth
    height: GridView.view.cellHeight

    Item {
        id: coverArea
        x: card.pad
        y: card.pad
        width: card.width - 2 * card.pad
        height: width * 1.5
        scale: hover.hovered ? 1.025 : 1
        Behavior on scale { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }

        // Further editions, as covers stacked behind.
        Repeater {
            model: Math.min(card.editions - 1, 2)
            Rectangle {
                required property int index
                x: art.box.x + 6 * (index + 1)
                y: art.box.y - 6 * (index + 1)
                width: art.box.width
                height: art.box.height
                radius: art.radius
                z: -1 - index
                color: Ui.mix(theme.raised, theme.ground, 0.25 * index)
                border.color: Ui.tint(theme.ink1, 0.08)
            }
        }

        // A hard shadow, as Omarchy's apps draw them.
        Rectangle {
            x: art.box.x
            y: art.box.y + 5
            width: art.box.width
            height: art.box.height
            radius: art.radius
            color: "black"
            opacity: theme.light ? 0.16 : 0.38
        }

        CoverImage {
            id: art
            anchors.fill: parent
            source: card.cover
            title: card.title
            author: card.author
        }

        // How far it's read, along the bottom of the cover.
        Item {
            visible: card.progress > 0.005 && !card.finished
            x: art.box.x
            y: art.box.y + art.box.height - 4
            width: art.box.width
            height: 4
            Rectangle { anchors.fill: parent; color: "black"; opacity: 0.45 }
            Rectangle { width: parent.width * card.progress; height: parent.height; color: theme.accent }
        }

        Rectangle {
            visible: card.finished
            x: art.box.x + art.box.width - width - 6
            y: art.box.y + 6
            width: 22
            height: 22
            radius: 11
            color: theme.accent
            Glyph {
                anchors.centerIn: parent
                width: 14
                height: 14
                stroke: 2.4
                path: Icons.check
                color: theme.accentForeground
            }
        }

        Label {
            visible: card.format === "pdf"
            x: art.box.x + 6
            y: art.box.y + art.box.height - height - 10
            text: "PDF"
            font.pixelSize: 9
            font.weight: Font.Bold
            font.letterSpacing: 0.6
            color: "white"
            leftPadding: 5
            rightPadding: 5
            topPadding: 2
            bottomPadding: 2
            background: Rectangle { radius: Math.min(3, theme.radius); color: "black"; opacity: 0.55 }
        }

        Label {
            visible: card.series !== "" && card.seriesIndex > 0
            x: art.box.x + 6
            y: art.box.y + 6
            text: "#" + (Math.floor(card.seriesIndex) === card.seriesIndex ? Math.floor(card.seriesIndex) : card.seriesIndex)
            font.pixelSize: 10
            font.weight: Font.DemiBold
            color: "white"
            leftPadding: 6
            rightPadding: 6
            topPadding: 2
            bottomPadding: 2
            background: Rectangle { radius: Math.min(4, theme.radius); color: "black"; opacity: 0.6 }
        }

        Rectangle {
            visible: card.focused
            x: art.box.x - 5
            y: art.box.y - 5
            width: art.box.width + 10
            height: art.box.height + 10
            radius: art.radius + (art.radius > 0 ? 4 : 0)
            color: "transparent"
            border.width: 2
            border.color: theme.accent
        }
    }

    Column {
        anchors.top: coverArea.bottom
        anchors.topMargin: 10
        x: card.pad
        width: coverArea.width
        spacing: 2

        Label {
            width: parent.width
            text: card.title
            textFormat: Text.PlainText
            color: card.problem !== "" ? theme.ink3 : theme.ink1
            font.pixelSize: 13
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
            maximumLineCount: 2
            elide: Text.ElideRight
            lineHeight: 1.1
        }
        Label {
            width: parent.width
            text: card.problem !== "" ? card.problem
                : card.editions > 1 ? qsTr("%n editions", "", card.editions) : card.author
            textFormat: Text.PlainText
            color: card.editions > 1 && card.problem === "" ? theme.accentInk : theme.ink2
            font.pixelSize: 12
            elide: Text.ElideRight
        }
    }

    HoverHandler {
        id: hover
        cursorShape: Qt.PointingHandCursor
    }
    TapHandler {
        onTapped: {
            card.GridView.view.currentIndex = card.index
            card.activated()
        }
    }
    TapHandler {
        acceptedButtons: Qt.RightButton
        onTapped: {
            card.GridView.view.currentIndex = card.index
            card.detailsRequested()
        }
    }
}
