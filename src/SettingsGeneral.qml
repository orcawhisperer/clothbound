import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Icons.js" as Icons

// Settings › General: how Dogear looks, its language, and the folders it
// watches for books.
ColumnLayout {
    id: page

    spacing: 16

    Settings {
        id: appearance
        location: settingsUrl
        category: "appearance"
        property string mode: "match"
        property bool desktopFont: false
    }
    property bool fontChanged: false

    SettingsCard {
        title: qsTr("Appearance")
        SettingsRow {
            title: qsTr("Light or dark")
            description: theme.omarchy || appearance.mode !== "match"
                         ? qsTr("Match desktop follows your Omarchy theme as it changes.")
                         : qsTr("Match desktop follows your Omarchy theme as it changes; without one, Dogear is dark.")
            Segmented {
                model: [{ value: "match", label: qsTr("Match desktop") },
                        { value: "dark", label: qsTr("Dark") },
                        { value: "light", label: qsTr("Light") }]
                current: theme.appearance
                onActivated: (value) => { theme.appearance = value; appearance.mode = value }
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Use the desktop's font")
            description: page.fontChanged ? qsTr("Takes effect the next time Dogear starts.")
                                          : qsTr("Dogear's interface is set in Inter. Books keep their own typeface.")
            Switch {
                checked: appearance.desktopFont
                onToggled: { appearance.desktopFont = checked; page.fontChanged = true }
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Language")
            description: qsTr("Dogear speaks the language chosen for Omarchy in Setup › Language, and follows it when it changes.")
            Label {
                text: Qt.locale(translator.language).nativeLanguageName || translator.language
                color: theme.ink2
                font.pixelSize: 13
            }
        }
    }

    SettingsCard {
        title: qsTr("Library folders")
        description: qsTr("Books in these folders are read where they are, never changed, and new ones are added each time Dogear starts.")
        Repeater {
            model: library.folders
            delegate: RowLayout {
                required property string modelData
                Layout.fillWidth: true
                spacing: 10
                Glyph { Layout.preferredWidth: 16; Layout.preferredHeight: 16; path: Icons.folder; color: theme.ink3 }
                Label {
                    Layout.fillWidth: true
                    text: modelData.replace(/^\/home\/[^/]+/, "~")
                    color: theme.ink1
                    font.pixelSize: 13
                    elide: Text.ElideMiddle
                }
                TextButton {
                    text: qsTr("Remove")
                    onClicked: library.removeFolder(modelData)
                }
            }
        }
        Label {
            visible: library.folders.length === 0
            text: qsTr("No folders yet.")
            color: theme.ink3
            font.pixelSize: 13
        }
        RowLayout {
            spacing: 8
            TextButton { text: qsTr("Add a folder…"); glyph: Icons.plus; onClicked: picker.chooseFolders() }
            TextButton {
                visible: library.folders.length > 0
                text: library.scanning ? qsTr("Looking…") : qsTr("Look for new books")
                enabled: !library.scanning
                onClicked: library.rescan()
            }
        }
    }

    SettingsCard {
        title: qsTr("Search inside books")
        description: libraryIndex.available
                     ? qsTr("Every book's words, kept on this computer so the palette (Ctrl+K) finds a passage in any book.")
                     : qsTr("This computer's SQLite has no full-text search (FTS5), so the palette searches titles, authors and notes only.")
        SettingsRow {
            visible: libraryIndex.available
            title: libraryIndex.indexing
                   ? qsTr("Reading books in · %1 of %2").arg(libraryIndex.done.toLocaleString(Qt.locale(), 'f', 0))
                                                         .arg(libraryIndex.total.toLocaleString(Qt.locale(), 'f', 0))
                   : qsTr("%n books searchable", "", libraryIndex.bookCount)
            description: qsTr("%1 MB, in the cache; it can always be built again.").arg(Math.max(0.1, libraryIndex.megabytes).toLocaleString(Qt.locale(), 'f', 1))
            TextButton {
                text: qsTr("Rebuild")
                enabled: !libraryIndex.indexing
                onClicked: libraryIndex.rebuild()
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("By meaning")
            description: !bookSearch.canSearchByMeaning
                         ? qsTr("Needs a search model, from Gemini or on this computer (Settings › Intelligence).")
                         : qsTr("A book is prepared the first time you search inside it by meaning (/ while reading); prepared books are searched together from the palette.")
            Label {
                text: qsTr("%n books prepared", "", bookSearch.booksPrepared)
                color: theme.ink2
                font.pixelSize: 13
            }
        }
    }

    SettingsCard {
        title: qsTr("What's new")
        description: qsTr("Dogear %1. See what changed in this version.").arg(Qt.application.version)
        RowLayout {
            TextButton { text: qsTr("Show what's new"); onClicked: welcome.show("whatsnew") }
        }
    }
}
