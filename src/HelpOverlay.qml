import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// Every shortcut, shown with ?. The section for where you are comes first.
Rectangle {
    id: help

    property bool reading: false

    signal dismissed()

    color: Ui.tint("black", 0.72)

    TapHandler { onTapped: help.dismissed() }
    // The page underneath doesn't turn while the shortcuts are shown.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.NoButton
        onWheel: (wheel) => wheel.accepted = true
    }

    readonly property var librarySection: [
        { keys: "← → ↑ ↓  h j k l", action: qsTr("Move between books") },
        { keys: "↵", action: qsTr("Open the book, or choose an edition") },
        { keys: "Space", action: qsTr("Continue the book you were reading") },
        { keys: "/", action: qsTr("Search the library") },
        { keys: "I", action: qsTr("Details about the book") },
        { keys: "S", action: qsTr("Change the order") },
        { keys: "G", action: qsTr("Group or separate editions") },
        { keys: "A", action: qsTr("Add a folder of books") },
        { keys: "Ctrl Z", action: qsTr("Undo") }
    ]
    readonly property var readingSection: [
        { keys: "Space  →  l  j", action: qsTr("Next page") },
        { keys: "⇧ Space  ←  h  k", action: qsTr("Previous page") },
        { keys: "[  ]", action: qsTr("Previous / next chapter") },
        { keys: "T", action: qsTr("Contents") },
        { keys: "/  Ctrl F", action: qsTr("Search in the book") },
        { keys: "n  N", action: qsTr("Next / previous result") },
        { keys: "B", action: qsTr("Bookmark this page") },
        { keys: "M", action: qsTr("Highlights, notes, and bookmarks") },
        { keys: "C", action: qsTr("Companion: ask, previously, who's who") },
        { keys: "A", action: qsTr("Typography and theme") },
        { keys: "+  −  0", action: qsTr("Larger, smaller, default text") },
        { keys: "D", action: qsTr("Next reading theme") },
        { keys: "G", action: qsTr("Go to a page or percentage") },
        { keys: "Alt ←", action: qsTr("Back after following a link") },
        { keys: "Ctrl Z  Ctrl ⇧ Z", action: qsTr("Undo / redo") },
        { keys: "Esc", action: qsTr("Close a panel, or back to the library") }
    ]
    readonly property var listeningSection: [
        { keys: "P", action: qsTr("Listen from this page, pause, go on") },
        { keys: "⇧ P", action: qsTr("Stop listening") },
        { keys: ",  .", action: qsTr("Previous / next paragraph") },
        { keys: "<  >", action: qsTr("Slower / faster") },
        { keys: "V", action: qsTr("Narrator and style") },
        { keys: "Super W", action: qsTr("Close the window and keep listening; open Dogear again to return") }
    ]
    readonly property var everywhere: [
        { keys: "F  F11", action: qsTr("Full screen") },
        { keys: "?", action: qsTr("Show these shortcuts") },
        { keys: "Q", action: qsTr("Quit") }
    ]

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, columns.implicitWidth + 64)
        height: Math.min(parent.height - 48, columns.implicitHeight + 56)
        radius: Math.min(12, theme.radius)
        color: theme.background
        border.color: Ui.tint(theme.text, 0.1)

        TapHandler {}

        Flickable {
            anchors.fill: parent
            anchors.leftMargin: 32
            anchors.rightMargin: 32
            anchors.topMargin: 28
            anchors.bottomMargin: 28
            contentWidth: columns.implicitWidth
            contentHeight: columns.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

            Grid {
                id: columns
                columns: help.width >= 960 ? 2 : 1
                columnSpacing: 48
                rowSpacing: 24

                // Each column is a few sections; the ones for where you are come first.
                Repeater {
                    model: {
                        const reading = { title: qsTr("Reading"), items: help.readingSection }
                        const listening = { title: qsTr("Listening"), items: help.listeningSection }
                        const library = { title: qsTr("Library"), items: help.librarySection.concat(help.everywhere) }
                        return help.reading ? [[reading], [listening, library]] : [[library], [reading, listening]]
                    }
                    delegate: Column {
                        id: column
                        required property var modelData
                        spacing: 22

                        Repeater {
                            model: column.modelData
                            delegate: Column {
                                id: section
                                required property var modelData
                                spacing: 9

                                Label {
                                    text: section.modelData.title
                                    color: theme.text
                                    font.pixelSize: 16
                                    font.weight: Font.DemiBold
                                    bottomPadding: 6
                                }
                                Repeater {
                                    model: section.modelData.items
                                    delegate: Row {
                                        required property var modelData
                                        spacing: 18
                                        Label {
                                            width: 150
                                            horizontalAlignment: Text.AlignRight
                                            text: modelData.keys
                                            color: theme.accent
                                            font.pixelSize: 13
                                            font.family: "monospace"
                                        }
                                        Label {
                                            width: Math.min(implicitWidth, Math.max(180, help.width - 280))
                                            text: modelData.action
                                            color: theme.textSecondary
                                            font.pixelSize: 13
                                            wrapMode: Text.Wrap
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
