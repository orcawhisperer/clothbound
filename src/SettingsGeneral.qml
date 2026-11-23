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
        title: qsTr("What's new")
        description: qsTr("Dogear %1. See what changed in this version.").arg(Qt.application.version)
        RowLayout {
            TextButton { text: qsTr("Show what's new"); onClicked: welcome.show("whatsnew") }
        }
    }
}
