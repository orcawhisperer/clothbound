import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

import "../components/Format.js" as Format
import "../components/Icons.js" as Icons

// Settings › About: version, licence, diagnostics summary, and credits.
ColumnLayout {
    id: page

    spacing: 16

    function prepare() {
        systemCheck.refresh()
    }

    readonly property var machineInfo: systemCheck.machine || ({})
    readonly property string hardwareSummary: {
        const parts = []
        if (machineInfo.architecture)
            parts.push(machineInfo.architecture)
        if (machineInfo.cores > 0)
            parts.push(qsTr("%n cores", "", machineInfo.cores))
        if (machineInfo.memoryGb > 0)
            parts.push(qsTr("%1 GB RAM").arg(Math.round(machineInfo.memoryGb)))
        if (machineInfo.graphics)
            parts.push(machineInfo.graphicsMemoryGb > 0
                       ? qsTr("%1 (%2 GB)").arg(machineInfo.graphics).arg(Math.round(machineInfo.graphicsMemoryGb))
                       : machineInfo.graphics)
        return parts.length > 0 ? parts.join(" · ") : Qt.platform.os
    }
    readonly property string presetLabel: ai.preset === "local" ? qsTr("This computer")
                                        : ai.preset === "hybrid" ? qsTr("Hybrid")
                                        : qsTr("Cloud")
    readonly property string appearanceLabel: theme.appearance === "dark" ? qsTr("Dark")
                                            : theme.appearance === "light" ? qsTr("Light")
                                            : qsTr("Match desktop")
    readonly property string aiSummary: {
        const parts = [presetLabel]
        if (ai.hasKey)
            parts.push(ai.connected ? qsTr("Gemini connected") : ai.checkFailed ? qsTr("Gemini error") : qsTr("Gemini key saved"))
        if (ai.hasLocal)
            parts.push(ai.localName + (ai.localTextModel ? " (" + ai.localTextModel + ")" : ""))
        return parts.join(" · ")
    }
    readonly property string speechSummary: {
        if (!voicePacks.supported)
            return qsTr("Unsupported processor")
        const engine = voicePacks.engineInstalled
                     ? qsTr("Sherpa-ONNX %1 (%2 voices)").arg(voicePacks.engineVersion).arg(voicePacks.installedVoiceCount)
                     : qsTr("Not downloaded")
        const exportTool = audioCache.ffmpegAvailable ? qsTr("ffmpeg ready") : qsTr("ffmpeg missing")
        return engine + " · " + exportTool
    }

    function diagnosticsText() {
        const lines = [
            "Dogear " + Qt.application.version,
            "Platform: " + Qt.platform.os + (machineInfo.architecture ? " (" + machineInfo.architecture + ")" : ""),
            "Hardware: " + hardwareSummary,
            "Language: " + translator.language,
            "Appearance: " + theme.appearance + (theme.omarchy ? " (Omarchy)" : " (standalone)")
                + (theme.reducedMotion ? ", reduced motion" : ""),
            "Library: " + library.bookCount + " books, " + library.folders.length + " folders"
                + ", FTS5 " + (libraryIndex.available ? "ready (" + libraryIndex.bookCount + " indexed)" : "unavailable"),
            "Intelligence: " + aiSummary,
            "Built-in speech: " + speechSummary,
            "Audio cache: " + Format.size(audioCache.megabytes * 1024 * 1024, Qt.locale())
                + " (" + audioCache.clipCount + " clips)"
        ]
        return lines.join("\n")
    }

    SettingsCard {
        RowLayout {
            Layout.fillWidth: true
            spacing: 16
            Image {
                source: "qrc:/dogear.svg"
                sourceSize.width: 56
                sourceSize.height: 56
                Layout.preferredWidth: 56
                Layout.preferredHeight: 56
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 3
                Label {
                    text: "Dogear"
                    color: theme.ink1
                    font.family: "Literata"
                    font.pixelSize: 22
                    font.weight: Font.DemiBold
                }
                Label {
                    text: qsTr("Version %1").arg(Qt.application.version)
                    color: theme.ink3
                    font.pixelSize: 13
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("An ebook reader for Omarchy. MIT licensed.")
                    color: theme.ink2
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            TextButton {
                size: "compact"
                text: qsTr("What's new")
                onClicked: welcome.show("whatsnew")
            }
            TextButton {
                size: "compact"
                ghost: true
                text: qsTr("Welcome guide")
                onClicked: welcome.show("welcome")
            }
        }
    }

    SettingsCard {
        title: qsTr("Diagnostics")
        glyph: Icons.info
        description: qsTr("Environment summary for bug reports. Never includes API keys or book titles.")
        headerExtra: TextButton {
            size: "compact"
            glyph: Icons.copy
            text: qsTr("Copy")
            onClicked: {
                reader.copyText(page.diagnosticsText())
                toast.show(qsTr("Diagnostics copied"))
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: 16
            rowSpacing: 8

            Label { text: qsTr("Version"); color: theme.ink3; font.pixelSize: 12 }
            Label {
                Layout.fillWidth: true
                text: Qt.application.version
                color: theme.ink1
                font.pixelSize: 12
                font.features: ({ "tnum": 1 })
                elide: Text.ElideRight
            }

            Label { text: qsTr("System"); color: theme.ink3; font.pixelSize: 12 }
            Label {
                Layout.fillWidth: true
                text: page.hardwareSummary
                color: theme.ink1
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            Label { text: qsTr("Appearance"); color: theme.ink3; font.pixelSize: 12 }
            Label {
                Layout.fillWidth: true
                text: (theme.omarchy ? qsTr("Omarchy theme · %1").arg(page.appearanceLabel)
                                     : qsTr("Standalone · %1").arg(page.appearanceLabel))
                      + (theme.reducedMotion ? " · " + qsTr("Reduced motion") : "")
                color: theme.ink1
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            Label { text: qsTr("Library"); color: theme.ink3; font.pixelSize: 12 }
            Label {
                Layout.fillWidth: true
                text: qsTr("%n books", "", library.bookCount)
                      + " · "
                      + (libraryIndex.available ? qsTr("FTS5 ready") : qsTr("FTS5 unavailable"))
                color: theme.ink1
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            Label { text: qsTr("Intelligence"); color: theme.ink3; font.pixelSize: 12 }
            Label {
                Layout.fillWidth: true
                text: page.aiSummary
                color: theme.ink1
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            Label { text: qsTr("Built-in speech"); color: theme.ink3; font.pixelSize: 12 }
            Label {
                Layout.fillWidth: true
                text: page.speechSummary
                color: theme.ink1
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
        }
    }

    SettingsCard {
        title: qsTr("Credits")
        glyph: Icons.bookOpen
        Label {
            Layout.fillWidth: true
            textFormat: Text.StyledText
            text: qsTr("Built-in voices use <b>sherpa-onnx</b> (Apache 2.0) with <b>ONNX Runtime</b> (MIT) and <b>espeak-ng</b> (GPL 3.0), downloaded with the voices; <b>Kokoro-82M</b> (Apache 2.0); <b>Pocket TTS</b> by Kyutai (CC BY 4.0, its voices from Kyutai's recordings); and <b>Piper</b> voices (MIT, each voice under its recording's own license, in its folder). The reading face is <b>Literata</b> and the interface face <b>Inter</b> (both OFL).")
            color: theme.ink2
            font.pixelSize: 12
            wrapMode: Text.Wrap
            lineHeight: 1.4
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Dogear reads with Qt (LGPL) and Chromium's engine.")
            color: theme.ink3
            font.pixelSize: 12
            wrapMode: Text.Wrap
            lineHeight: 1.35
        }
    }
}
