import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Shortcuts.js" as Shortcuts

// Settings › Shortcuts: every key, by where it works, with a filter.
ColumnLayout {
    id: page

    spacing: 16

    readonly property string query: filter.text.trim().toLowerCase()
    readonly property var groups: [
        { title: qsTr("Everywhere"), keys: Shortcuts.everywhere(translator.language) },
        { title: qsTr("Library"), keys: Shortcuts.library(translator.language) },
        { title: qsTr("Reading"), keys: Shortcuts.reading(translator.language) },
        { title: qsTr("Listening"), keys: Shortcuts.listening(translator.language) }
    ]
    function matches(entry) {
        return query === "" || entry.action.toLowerCase().indexOf(query) >= 0 || entry.keys.toLowerCase().indexOf(query) >= 0
    }

    TextField {
        id: filter
        Layout.fillWidth: true
        placeholderText: qsTr("Find a shortcut")
    }

    Repeater {
        model: page.groups
        delegate: SettingsCard {
            id: group
            required property var modelData
            readonly property var shown: modelData.keys.filter(page.matches)
            visible: shown.length > 0
            title: modelData.title
            Repeater {
                model: group.shown
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 12
                    Label {
                        Layout.fillWidth: true
                        text: modelData.action
                        color: theme.ink1
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }
                    Row {
                        spacing: 4
                        Repeater {
                            model: modelData.keys.split(/\s{2,}/)
                            delegate: KeyHint {
                                required property string modelData
                                text: modelData
                            }
                        }
                    }
                }
            }
        }
    }
    Label {
        visible: page.groups.every(g => g.keys.filter(page.matches).length === 0)
        text: qsTr("No shortcut matches “%1”.").arg(filter.text)
        color: theme.ink3
        font.pixelSize: 13
    }
}
