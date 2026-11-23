import QtQuick
import QtQuick.Controls
import "../components"

import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui

// One work in the library grid: its 2:3 cover (stacked when there are several
// editions), subtle top-right PDF / finished badges, single-line title, and a
// calm tabular status/meta line underneath (01_library.html).
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
    required property int pages
    required property bool wantToRead
    required property bool listening

    readonly property bool current: GridView.isCurrentItem
    readonly property bool focused: current && GridView.view.activeFocus
    readonly property real pad: 12

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
        scale: hover.hovered ? 1.02 : 1
        Behavior on scale {
            enabled: !theme.reducedMotion
            NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
        }

        // Further editions, as covers stacked behind.
        Repeater {
            model: Math.min(card.editions - 1, 2)
            Rectangle {
                required property int index
                x: art.box.x + 5 * (index + 1)
                y: art.box.y - 5 * (index + 1)
                width: art.box.width
                height: art.box.height
                radius: art.radius
                z: -1 - index
                color: Ui.mix(theme.raised, theme.ground, 0.25 * index)
                border.color: Ui.tint(theme.ink1, 0.08)
            }
        }

        // Grounded book shadow.
        Rectangle {
            x: art.box.x
            y: art.box.y + 6
            width: art.box.width
            height: art.box.height
            radius: art.radius
            color: "black"
            opacity: theme.light ? 0.16 : 0.42
        }

        CoverImage {
            id: art
            anchors.fill: parent
            source: card.cover
            title: card.title
            author: card.author
        }

        // Top-right Finished badge: quiet 18×18 translucent circle with check.
        Rectangle {
            id: finishedBadge
            visible: card.finished
            x: art.box.x + art.box.width - width - 8
            y: art.box.y + 8
            width: 18
            height: 18
            radius: 9
            color: Qt.rgba(0, 0, 0, 0.55)
            Accessible.name: qsTr("Finished")
            Glyph {
                anchors.centerIn: parent
                width: 10
                height: 10
                stroke: 2.8
                path: Icons.check
                color: "#F2F2F3"
            }
        }

        // Top-right PDF badge (shifts left beside the finished check if both show).
        Label {
            visible: card.format === "pdf"
            x: art.box.x + art.box.width - width - (card.finished ? 30 : 8)
            y: art.box.y + 8
            text: "PDF"
            font.pixelSize: 9
            font.weight: Font.Bold
            font.letterSpacing: 0.4
            color: "#F2F2F3"
            leftPadding: 5
            rightPadding: 5
            topPadding: 2
            bottomPadding: 2
            background: Rectangle {
                radius: Ui.radius(4, theme.radius)
                color: Qt.rgba(0, 0, 0, 0.55)
            }
        }

        // Top-left series number badge (preserved Dogear feature).
        Label {
            visible: card.series !== "" && card.seriesIndex > 0
            x: art.box.x + 8
            y: art.box.y + 8
            text: "#" + (Math.floor(card.seriesIndex) === card.seriesIndex ? Math.floor(card.seriesIndex) : card.seriesIndex)
            font.pixelSize: 10
            font.weight: Font.DemiBold
            color: "#F2F2F3"
            leftPadding: 6
            rightPadding: 6
            topPadding: 2
            bottomPadding: 2
            background: Rectangle {
                radius: Ui.radius(4, theme.radius)
                color: Qt.rgba(0, 0, 0, 0.58)
            }
        }

        // Keyboard focus ring around the cover.
        Rectangle {
            visible: card.focused
            x: art.box.x - 4
            y: art.box.y - 4
            width: art.box.width + 8
            height: art.box.height + 8
            radius: Ui.radius(art.radius + 4, theme.radius)
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
        spacing: 3

        Label {
            width: parent.width
            text: card.title
            textFormat: Text.PlainText
            color: card.problem !== "" ? theme.ink3 : theme.ink1
            font.pixelSize: 13
            font.weight: Font.Medium
            maximumLineCount: 1
            elide: Text.ElideRight
        }
        Label {
            width: parent.width
            text: {
                const _lang = translator.language
                if (card.problem !== "")
                    return card.problem
                if (card.editions > 1)
                    return qsTr("%n editions", "", card.editions)
                if (card.finished)
                    return qsTr("Finished")
                if (card.progress > 0.005) {
                    const pct = Math.round(card.progress * 100).toLocaleString(Qt.locale(), "f", 0)
                    return card.listening ? qsTr("%1% · listening").arg(pct) : qsTr("%1%").arg(pct)
                }
                if (card.wantToRead)
                    return qsTr("Want to read")
                if (card.format === "pdf" && card.pages > 0)
                    return qsTr("PDF · %n pages", "", card.pages)
                return card.author !== "" ? card.author : qsTr("New")
            }
            textFormat: Text.PlainText
            color: card.problem !== "" ? theme.danger
                 : card.editions > 1 ? theme.accentInk : theme.ink4
            font.pixelSize: 12
            font.features: ({ "tnum": 1 })
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
