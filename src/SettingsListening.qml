import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Format.js" as Format

// Settings › Listening: the voices made on this computer and the audio kept
// for listening again. Each book's narrator is chosen while reading (V).
ColumnLayout {
    id: page

    spacing: 16

    SettingsCard {
        title: qsTr("Narrators")
        description: qsTr("Each book keeps its own narrator and style, chosen in the Narrator panel while reading (V).")
        RowLayout {
            spacing: 8
            Badge { where: "local"; text: qsTr("Built-in voices: free, private, offline") }
            Badge { where: "cloud"; text: qsTr("Gemini voices: expressive, with your key") }
        }
    }

    SettingsCard {
        visible: voicePacks.supported
        title: qsTr("Built-in voices")
        description: qsTr("Made on this computer, without a key. They're plainer than Gemini's and read without styles.")
        Repeater {
            model: voicePacks.megabytes >= 0 ? voicePacks.installedPacks() : []
            delegate: RowLayout {
                required property var modelData
                Layout.fillWidth: true
                spacing: 10
                Label {
                    Layout.fillWidth: true
                    text: modelData.title
                    color: theme.ink1
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
                Label {
                    text: qsTr("%1 MB").arg(Math.round(modelData.megabytes).toLocaleString(Qt.locale(), 'f', 0))
                    color: theme.ink3
                    font.pixelSize: 12
                }
                TextButton { text: qsTr("Remove"); onClicked: voicePacks.remove(modelData.id) }
            }
        }
        Label {
            visible: voicePacks.megabytes < 1
            Layout.fillWidth: true
            text: qsTr("None yet. Choose one in the Narrator panel while reading (V).")
            color: theme.ink3
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }
    }

    SettingsCard {
        title: qsTr("Audio kept for listening again")
        description: qsTr("Narration is kept so a chapter heard once plays again at once, and costs nothing the second time.")
        SettingsRow {
            title: audioCache.megabytes >= 1
                   ? qsTr("%1 MB of audio").arg(Math.round(audioCache.megabytes).toLocaleString(Qt.locale(), 'f', 0))
                   : qsTr("No audio kept")
            TextButton {
                text: qsTr("Clear")
                enabled: audioCache.megabytes >= 1
                onClicked: audioCache.clear()
            }
        }
    }
}
