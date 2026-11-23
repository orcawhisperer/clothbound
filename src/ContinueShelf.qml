import QtQuick
import QtQuick.Controls

import "Format.js" as Format
import "Ui.js" as Ui

// Where you left off: the book you read last as one large card, with its
// chapter, progress and time left, and the next few beside it. A click or
// Enter opens a book where it was left; arrows move between them.
FocusScope {
    id: shelf

    property var books: []
    property int currentIndex: 0

    signal open(int bookId)
    signal leaveDown()

    readonly property var lead: books.length > 0 ? books[0] : null
    readonly property var others: books.slice(1, 4)
    readonly property bool wide: width >= 760

    function focusShelf() {
        currentIndex = 0
        forceActiveFocus()
    }
    function openCurrent() {
        if (currentIndex >= 0 && currentIndex < Math.min(books.length, 4))
            open(books[currentIndex].id)
    }
    function percentOf(book) {
        return Math.round(book.progress * 100).toLocaleString(Qt.locale(), 'f', 0)
    }
    function timeLeft(book) {
        const _lang = translator.language
        return book.minutesLeft > 0 ? qsTr("%1 left").arg(Format.duration(book.minutesLeft)) : ""
    }

    implicitHeight: 212
    activeFocusOnTab: true
    Keys.onReturnPressed: openCurrent()
    Keys.onEnterPressed: openCurrent()
    Keys.onRightPressed: if (wide && currentIndex === 0 && others.length > 0) currentIndex = 1
    Keys.onLeftPressed: if (currentIndex > 0) currentIndex = 0
    Keys.onUpPressed: if (currentIndex > 1) currentIndex--
    Keys.onDownPressed: {
        if (currentIndex >= 1 && currentIndex < others.length)
            currentIndex++
        else
            shelf.leaveDown()
    }

    // --- The book read last ---------------------------------------------
    AbstractButton {
        id: hero
        visible: shelf.lead !== null
        width: shelf.wide && shelf.others.length > 0 ? Math.round(shelf.width * 0.6) : shelf.width
        height: shelf.height
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        readonly property bool focused: shelf.activeFocus && shelf.currentIndex === 0
        Accessible.name: shelf.lead ? qsTr("Continue %1").arg(shelf.lead.title) : ""
        onClicked: { shelf.currentIndex = 0; if (shelf.lead) shelf.open(shelf.lead.id) }

        background: Rectangle {
            radius: Math.min(14, theme.radius)
            color: hero.pressed ? theme.fillPressed : hero.hovered ? theme.fillHover : theme.fill
            border.width: hero.focused ? 2 : 1
            border.color: hero.focused ? theme.accent : theme.hairline
            Behavior on color { ColorAnimation { duration: 100 } }
        }
        contentItem: Item {
            Rectangle {
                x: cover.x + cover.box.x
                y: cover.y + cover.box.y + 6
                width: cover.box.width
                height: cover.box.height
                radius: cover.radius
                color: "black"
                opacity: theme.light ? 0.16 : 0.4
            }
            CoverImage {
                id: cover
                x: 20
                y: 18
                width: 118
                height: parent.height - 36
                source: shelf.lead ? shelf.lead.cover : ""
                title: shelf.lead ? shelf.lead.title : ""
                author: shelf.lead ? shelf.lead.author : ""
            }
            Column {
                anchors.left: cover.right
                anchors.leftMargin: 24
                anchors.right: parent.right
                anchors.rightMargin: 24
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                Label {
                    text: qsTr("Continue reading")
                    color: theme.accentInk
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 0.5
                }
                Label {
                    width: parent.width
                    text: shelf.lead ? shelf.lead.title : ""
                    color: theme.ink1
                    font.family: "Literata"
                    font.pixelSize: 22
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                    lineHeight: 1.05
                }
                Label {
                    width: parent.width
                    text: shelf.lead ? shelf.lead.author : ""
                    color: theme.ink2
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
                Item { width: 1; height: 10 }
                Label {
                    width: parent.width
                    text: !shelf.lead ? ""
                          : shelf.lead.chapterTitle !== "" ? qsTr("%1 · %2%").arg(shelf.lead.chapterTitle).arg(shelf.percentOf(shelf.lead))
                          : qsTr("%1% read").arg(shelf.percentOf(shelf.lead))
                    color: theme.ink2
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                Rectangle {
                    width: Math.min(parent.width, 280)
                    height: 4
                    radius: 2
                    color: theme.separator
                    Rectangle {
                        width: parent.width * (shelf.lead ? Math.max(0.02, shelf.lead.progress) : 0)
                        height: parent.height
                        radius: 2
                        color: theme.accent
                    }
                }
                Item { width: 1; height: 8 }
                Row {
                    spacing: 12
                    TextButton {
                        text: qsTr("Resume")
                        primary: true
                        hint: "↵"
                        onClicked: if (shelf.lead) shelf.open(shelf.lead.id)
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: shelf.lead ? shelf.timeLeft(shelf.lead) : ""
                        color: theme.ink3
                        font.pixelSize: 12
                    }
                }
            }
        }
    }

    // --- The next books under way ---------------------------------------
    Column {
        visible: shelf.wide && shelf.others.length > 0
        anchors.left: hero.right
        anchors.leftMargin: 16
        anchors.right: parent.right
        height: shelf.height
        spacing: 8

        Repeater {
            model: shelf.others
            delegate: AbstractButton {
                id: other
                required property var modelData
                required property int index
                readonly property bool focused: shelf.activeFocus && shelf.currentIndex === index + 1
                width: parent.width
                height: (shelf.height - 16) / 3
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                Accessible.name: modelData.title
                onClicked: { shelf.currentIndex = index + 1; shelf.open(modelData.id) }

                background: Rectangle {
                    radius: Math.min(10, theme.radius)
                    color: other.pressed ? theme.fillPressed : other.hovered ? theme.fill : "transparent"
                    border.width: other.focused ? 2 : 0
                    border.color: theme.accent
                }
                contentItem: Item {
                    CoverImage {
                        id: thumb
                        x: 8
                        y: 6
                        width: 40
                        height: parent.height - 12
                        source: other.modelData.cover
                        title: other.modelData.title
                        author: other.modelData.author
                    }
                    Column {
                        anchors.left: thumb.right
                        anchors.leftMargin: 12
                        anchors.right: parent.right
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3
                        Label {
                            width: parent.width
                            text: other.modelData.title
                            color: theme.ink1
                            font.pixelSize: 13
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                        }
                        Label {
                            width: parent.width
                            text: [qsTr("%1%").arg(shelf.percentOf(other.modelData)), shelf.timeLeft(other.modelData)]
                                  .filter(s => s !== "").join(" · ")
                            color: theme.ink3
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                        Rectangle {
                            width: Math.min(parent.width, 160)
                            height: 3
                            radius: 1.5
                            color: theme.separator
                            Rectangle {
                                width: parent.width * Math.max(0.02, other.modelData.progress)
                                height: parent.height
                                radius: 1.5
                                color: theme.accent
                            }
                        }
                    }
                }
            }
        }
    }
}
