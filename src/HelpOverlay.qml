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
        { keys: "A", action: qsTr("Typography and theme") },
        { keys: "+  −  0", action: qsTr("Larger, smaller, default text") },
        { keys: "D", action: qsTr("Next reading theme") },
        { keys: "G", action: qsTr("Go to a page or percentage") },
        { keys: "Alt ←", action: qsTr("Back after following a link") },
        { keys: "Ctrl Z  Ctrl ⇧ Z", action: qsTr("Undo / redo") },
        { keys: "Esc", action: qsTr("Close a panel, or back to the library") }
    ]
    readonly property var everywhere: [
        { keys: "F  F11", action: qsTr("Full screen") },
        { keys: "?", action: qsTr("Show these shortcuts") },
        { keys: "Q", action: qsTr("Quit") }
    ]

    Rectangle {
        anchors.centerIn: parent
        width: columns.implicitWidth + 64
        height: columns.implicitHeight + 56
        radius: Math.min(12, theme.radius)
        color: theme.background
        border.color: Ui.tint(theme.text, 0.1)

        TapHandler {}

        Row {
            id: columns
            anchors.centerIn: parent
            spacing: 48

            Repeater {
                model: help.reading ? [help.readingSection, help.librarySection.concat(help.everywhere)]
                                    : [help.librarySection.concat(help.everywhere), help.readingSection]
                delegate: Column {
                    id: section
                    required property var modelData
                    required property int index
                    spacing: 9

                    Label {
                        text: (section.index === 0) === help.reading ? qsTr("Reading") : qsTr("Library")
                        color: theme.text
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                        bottomPadding: 6
                    }
                    Repeater {
                        model: section.modelData
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
                                text: modelData.action
                                color: theme.textSecondary
                                font.pixelSize: 13
                            }
                        }
                    }
                }
            }
        }
    }
}
