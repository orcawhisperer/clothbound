import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// The book's table of contents beside the page, the current chapter marked.
// Arrows move, ↵ goes, Esc closes.
FocusScope {
    id: panel

    property bool open: false
    property var look

    signal chosen(int chapter, string fragment)
    signal closed()

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
    function close() {
        if (!open)
            return
        open = false
        closed()
    }

    Keys.onEscapePressed: close()

    width: 380
    x: open ? 0 : -width - 24
    Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    visible: x > -width - 20

    Rectangle {
        anchors.fill: parent
        color: panel.look.popover
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: panel.look.line }
    }
    // The panel keeps its clicks and the wheel: nothing falls through to the page.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }
    // Shadow onto the page.
    Rectangle {
        anchors.left: parent.right
        width: 18
        height: parent.height
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: Ui.tint("black", panel.look.dark ? 0.35 : 0.12) }
            GradientStop { position: 1; color: "transparent" }
        }
    }

    Column {
        id: heading
        x: 24
        y: 22
        width: parent.width - 48
        spacing: 4
        Label {
            text: qsTr("Contents")
            color: panel.look.fg
            font.pixelSize: 18
            font.weight: Font.DemiBold
        }
        Label {
            width: parent.width
            text: reader.title
            color: panel.look.muted
            font.pixelSize: 13
            elide: Text.ElideRight
        }
    }

    Label {
        anchors.top: heading.bottom
        anchors.topMargin: 28
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        visible: reader.contents.length === 0
        text: qsTr("This book has no table of contents.")
        color: panel.look.muted
        font.pixelSize: 13
        wrapMode: Text.Wrap
    }

    ListView {
        id: list
        visible: reader.contents.length > 0
        anchors.top: heading.bottom
        anchors.topMargin: 16
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        width: parent.width
        clip: true
        model: reader.contents
        boundsBehavior: Flickable.StopAtBounds
        keyNavigationEnabled: true
        highlightMoveDuration: 120
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
            height: Math.max(36, label.implicitHeight + 16)

            Rectangle {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                radius: Math.min(6, theme.radius)
                color: list.currentIndex === row.index && list.activeFocus ? Ui.tint(panel.look.link, 0.18)
                     : hover.hovered ? Ui.tint(panel.look.fg, 0.06) : "transparent"
            }
            Rectangle {
                visible: row.here
                x: 12
                anchors.verticalCenter: parent.verticalCenter
                width: 3
                height: parent.height - 14
                radius: 1.5
                color: panel.look.link
            }
            Label {
                id: label
                x: 24 + Math.min(4, row.modelData.depth) * 16
                width: row.width - x - 24
                anchors.verticalCenter: parent.verticalCenter
                text: row.modelData.title
                color: row.modelData.chapter < 0 ? panel.look.muted : panel.look.fg
                font.pixelSize: row.modelData.depth === 0 ? 14 : 13
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
