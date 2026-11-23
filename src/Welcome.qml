import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

import "Icons.js" as Icons
import "Ui.js" as Ui

// The first time Dogear opens: three short cards (your books; reading and
// listening; going further) that can be skipped, each with the one thing to
// do. After an update, one card of what's new instead.
FocusScope {
    id: welcome

    property bool open: false
    // "welcome" or "whatsnew".
    property string mode: "welcome"
    property int page: 0

    signal closed()
    signal addBooks()
    signal showIntelligence()

    readonly property var pages: [
        {
            glyph: Icons.books,
            title: qsTr("Welcome to Dogear"),
            text: qsTr("Your EPUB and PDF books in one library, and a calm page to read them on. Dogear never changes your book files: it only reads them."),
            points: [qsTr("Add the folder your books are in, and new ones there join by themselves."),
                     qsTr("Or drop books and folders anywhere on the window.")],
            action: qsTr("Add a folder of books")
        },
        {
            glyph: Icons.headphones,
            title: qsTr("Read, and listen"),
            text: qsTr("Everything works from the keyboard, and with a mouse or touchpad too."),
            points: [qsTr("Space and the arrows turn pages; select words to highlight them, add a note, or look them up."),
                     qsTr("P reads the book aloud from the page you're on, the sentence shown as it's read. A free voice made on this computer is a click away (V)."),
                     qsTr("Close the window while listening and the reading goes on; open Dogear again to come back."),
                     qsTr("? shows every key, wherever you are.")],
            action: ""
        },
        {
            glyph: Icons.sparkle,
            title: qsTr("Going further"),
            text: qsTr("Some things need a model: Gemini, with your own key and your own budget, or one on this computer, free and private."),
            points: [qsTr("The companion (C): ask about the book, define, explain, translate, Previously…, who's who, and pictures. It never reads past your page."),
                     qsTr("Natural Gemini voices, a narrator designed for the book, and a full cast."),
                     qsTr("Search by meaning, and audiobooks to keep.")],
            action: qsTr("Set up Intelligence")
        }
    ]
    readonly property var news: [
        qsTr("PDFs, in your reading theme, one or two pages at a time, with highlights, search and listening."),
        qsTr("The companion (C): ask about the book, Previously…, who's who, define, explain, translate, and pictures."),
        qsTr("Models on this computer: Intelligence (✦) sets one up for you, or finds yours."),
        qsTr("Search by meaning, a full cast, Studio and Pocket TTS voices, and audiobooks (M4B) from downloads."),
        qsTr("Opening Dogear again brings back the one already running, even while it reads in the background."),
        qsTr("Dogear speaks Omarchy's 50 languages.")
    ]

    function show(which) {
        mode = which
        page = 0
        open = true
        forceActiveFocus()
    }
    function close() {
        if (!open)
            return
        open = false
        closed()
    }
    function next() {
        if (mode === "welcome" && page < pages.length - 1)
            page++
        else
            close()
    }

    visible: open
    Keys.onEscapePressed: close()
    Keys.onReturnPressed: next()
    Keys.onEnterPressed: next()
    Keys.onSpacePressed: next()
    Keys.onRightPressed: next()
    Keys.onLeftPressed: if (page > 0) page--
    Material.roundedScale: theme.radius

    Rectangle {
        anchors.fill: parent
        color: Ui.tint("black", 0.55)
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(560, parent.width - 48)
        height: Math.min(parent.height - 48, body.implicitHeight + footer.implicitHeight + 70)
        radius: Math.min(14, theme.radius + 4)
        color: theme.surface
        border.color: Ui.tint(theme.text, 0.1)

        readonly property var current: welcome.mode === "welcome" ? welcome.pages[welcome.page] : null

        Flickable {
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: footer.top
            anchors.topMargin: 28
            anchors.leftMargin: 28
            anchors.rightMargin: 28
            anchors.bottomMargin: 14
            contentHeight: body.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

            ColumnLayout {
                id: body
                width: parent.width
                spacing: 14

                RowLayout {
                    spacing: 12
                    Glyph {
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 28
                        path: card.current ? card.current.glyph : Icons.sparkle
                        color: theme.accent
                    }
                    Label {
                        Layout.fillWidth: true
                        text: card.current ? card.current.title : qsTr("What's new in Dogear %1").arg(Qt.application.version)
                        color: theme.text
                        font.pixelSize: 22
                        font.weight: Font.DemiBold
                        wrapMode: Text.Wrap
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: !!card.current
                    text: card.current ? card.current.text : ""
                    color: theme.textSecondary
                    font.pixelSize: 15
                    wrapMode: Text.Wrap
                    lineHeight: 1.3
                }
                Repeater {
                    model: card.current ? card.current.points : welcome.news
                    delegate: RowLayout {
                        required property string modelData
                        Layout.fillWidth: true
                        spacing: 10
                        Rectangle {
                            Layout.alignment: Qt.AlignTop
                            Layout.topMargin: 8
                            width: 6; height: 6; radius: 3
                            color: theme.accent
                        }
                        Label {
                            Layout.fillWidth: true
                            text: modelData
                            color: theme.text
                            font.pixelSize: 14
                            wrapMode: Text.Wrap
                            lineHeight: 1.3
                        }
                    }
                }
            }
        }

        RowLayout {
            id: footer
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 28
            anchors.rightMargin: 28
            anchors.bottomMargin: 24
            spacing: 8
            // Where you are in the three.
            Row {
                visible: welcome.mode === "welcome"
                spacing: 6
                Repeater {
                    model: welcome.pages.length
                    delegate: Rectangle {
                        required property int index
                        width: index === welcome.page ? 18 : 6
                        height: 6
                        radius: 3
                        color: index === welcome.page ? theme.accent : Ui.tint(theme.text, 0.25)
                        Behavior on width { NumberAnimation { duration: 150 } }
                    }
                }
            }
            Item { Layout.fillWidth: true }
            TextButton {
                visible: welcome.mode === "welcome" && welcome.page < welcome.pages.length - 1
                text: qsTr("Skip")
                onClicked: welcome.close()
            }
            TextButton {
                visible: !!card.current && card.current.action !== ""
                text: card.current ? card.current.action : ""
                onClicked: {
                    if (welcome.page === 0) {
                        welcome.addBooks()
                        welcome.next()
                    } else {
                        welcome.close()
                        welcome.showIntelligence()
                    }
                }
            }
            TextButton {
                primary: true
                text: welcome.mode === "welcome" && welcome.page < welcome.pages.length - 1 ? qsTr("Next")
                    : qsTr("Start reading")
                onClicked: welcome.next()
            }
        }
    }
}
