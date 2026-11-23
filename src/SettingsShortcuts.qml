import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Icons.js" as Icons
import "Shortcuts.js" as Shortcuts

// Settings › Shortcuts: every key, by where it works, with an instant filter.
ColumnLayout {
    id: page

    spacing: 16

    function normalize(text) {
        return String(text || "").toLowerCase().replace(/\+/g, " ").replace(/\s+/g, " ").trim()
    }
    readonly property string query: normalize(filter.text)
    readonly property var groups: [
        { title: qsTr("Everywhere"), glyph: Icons.keyboard, keys: Shortcuts.everywhere(translator.language) },
        { title: qsTr("Library"), glyph: Icons.books, keys: Shortcuts.library(translator.language) },
        { title: qsTr("Reading"), glyph: Icons.bookOpen, keys: Shortcuts.reading(translator.language) },
        { title: qsTr("Listening"), glyph: Icons.headphones, keys: Shortcuts.listening(translator.language) }
    ]
    function matches(entry) {
        if (query === "")
            return true
        return normalize(entry.action).indexOf(query) >= 0 || normalize(entry.keys).indexOf(query) >= 0
    }
    function clearFilterIfActive() {
        if (filter.text === "")
            return false
        filter.text = ""
        return true
    }

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        TextField {
            id: filter
            Layout.fillWidth: true
            leftPadding: 34
            rightPadding: text !== "" ? 32 : 12
            placeholderText: qsTr("Find a shortcut…")
            Accessible.name: qsTr("Find a shortcut")
            Keys.onEscapePressed: (event) => {
                if (text !== "") {
                    text = ""
                    event.accepted = true
                }
            }

            Glyph {
                anchors.left: parent.left
                anchors.leftMargin: 11
                anchors.verticalCenter: parent.verticalCenter
                width: 14
                height: 14
                path: Icons.search
                color: theme.ink4
            }

            IconButton {
                visible: filter.text !== ""
                anchors.right: parent.right
                anchors.rightMargin: 4
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: 24
                implicitHeight: 24
                glyph: Icons.close
                tip: qsTr("Clear filter")
                onClicked: {
                    filter.text = ""
                    filter.forceActiveFocus()
                }
            }
        }
    }

    Repeater {
        model: page.groups
        delegate: SettingsCard {
            id: group
            required property var modelData
            readonly property var shown: modelData.keys.filter(page.matches)
            visible: shown.length > 0
            title: modelData.title
            glyph: modelData.glyph

            Repeater {
                model: group.shown
                delegate: ColumnLayout {
                    id: shortcutEntry
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    spacing: 8

                    Rectangle {
                        visible: shortcutEntry.index > 0
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                        color: theme.hairline
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumHeight: 24
                        spacing: 12

                        Label {
                            Layout.fillWidth: true
                            text: shortcutEntry.modelData.action
                            color: theme.ink1
                            font.pixelSize: 13
                            wrapMode: Text.Wrap
                        }
                        Flow {
                            Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
                            spacing: 4
                            Repeater {
                                model: shortcutEntry.modelData.keys.split(/\s{2,}/)
                                delegate: KeyHint {
                                    required property string modelData
                                    text: modelData
                                    keycap: true
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    SettingsCard {
        visible: page.groups.every(g => g.keys.filter(page.matches).length === 0)
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Label {
                Layout.fillWidth: true
                text: qsTr("No shortcut matches “%1”.").arg(filter.text)
                textFormat: Text.PlainText
                color: theme.ink3
                font.pixelSize: 13
                wrapMode: Text.Wrap
            }
            TextButton {
                size: "compact"
                text: qsTr("Clear")
                onClicked: {
                    filter.text = ""
                    filter.forceActiveFocus()
                }
            }
        }
    }
}
