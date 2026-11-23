import QtQuick
import QtQuick.Controls

import "Format.js" as Format
import "Ui.js" as Ui

// The books under way, newest first, each a click or Enter from where it was left.
Column {
    id: shelf

    property var books: []

    signal open(int bookId)
    signal leaveDown()

    function focusShelf() {
        if (list.count > 0 && list.currentIndex < 0)
            list.currentIndex = 0
        list.forceActiveFocus()
    }

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
        activeFocusOnTab: true
        keyNavigationEnabled: true
        keyNavigationWraps: false
        boundsBehavior: Flickable.StopAtBounds
        currentIndex: count > 0 ? 0 : -1
        model: shelf.books

        Keys.onReturnPressed: {
            if (currentIndex >= 0 && currentIndex < shelf.books.length)
                shelf.open(shelf.books[currentIndex].id)
        }
        Keys.onEnterPressed: {
            if (currentIndex >= 0 && currentIndex < shelf.books.length)
                shelf.open(shelf.books[currentIndex].id)
        }
        Keys.onDownPressed: shelf.leaveDown()

        // Horizontal wheel scrolling: handles touchpad horizontal swipe and
        // vertical mouse wheel when cards overflow horizontally, passing through
        // at the edges so vertical scrolling of the library grid still works.
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: (event) => {
                const maxX = Math.max(0, list.contentWidth - list.width)
                if (maxX <= 0) {
                    event.accepted = false
                    return
                }
                const delta = event.angleDelta.x !== 0 ? event.angleDelta.x : event.angleDelta.y
                if (delta === 0) {
                    event.accepted = false
                    return
                }
                const step = -delta * 0.75
                const nextX = Math.max(0, Math.min(maxX, list.contentX + step))
                if (Math.abs(nextX - list.contentX) > 0.5 || event.angleDelta.x !== 0) {
                    list.contentX = nextX
                    event.accepted = true
                } else {
                    event.accepted = false
                }
            }
        }

        delegate: Item {
            id: entry
            required property var modelData
            required property int index
            readonly property bool focused: list.activeFocus && ListView.isCurrentItem
            width: 330
            height: list.height

            Rectangle {
                anchors.fill: parent
                radius: Math.min(10, theme.radius)
                color: entry.focused ? Ui.tint(theme.accent, 0.14)
                     : hover.hovered ? Ui.tint(theme.text, 0.07) : Ui.tint(theme.text, 0.035)
                border.width: entry.focused ? 1.5 : 0
                border.color: theme.accent
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
                        const _lang = translator.language
                        const percent = Math.round(entry.modelData.progress * 100).toLocaleString(Qt.locale(), 'f', 0)
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
            TapHandler {
                onTapped: {
                    list.currentIndex = entry.index
                    shelf.open(entry.modelData.id)
                }
            }
        }
    }
}
