import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui

// The book's table of contents beside the page, the current chapter marked.
// Arrows move, ↵ goes, Esc closes.
SidePanel {
    id: panel

    leftEdge: true
    title: qsTr("Contents")
    glyph: Icons.contents

    signal chosen(int chapter, string fragment)

    function show() {
        open = true
        if (reader.contents.length > 0) {
            list.currentIndex = Math.max(0, reader.contentsIndex)
            list.positionViewAtIndex(list.currentIndex, ListView.Center)
            list.forceActiveFocus()
        } else {
            forceActiveFocus()
        }
    }

    Label {
        id: bookTitle
        x: 18
        y: 12
        width: parent.width - 36
        text: reader.title
        color: panel.look.muted
        font.pixelSize: 12
        elide: Text.ElideRight
    }

    Label {
        anchors.top: bookTitle.bottom
        anchors.topMargin: 24
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 18
        anchors.rightMargin: 18
        visible: reader.contents.length === 0
        text: qsTr("This book has no table of contents.")
        color: panel.look.muted
        font.pixelSize: 13
        wrapMode: Text.Wrap
    }

    ListView {
        id: list
        visible: reader.contents.length > 0
        anchors.top: bookTitle.bottom
        anchors.topMargin: 10
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        width: parent.width
        clip: true
        model: reader.contents
        boundsBehavior: Flickable.StopAtBounds
        keyNavigationEnabled: true
        highlightMoveDuration: theme.reducedMotion ? 0 : 120
        ScrollBar.vertical: ScrollBar {}

        Keys.onReturnPressed: panel.choose(currentIndex)
        Keys.onEnterPressed: panel.choose(currentIndex)
        Keys.onEscapePressed: panel.close()

        delegate: Item {
            id: row
            required property var modelData
            required property int index
            readonly property bool here: index === reader.contentsIndex
            width: list.width
            height: Math.max(32, label.implicitHeight + 14)

            Rectangle {
                anchors.fill: parent
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                radius: Ui.radius(7, theme.radius)
                color: list.currentIndex === row.index && list.activeFocus ? Ui.tint(panel.look.link, 0.16)
                     : hover.hovered ? Ui.tint(panel.look.fg, 0.06) : "transparent"
            }
            Rectangle {
                visible: row.here
                x: 10
                anchors.verticalCenter: parent.verticalCenter
                width: 3
                height: parent.height - 12
                radius: 1.5
                color: panel.look.accentInk || panel.look.link
            }
            Label {
                id: label
                x: 20 + Math.min(4, row.modelData.depth) * 14
                width: row.width - x - 18
                anchors.verticalCenter: parent.verticalCenter
                text: row.modelData.title
                color: row.modelData.chapter < 0 ? panel.look.muted : panel.look.fg
                font.pixelSize: row.modelData.depth === 0 ? 13 : 12
                font.weight: row.here ? Font.DemiBold : Font.Normal
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
            }
            HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: panel.choose(row.index) }
        }
    }

    function choose(index) {
        const entry = reader.contents[index]
        if (!entry || entry.chapter < 0)
            return
        close()
        chosen(entry.chapter, entry.fragment)
    }
}
