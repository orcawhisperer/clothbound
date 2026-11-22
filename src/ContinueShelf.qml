import QtQuick
import QtQuick.Controls

import "Format.js" as Format
import "Ui.js" as Ui

// The books under way, newest first, each a click from where it was left.
Column {
    id: shelf

    property var books: []

    signal open(int bookId)

    spacing: 14

    Label {
        text: qsTr("Continue reading")
        color: theme.text
        font.pixelSize: 15
        font.weight: Font.DemiBold
    }

    ListView {
        id: list
        width: shelf.width
        height: 150
        orientation: ListView.Horizontal
        spacing: 18
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: shelf.books

        delegate: Item {
            id: entry
            required property var modelData
            width: 330
            height: list.height

            Rectangle {
                anchors.fill: parent
                radius: Math.min(10, theme.radius)
                color: hover.hovered ? Ui.tint(theme.text, 0.07) : Ui.tint(theme.text, 0.035)
                Behavior on color { ColorAnimation { duration: 120 } }
            }

            Rectangle {
                x: thumb.x + thumb.box.x
                y: thumb.y + thumb.box.y + 4
                width: thumb.box.width
                height: thumb.box.height
                color: "black"
                opacity: theme.light ? 0.15 : 0.35
                radius: thumb.radius
            }
            CoverImage {
                id: thumb
                x: 14
                y: 12
                width: 84
                height: parent.height - 24
                source: entry.modelData.cover
                title: entry.modelData.title
                author: entry.modelData.author
            }

            Column {
                anchors.left: thumb.right
                anchors.leftMargin: 16
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                Label {
                    width: parent.width
                    text: entry.modelData.title
                    color: theme.text
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }
                Label {
                    width: parent.width
                    text: entry.modelData.author
                    color: theme.textSecondary
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Label {
                    width: parent.width
                    visible: text !== ""
                    text: entry.modelData.editionLine
                    color: theme.textTertiary
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
                Item { width: 1; height: 6 }
                Rectangle {
                    width: parent.width
                    height: 4
                    radius: 2
                    color: Ui.tint(theme.text, 0.12)
                    Rectangle {
                        width: parent.width * entry.modelData.progress
                        height: parent.height
                        radius: 2
                        color: theme.accent
                    }
                }
                Label {
                    text: {
                        const percent = Math.round(entry.modelData.progress * 100).toLocaleString(Qt.locale(translator.language), 'f', 0)
                        const minutes = entry.modelData.minutesLeft
                        if (minutes <= 0)
                            return qsTr("%1%").arg(percent)
                        return qsTr("%1% · %2 left").arg(percent).arg(Format.duration(minutes))
                    }
                    color: theme.textSecondary
                    font.pixelSize: 11
                }
            }

            HoverHandler {
                id: hover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler { onTapped: shelf.open(entry.modelData.id) }
        }
    }
}
