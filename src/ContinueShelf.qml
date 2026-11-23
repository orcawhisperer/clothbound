import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Format.js" as Format
import "Icons.js" as Icons
import "Ui.js" as Ui

// Where you left off: the book you read last as one large hero card with its
// quote, progress, time left, and Read / Listen buttons, plus two generous
// cards beside it (01_library.html). Arrows move between them; Enter opens.
FocusScope {
    id: shelf

    property var books: []
    property int currentIndex: 0

    signal open(int bookId)
    signal listen(int bookId)
    signal leaveDown()

    readonly property var lead: books.length > 0 ? books[0] : null
    readonly property var others: books.slice(1, 3)
    readonly property bool wide: width >= 760

    function focusShelf() {
        currentIndex = 0
        forceActiveFocus()
    }
    function openCurrent() {
        if (currentIndex >= 0 && currentIndex < Math.min(books.length, 3))
            open(books[currentIndex].id)
    }
    function percentOf(book) {
        return Math.round((book.progress || 0) * 100).toLocaleString(Qt.locale(), "f", 0)
    }
    function timeLeft(book) {
        const _lang = translator.language
        return (book.minutesLeft || 0) > 0 ? qsTr("%1 left").arg(Format.duration(book.minutesLeft)) : ""
    }
    function progressLine(book) {
        if (!book)
            return ""
        const parts = [qsTr("%1%").arg(percentOf(book))]
        const left = timeLeft(book)
        if (left !== "")
            parts.push(left)
        return parts.join(" · ")
    }

    implicitHeight: 226
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

    // --- The book read last (1.45 flex share) ---------------------------
    Rectangle {
        id: hero
        anchors.left: parent.left
        visible: shelf.lead !== null
        width: shelf.wide && shelf.others.length > 0
               ? Math.round((shelf.width - 20) * 1.45 / 2.45) : shelf.width
        height: shelf.height
        radius: Ui.radius(14, theme.radius)
        readonly property bool focused: shelf.activeFocus && shelf.currentIndex === 0
        readonly property bool hovered: heroHover.hovered

        gradient: Gradient {
            GradientStop { position: 0.0; color: Ui.tint(theme.ink1, hero.hovered ? 0.07 : 0.05) }
            GradientStop { position: 1.0; color: Ui.tint(theme.ink1, hero.hovered ? 0.04 : 0.02) }
        }
        border.width: hero.focused ? 2 : 1
        border.color: hero.focused ? theme.accent : Ui.tint(theme.ink1, 0.07)

        HoverHandler { id: heroHover; cursorShape: Qt.PointingHandCursor }
        TapHandler {
            onTapped: {
                shelf.currentIndex = 0
                if (shelf.lead)
                    shelf.open(shelf.lead.id)
            }
        }

        // Cover drop shadow.
        Rectangle {
            x: cover.x + cover.box.x
            y: cover.y + cover.box.y + 8
            width: cover.box.width
            height: cover.box.height
            radius: cover.radius
            color: "black"
            opacity: theme.light ? 0.16 : 0.45
        }

        CoverImage {
            id: cover
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.verticalCenter: parent.verticalCenter
            width: 124
            height: 186
            source: shelf.lead ? (shelf.lead.cover || "") : ""
            title: shelf.lead ? (shelf.lead.title || "") : ""
            author: shelf.lead ? (shelf.lead.author || "") : ""
        }

        ColumnLayout {
            anchors.left: cover.right
            anchors.leftMargin: 24
            anchors.right: parent.right
            anchors.rightMargin: 20
            anchors.top: parent.top
            anchors.topMargin: 20
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 20
            spacing: 4

            Label {
                text: qsTr("Continue reading")
                color: theme.accentInk
                font.pixelSize: 11
                font.weight: Font.DemiBold
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 0.44
            }

            Label {
                Layout.fillWidth: true
                text: shelf.lead ? (shelf.lead.title || "") : ""
                textFormat: Text.PlainText
                color: theme.ink1
                font.family: "Literata"
                font.pixelSize: 24
                font.weight: Font.Medium
                font.letterSpacing: -0.24
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
                lineHeight: 1.08
            }

            Label {
                Layout.fillWidth: true
                text: {
                    if (!shelf.lead)
                        return ""
                    const parts = []
                    if (shelf.lead.author)
                        parts.push(shelf.lead.author)
                    if (shelf.lead.chapterTitle)
                        parts.push(shelf.lead.chapterTitle)
                    return parts.join(" · ")
                }
                textFormat: Text.PlainText
                visible: text !== ""
                color: theme.ink3
                font.pixelSize: 13
                elide: Text.ElideRight
            }

            Label {
                Layout.fillWidth: true
                Layout.topMargin: 6
                visible: shelf.lead !== null && (shelf.lead.snippet || "") !== ""
                text: shelf.lead && shelf.lead.snippet ? qsTr("“%1”").arg(shelf.lead.snippet) : ""
                textFormat: Text.PlainText
                color: theme.quote
                font.family: "Literata"
                font.italic: true
                font.pixelSize: 14
                lineHeight: 1.45
                wrapMode: Text.Wrap
                maximumLineCount: 2
                elide: Text.ElideRight
            }

            Item { Layout.fillHeight: true }

            // Progress bar paired inline with percentage and time left.
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 4
                    radius: 2
                    color: Ui.tint(theme.ink1, 0.10)
                    Rectangle {
                        anchors.left: parent.left
                        width: parent.width * (shelf.lead ? Math.max(0.02, shelf.lead.progress || 0) : 0)
                        height: parent.height
                        radius: 2
                        color: theme.accent
                    }
                }

                Label {
                    text: shelf.progressLine(shelf.lead)
                    color: theme.ink3
                    font.pixelSize: 12
                    font.features: ({ "tnum": 1 })
                }
            }

            Row {
                Layout.topMargin: 6
                spacing: 8

                TextButton {
                    text: qsTr("Read")
                    primary: true
                    size: "normal"
                    onClicked: if (shelf.lead) shelf.open(shelf.lead.id)
                }

                TextButton {
                    text: qsTr("Listen")
                    glyph: Icons.headphones
                    size: "normal"
                    onClicked: if (shelf.lead) shelf.listen(shelf.lead.id)
                }
            }
        }
    }

    // --- Two secondary cards beside the hero (1.0 flex share) -----------
    Column {
        visible: shelf.wide && shelf.others.length > 0
        anchors.left: hero.right
        anchors.leftMargin: 20
        anchors.right: parent.right
        height: shelf.height
        spacing: 12

        Repeater {
            model: shelf.others
            delegate: Rectangle {
                id: other
                required property var modelData
                required property int index
                readonly property bool focused: shelf.activeFocus && shelf.currentIndex === index + 1
                readonly property bool isListeningCard: (narration.active && reader.bookId === modelData.id)
                                                        || (modelData.listening && (modelData.voiceName || "") !== "")
                readonly property string voiceLabel: (narration.active && reader.bookId === modelData.id && narration.voiceName !== "")
                                                     ? narration.voiceName : (modelData.voiceName || "")

                width: parent.width
                height: (shelf.height - 12) / 2
                radius: Ui.radius(14, theme.radius)
                color: otherHover.hovered ? Ui.tint(theme.ink1, 0.06) : Ui.tint(theme.ink1, 0.03)
                border.width: other.focused ? 2 : 1
                border.color: other.focused ? theme.accent : Ui.tint(theme.ink1, 0.06)
                Behavior on color { ColorAnimation { duration: 100 } }

                HoverHandler { id: otherHover; cursorShape: Qt.PointingHandCursor }
                TapHandler {
                    onTapped: {
                        shelf.currentIndex = other.index + 1
                        shelf.open(other.modelData.id)
                    }
                }

                // Cover shadow.
                Rectangle {
                    x: thumb.x + thumb.box.x
                    y: thumb.y + thumb.box.y + 5
                    width: thumb.box.width
                    height: thumb.box.height
                    radius: thumb.radius
                    color: "black"
                    opacity: theme.light ? 0.15 : 0.4
                }

                CoverImage {
                    id: thumb
                    anchors.left: parent.left
                    anchors.leftMargin: 16
                    anchors.verticalCenter: parent.verticalCenter
                    width: 56
                    height: 84
                    source: other.modelData.cover || ""
                    title: other.modelData.title || ""
                    author: other.modelData.author || ""
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
                        text: other.modelData.title || ""
                        textFormat: Text.PlainText
                        color: theme.ink1
                        font.pixelSize: 14
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }

                    Row {
                        width: parent.width
                        spacing: 6

                        Glyph {
                            visible: other.isListeningCard
                            anchors.verticalCenter: parent.verticalCenter
                            width: 12
                            height: 12
                            stroke: 2
                            path: Icons.waveform
                            color: theme.accentInk
                        }

                        Label {
                            width: parent.width - (other.isListeningCard ? 18 : 0)
                            anchors.verticalCenter: parent.verticalCenter
                            text: {
                                const _lang = translator.language
                                if (other.isListeningCard) {
                                    return other.voiceLabel !== ""
                                        ? qsTr("Listening · %1").arg(other.voiceLabel)
                                        : qsTr("Listening")
                                }
                                const parts = []
                                if (other.modelData.chapterTitle)
                                    parts.push(other.modelData.chapterTitle)
                                if ((other.modelData.highlightCount || 0) > 0)
                                    parts.push(qsTr("%n highlights", "", other.modelData.highlightCount))
                                if (parts.length === 0 && other.modelData.author)
                                    parts.push(other.modelData.author)
                                return parts.join(" · ")
                            }
                            textFormat: Text.PlainText
                            color: theme.ink3
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }

                    Item { width: 1; height: 4 }

                    Rectangle {
                        width: parent.width
                        height: 3
                        radius: 1.5
                        color: Ui.tint(theme.ink1, 0.10)
                        Rectangle {
                            anchors.left: parent.left
                            width: parent.width * Math.max(0.02, other.modelData.progress || 0)
                            height: parent.height
                            radius: 1.5
                            color: theme.accent
                        }
                    }

                    Label {
                        width: parent.width
                        text: shelf.progressLine(other.modelData)
                        color: theme.ink4
                        font.pixelSize: 11
                        font.features: ({ "tnum": 1 })
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }
}
