import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Format.js" as Format
import "Icons.js" as Icons

// Settings › Voices (page id "listening"): the voices installed on this
// computer and the audio kept on disk for instant replay. Each book keeps its
// own narrator and style, chosen in the Narrator sheet while reading (V).
ColumnLayout {
    id: page

    spacing: 16

    property int packsVersion: 0
    readonly property var installedPacks: packsVersion >= 0 && voicePacks.megabytes >= 0
                                        ? voicePacks.installedPacks() : []

    Connections {
        target: voicePacks
        function onChanged() { page.packsVersion++ }
    }

    SettingsCard {
        title: qsTr("Narrators")
        glyph: Icons.headphones
        description: qsTr("Each book keeps its own narrator and style, chosen in the Narrator sheet while reading (V).")

        Flow {
            Layout.fillWidth: true
            spacing: 8

            ProviderPill {
                where: "local"
                showIcon: true
                text: qsTr("Built-in voices · free, private, offline")
            }
            ProviderPill {
                where: "cloud"
                showIcon: true
                text: qsTr("Gemini voices · expressive, with your key")
            }
        }
    }

    SettingsCard {
        visible: voicePacks.supported
        title: qsTr("Built-in voices")
        glyph: Icons.computer
        glyphColor: theme.positive
        description: qsTr("Made on this computer, without a key. Plainer than Gemini's and read without styles.")

        Repeater {
            model: page.installedPacks
            delegate: ColumnLayout {
                id: packEntry
                required property var modelData
                required property int index
                property bool confirming: false

                Timer {
                    running: packEntry.confirming
                    interval: 4000
                    onTriggered: packEntry.confirming = false
                }

                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    visible: packEntry.index > 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: theme.hairline
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Label {
                            Layout.fillWidth: true
                            text: packEntry.modelData.title
                            color: theme.ink1
                            font.pixelSize: 13
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                        }
                        Label {
                            visible: !!packEntry.modelData.license
                            Layout.fillWidth: true
                            text: packEntry.modelData.license || ""
                            color: theme.ink4
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                    }

                    Label {
                        text: Format.size((packEntry.modelData.megabytes || 0) * 1024 * 1024, Qt.locale())
                        color: theme.ink3
                        font.pixelSize: 12
                        font.features: ({ "tnum": 1 })
                    }

                    TextButton {
                        size: "compact"
                        enabled: !voicePacks.busy
                        text: packEntry.confirming ? qsTr("Remove?") : qsTr("Remove")
                        onClicked: {
                            if (!packEntry.confirming) {
                                packEntry.confirming = true
                                return
                            }
                            packEntry.confirming = false
                            voicePacks.remove(packEntry.modelData.id)
                        }
                    }
                }
            }
        }

        Label {
            visible: page.installedPacks.length === 0
            Layout.fillWidth: true
            text: qsTr("None downloaded yet. Press P while reading to download one, or choose in Narrator (V).")
            color: theme.ink3
            font.pixelSize: 13
            wrapMode: Text.Wrap
            lineHeight: 1.4
        }
    }

    SettingsCard {
        id: cacheCard
        title: qsTr("Audio kept for listening again")
        glyph: Icons.disc
        description: qsTr("Narration is cached on disk so chapters heard once replay instantly and cost nothing the second time.")

        property bool confirmingClear: false
        Timer {
            running: cacheCard.confirmingClear
            interval: 4000
            onTriggered: cacheCard.confirmingClear = false
        }

        SettingsRow {
            title: audioCache.megabytes > 0.001
                   ? qsTr("%1 kept on disk").arg(Format.size(audioCache.megabytes * 1024 * 1024, Qt.locale()))
                   : qsTr("No audio kept")
            description: audioCache.megabytes > 0.001
                         ? qsTr("Clearing removes cached clips; saved M4B audiobooks in Music/Dogear stay untouched.")
                         : ""
            TextButton {
                size: "compact"
                enabled: audioCache.megabytes > 0.001
                text: cacheCard.confirmingClear ? qsTr("Clear cache?") : qsTr("Clear")
                onClicked: {
                    if (!cacheCard.confirmingClear) {
                        cacheCard.confirmingClear = true
                        return
                    }
                    cacheCard.confirmingClear = false
                    audioCache.clear()
                }
            }
        }
    }
}
