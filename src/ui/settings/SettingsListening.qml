import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

import "../components/Format.js" as Format
import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui

// Settings › Voices (page id "listening"): default narrator, playback speed,
// delivery style, sleep timer defaults, built-in voice packs, Studio & M4B
// audiobook export, and cached audio management.
ColumnLayout {
    id: page

    spacing: 16

    Settings {
        id: listenPrefs
        location: settingsUrl
        category: "listen"
        property string voice: ""
        property string voiceName: ""
        property string defaultSleep: "off"
    }

    property int packsVersion: 0
    property bool wasNarrationActive: false

    function prepare() {
        audioCache.refreshStats()
        if (ai.localSpeechModel !== "")
            voices.loadStudioVoices()
        if (ai.hasKey && voices.count === 0 && !voices.loading)
            voices.refresh(false)
    }

    readonly property var installedPacks: packsVersion >= 0 && voicePacks.megabytes >= 0
                                        ? voicePacks.installedPacks() : []
    readonly property var downloadablePacks: packsVersion >= 0 ? voicePacks.catalogPacks() : []
    readonly property string activeVoiceId: narration.voice !== "" ? narration.voice : listenPrefs.voice
    readonly property string activeVoiceName: narration.voiceName !== "" ? narration.voiceName : listenPrefs.voiceName

    // Ready-to-use voices for the default narrator picker (built-in installed,
    // Studio, and Gemini suggestions when configured).
    readonly property var readyVoices: {
        void(page.packsVersion); void(voices.count); void(voices.studioVoices.length); void(ai.hasKey)
        const list = []
        const seen = {}
        const add = (id, name, source, subtitle) => {
            if (!id || seen[id])
                return
            seen[id] = true
            list.push({ id: id, name: name || id, source: source, subtitle: subtitle || "" })
        }
        if (voicePacks.engineInstalled) {
            const installed = voicePacks.searchVoices("", "", true)
            for (let i = 0; i < installed.length && i < 24; ++i)
                add(installed[i].id, installed[i].name, "local", installed[i].engine + " · " + installed[i].accent)
        }
        const studio = voices.studioVoices || []
        for (let s = 0; s < studio.length; ++s)
            add(studio[s].id, studio[s].name, "local", qsTr("Studio"))
        if (ai.hasKey) {
            const suggested = voices.suggestions("")
            for (let g = 0; g < suggested.length && g < 16; ++g)
                add(suggested[g].id, suggested[g].title || suggested[g].name, "cloud", qsTr("Gemini"))
        }
        if (page.activeVoiceId !== "" && !seen[page.activeVoiceId]) {
            const isLocal = page.activeVoiceId.startsWith("builtin:") || page.activeVoiceId.startsWith("studio:")
            add(page.activeVoiceId, page.activeVoiceName || page.activeVoiceId, isLocal ? "local" : "cloud", "")
        }
        return list
    }

    readonly property var sleepOptions: [
        { value: "off",     label: qsTr("Off") },
        { value: "15",      label: qsTr("In %n minutes", "", 15) },
        { value: "30",      label: qsTr("In %n minutes", "", 30) },
        { value: "60",      label: qsTr("In %n minutes", "", 60) },
        { value: "chapter", label: qsTr("End of chapter") }
    ]

    function formatCountdown(seconds) {
        const s = Math.max(0, Math.round(seconds))
        const m = Math.floor(s / 60)
        const rem = s % 60
        return m + ":" + (rem < 10 ? "0" : "") + rem
    }

    function applyDefaultSleep() {
        const pref = listenPrefs.defaultSleep
        if (pref === "chapter")
            narration.sleepAtEndOfChapter()
        else if (pref === "15" || pref === "30" || pref === "60")
            narration.sleepIn(parseInt(pref, 10))
    }

    Connections {
        target: voicePacks
        function onChanged() { page.packsVersion++ }
    }
    Connections {
        target: narration
        function onVoiceChanged() {
            if (narration.voice !== "") {
                listenPrefs.voice = narration.voice
                listenPrefs.voiceName = narration.voiceName
            }
        }
        function onStateChanged() {
            const active = narration.active
            if (active && !page.wasNarrationActive && listenPrefs.defaultSleep !== "off"
                && narration.sleepSecondsLeft <= 0 && !narration.sleepAtChapterEnd) {
                page.applyDefaultSleep()
            }
            page.wasNarrationActive = active
        }
    }

    onVisibleChanged: {
        if (!visible && voices.previewing !== "")
            voices.stopPreview()
    }

    // --- 1. Playback & defaults --------------------------------------------
    SettingsCard {
        title: qsTr("Playback & defaults")
        glyph: Icons.headphones
        description: qsTr("Applied to new books automatically. Each book can still choose its own narrator in Narrator (V).")

        SettingsRow {
            title: qsTr("Default narrator")
            description: page.readyVoices.length > 0
                         ? qsTr("Used when you press P on a book that doesn't have a narrator yet.")
                         : qsTr("Download a built-in voice below or connect Gemini in Intelligence to choose a narrator.")

            ProviderPill {
                visible: page.activeVoiceId !== ""
                anchors.verticalCenter: parent.verticalCenter
                where: page.activeVoiceId.startsWith("builtin:") || page.activeVoiceId.startsWith("studio:")
                       ? "local" : "cloud"
                showIcon: true
                text: page.activeVoiceId.startsWith("studio:") ? qsTr("Studio")
                    : page.activeVoiceId.startsWith("builtin:") ? qsTr("Built-in")
                    : qsTr("Gemini")
            }
            ComboBox {
                visible: page.readyVoices.length > 0
                width: 200
                model: page.readyVoices.map(v => v.subtitle !== "" ? (v.name + " · " + v.subtitle) : v.name)
                currentIndex: Math.max(0, page.readyVoices.findIndex(v => v.id === page.activeVoiceId))
                onActivated: (index) => {
                    const picked = page.readyVoices[index]
                    if (!picked)
                        return
                    narration.setVoice(picked.id, picked.name)
                    listenPrefs.voice = picked.id
                    listenPrefs.voiceName = picked.name
                }
            }
            TextButton {
                visible: page.activeVoiceId !== "" && ai.canSpeakWith(page.activeVoiceId)
                size: "compact"
                readonly property bool thisPreview: voices.previewing === page.activeVoiceId
                text: thisPreview ? qsTr("Stop") : qsTr("Preview")
                onClicked: {
                    if (thisPreview)
                        voices.stopPreview()
                    else
                        voices.preview(page.activeVoiceId, qsTr("This is how your books will sound when read aloud."))
                }
            }
            BusyIndicator {
                visible: voices.previewing === page.activeVoiceId && voices.previewBuffering
                running: visible
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: 16
                implicitHeight: 16
            }
        }

        Label {
            visible: voices.problem !== ""
            Layout.fillWidth: true
            text: voices.problem
            color: theme.danger
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }

        SettingsRow {
            title: qsTr("Playback speed")
            description: qsTr("%1×").arg(Number(narration.rate).toLocaleString(Qt.locale(), 'f', 2))
            Slider {
                width: 200
                from: 0.75; to: 2.0; stepSize: 0.05
                value: narration.rate
                onMoved: narration.rate = Math.round(value * 20) / 20
            }
        }
        Flow {
            Layout.fillWidth: true
            spacing: 6
            Repeater {
                model: [0.9, 1.0, 1.25, 1.5, 1.75]
                delegate: TextButton {
                    required property real modelData
                    size: "compact"
                    primary: Math.abs(narration.rate - modelData) < 0.01
                    text: Number(modelData).toLocaleString(Qt.locale(), 'f', (modelData === 1.25 || modelData === 1.75) ? 2 : 1) + "×"
                    onClicked: narration.rate = modelData
                }
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }

        SettingsRow {
            title: qsTr("Delivery style")
            description: qsTr("How expressively Gemini and style-capable voices read each scene. Built-in voices always read plainly.")
            Segmented {
                model: [{ value: "plain", label: qsTr("Plain") },
                        { value: "natural", label: qsTr("Natural") },
                        { value: "dramatic", label: qsTr("Dramatic") }]
                current: narration.performance
                onActivated: (value) => narration.performance = value
            }
        }

        Rectangle {
            visible: ai.hasKey
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: theme.hairline
        }
        SettingsRow {
            visible: ai.hasKey
            title: qsTr("Lower-cost Gemini voice")
            description: qsTr("Use Gemini Flash Lite for narration to cut online listening cost roughly in half.")
            Switch {
                checked: narration.economy
                onToggled: narration.economy = checked
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }

        SettingsRow {
            title: qsTr("Default sleep timer")
            description: narration.active && (narration.sleepSecondsLeft > 0 || narration.sleepAtChapterEnd)
                         ? (narration.sleepAtChapterEnd
                            ? qsTr("Active now: stopping at the end of this chapter.")
                            : qsTr("Active now: %1 remaining (fades out gently in the final 10 seconds).").arg(page.formatCountdown(narration.sleepSecondsLeft)))
                         : qsTr("Automatically arm a sleep timer whenever you start listening.")
            ComboBox {
                width: 170
                model: page.sleepOptions.map(o => o.label)
                currentIndex: Math.max(0, page.sleepOptions.findIndex(o => o.value === listenPrefs.defaultSleep))
                onActivated: (index) => {
                    const picked = page.sleepOptions[index].value
                    listenPrefs.defaultSleep = picked
                    if (narration.active) {
                        if (picked === "off")
                            narration.sleepIn(0)
                        else
                            page.applyDefaultSleep()
                    }
                }
            }
            TextButton {
                visible: narration.active && (narration.sleepSecondsLeft > 0 || narration.sleepAtChapterEnd)
                size: "compact"
                ghost: true
                text: qsTr("Stop timer")
                onClicked: narration.sleepIn(0)
            }
        }
    }

    // --- 2. Built-in voices (Sherpa-ONNX / Kokoro / Pocket / Piper) --------
    SettingsCard {
        visible: voicePacks.supported
        title: qsTr("Built-in voices")
        glyph: Icons.computer
        glyphColor: theme.positive
        description: qsTr("Run privately on this computer without an API key or internet connection.")
        headerExtra: Badge {
            showDot: true
            where: voicePacks.engineInstalled ? "local" : "none"
            text: voicePacks.engineInstalled
                  ? qsTr("Sherpa-ONNX %1").arg(voicePacks.engineVersion)
                  : qsTr("Not installed")
        }

        // Engine runtime details when installed.
        Label {
            visible: voicePacks.engineInstalled
            Layout.fillWidth: true
            text: {
                const parts = [
                    qsTr("%1 voices ready").arg(voicePacks.installedVoiceCount),
                    qsTr("%1 CPU threads").arg(localSpeech.threads),
                    Format.size(voicePacks.megabytes * 1024 * 1024, Qt.locale())
                ]
                if (localSpeech.averageSpeed > 0)
                    parts.push(qsTr("%1× real time").arg(Number(localSpeech.averageSpeed).toLocaleString(Qt.locale(), 'f', 1)))
                return parts.join(" · ")
            }
            color: theme.ink3
            font.pixelSize: 12
            font.features: ({ "tnum": 1 })
            wrapMode: Text.Wrap
        }

        // Active download banner with real progress and Cancel.
        Rectangle {
            visible: voicePacks.busy
            Layout.fillWidth: true
            implicitHeight: dlCol.implicitHeight + 20
            radius: Ui.radius(8, theme.radius)
            color: theme.accentSoft
            border.width: 1
            border.color: Ui.tint(theme.accent, 0.25)

            ColumnLayout {
                id: dlCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 10
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    BusyIndicator {
                        running: voicePacks.busy
                        implicitWidth: 14
                        implicitHeight: 14
                    }
                    Label {
                        Layout.fillWidth: true
                        text: voicePacks.installing !== ""
                              ? qsTr("Downloading %1… %2%").arg(voicePacks.installing).arg(Math.round(voicePacks.progress * 100))
                              : qsTr("Installing voice pack…")
                        color: theme.ink1
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }
                    TextButton {
                        size: "compact"
                        text: qsTr("Cancel")
                        onClicked: voicePacks.cancel()
                    }
                }
                ProgressBar {
                    Layout.fillWidth: true
                    value: voicePacks.progress
                }
            }
        }

        Label {
            visible: voicePacks.problem !== ""
            Layout.fillWidth: true
            text: voicePacks.problem
            color: theme.danger
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }

        // Installed packs list.
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
                            Layout.fillWidth: true
                            text: {
                                const parts = []
                                if (packEntry.modelData.voiceCount > 0)
                                    parts.push(qsTr("%1 voices").arg(packEntry.modelData.voiceCount))
                                if (packEntry.modelData.license)
                                    parts.push(packEntry.modelData.license)
                                return parts.join(" · ")
                            }
                            visible: text !== ""
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
                            if (voices.previewing !== "")
                                voices.stopPreview()
                            if (narration.active && narration.voice.startsWith("builtin:"))
                                narration.stop()
                            const removedTitle = packEntry.modelData.title
                            voicePacks.remove(packEntry.modelData.id)
                            toast.show(qsTr("Removed %1").arg(removedTitle))
                        }
                    }
                }
            }
        }

        // Featured downloadable voice packs not yet installed.
        ColumnLayout {
            readonly property var pendingPacks: page.downloadablePacks.filter(p => !p.installed)
            visible: pendingPacks.length > 0
            Layout.fillWidth: true
            spacing: 8

            Rectangle {
                visible: page.installedPacks.length > 0
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: theme.hairline
            }

            Repeater {
                model: parent.pendingPacks
                delegate: ColumnLayout {
                    id: catalogEntry
                    required property var modelData
                    required property int index
                    Layout.fillWidth: true
                    spacing: 8

                    Rectangle {
                        visible: catalogEntry.index > 0
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
                                text: catalogEntry.modelData.title
                                color: theme.ink1
                                font.pixelSize: 13
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("%1 voices · %2 download")
                                      .arg(catalogEntry.modelData.voiceCount)
                                      .arg(Format.size(catalogEntry.modelData.bytes, Qt.locale()))
                                color: theme.ink3
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                        }

                        TextButton {
                            size: "compact"
                            primary: page.installedPacks.length === 0 && catalogEntry.modelData.kind === "kokoro"
                            glyph: Icons.download
                            enabled: !voicePacks.busy
                            text: qsTr("Download")
                            onClicked: voicePacks.installPack(catalogEntry.modelData.id)
                        }
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            text: qsTr("Individual Piper voices for 40+ more languages can be downloaded on demand in Narrator (V) while reading.")
            color: theme.ink4
            font.pixelSize: 11
            wrapMode: Text.Wrap
            lineHeight: 1.35
        }
    }

    // --- 3. Studio & Audiobook export --------------------------------------
    SettingsCard {
        title: qsTr("Studio & audiobooks")
        glyph: Icons.disc
        description: qsTr("Local Studio voice servers and M4B audiobook export settings.")

        SettingsRow {
            readonly property bool studioConnected: ai.localSpeechModel !== ""
            title: qsTr("Studio voice server")
            description: studioConnected
                         ? qsTr("Connected to %1 (%2) · %3 Studio voices available.")
                               .arg(ai.localName).arg(ai.localSpeechModel).arg(voices.studioVoices.length)
                         : qsTr("Connect an OpenAI-compatible local speech server in Settings › Intelligence for Studio voices.")
            TextButton {
                visible: ai.localSpeechModel !== ""
                size: "compact"
                text: qsTr("Refresh")
                onClicked: {
                    voices.loadStudioVoices()
                    toast.show(qsTr("Refreshed Studio voices"))
                }
            }
            TextButton {
                size: "compact"
                ghost: ai.localSpeechModel !== ""
                text: ai.localSpeechModel !== "" ? qsTr("Configure…") : qsTr("Set up…")
                onClicked: settingsView.show("intelligence")
            }
        }

        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }

        SettingsRow {
            title: qsTr("Audiobook export folder")
            description: {
                const folder = audioCache.audiobookFolder.replace(/^\/home\/[^/]+/, "~")
                if (!audioCache.ffmpegAvailable)
                    return qsTr("%1 · ffmpeg not found on PATH (install ffmpeg to save M4B audiobooks).").arg(folder)
                return qsTr("Saved M4B files with chapter markers go to %1 · ffmpeg ready.").arg(folder)
            }
            Badge {
                anchors.verticalCenter: parent.verticalCenter
                showDot: true
                where: audioCache.ffmpegAvailable ? "local" : "cloud"
                text: audioCache.ffmpegAvailable ? qsTr("ffmpeg ready") : qsTr("ffmpeg missing")
            }
            TextButton {
                size: "compact"
                text: qsTr("Open folder")
                onClicked: audioCache.openAudiobookFolder()
            }
        }

        // Live export progress when an audiobook is currently building.
        ColumnLayout {
            visible: audiobook.busy
            Layout.fillWidth: true
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                BusyIndicator {
                    running: audiobook.busy
                    implicitWidth: 14
                    implicitHeight: 14
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Exporting M4B audiobook… %1%").arg(Math.round(audiobook.progress * 100))
                    color: theme.ink2
                    font.pixelSize: 12
                }
                TextButton {
                    size: "compact"
                    text: qsTr("Cancel")
                    onClicked: audiobook.cancel()
                }
            }
            ProgressBar {
                Layout.fillWidth: true
                value: audiobook.progress
            }
        }
    }

    // --- 4. Audio cache ----------------------------------------------------
    SettingsCard {
        id: cacheCard
        title: qsTr("Audio kept for listening again")
        glyph: Icons.tray
        description: qsTr("Narration is cached on disk so chapters heard once replay instantly and cost nothing the second time.")

        property bool confirmingClear: false
        readonly property bool hasCachedAudio: audioCache.clipCount > 0 || audioCache.megabytes > 0

        Timer {
            running: cacheCard.confirmingClear
            interval: 4000
            onTriggered: cacheCard.confirmingClear = false
        }

        SettingsRow {
            title: audioCache.clearing
                   ? qsTr("Clearing audio cache…")
                   : cacheCard.hasCachedAudio
                   ? qsTr("%1 kept on disk (%2 clips)")
                         .arg(Format.size(audioCache.megabytes * 1024 * 1024, Qt.locale()))
                         .arg(audioCache.clipCount.toLocaleString(Qt.locale(), 'f', 0))
                   : qsTr("No audio kept")
            description: cacheCard.hasCachedAudio
                         ? qsTr("Clearing removes cached clips; saved M4B audiobooks in %1 stay untouched.")
                               .arg(audioCache.audiobookFolder.replace(/^\/home\/[^/]+/, "~"))
                         : ""
            BusyIndicator {
                visible: audioCache.clearing
                running: visible
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: 16
                implicitHeight: 16
            }
            TextButton {
                size: "compact"
                enabled: cacheCard.hasCachedAudio && !audioCache.clearing
                text: cacheCard.confirmingClear ? qsTr("Clear cache?") : qsTr("Clear")
                onClicked: {
                    if (!cacheCard.confirmingClear) {
                        cacheCard.confirmingClear = true
                        return
                    }
                    cacheCard.confirmingClear = false
                    audioCache.clear()
                    toast.show(qsTr("Audio cache cleared"))
                }
            }
        }
    }
}
