import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Format.js" as Format
import "Icons.js" as Icons
import "Ui.js" as Ui

// Who reads the book, and how. Virtualized ListView keeps browsing 200+
// built-in and Gemini voices instant, with debounced search, language &
// installed-pack management, audio cache controls, Flash Lite toggle, editable
// AI casting proposals, and Studio M4B export with folder reveal.
FocusScope {
    id: panel

    property bool open: false
    property var look
    // A passage from the page in view, for previews.
    property string sample: ""
    property string query: ""
    property string debouncedQuery: ""
    // Narration paused for a preview picks up again when the panel closes.
    property bool resumeAfter: false
    property string draftName: ""
    property string draftDescription: ""
    property string draftGender: ""
    property string draftLanguage: ""

    signal closed()
    signal sampleWanted()

    // Which voices the panel shows: "builtin", "gemini", or "studio".
    property string source: "gemini"
    // Built-in language filter: "__book__" (book's language), "" (all),
    // "__installed__" (installed only), or a BCP-47 base code ("en", "de"...).
    property string builtinFilter: "__book__"
    property bool allBuiltins: false
    property string pendingVoice: ""
    property int packsVersion: 0

    readonly property var accentChoices: [
        { code: "en-US", label: "English (US) · en-US" },
        { code: "en-GB", label: "English (UK) · en-GB" },
        { code: "en-AU", label: "English (Australia) · en-AU" },
        { code: "en-IE", label: "English (Ireland) · en-IE" },
        { code: "en-IN", label: "English (India) · en-IN" },
        { code: "en-CA", label: "English (Canada) · en-CA" },
        { code: "fr-FR", label: "Français (France) · fr-FR" },
        { code: "fr-CA", label: "Français (Canada) · fr-CA" },
        { code: "de-DE", label: "Deutsch · de-DE" },
        { code: "es-ES", label: "Español (España) · es-ES" },
        { code: "es-MX", label: "Español (México) · es-MX" },
        { code: "it-IT", label: "Italiano · it-IT" },
        { code: "pt-BR", label: "Português (Brasil) · pt-BR" },
        { code: "pt-PT", label: "Português (Portugal) · pt-PT" },
        { code: "nl-NL", label: "Nederlands · nl-NL" },
        { code: "pl-PL", label: "Polski · pl-PL" },
        { code: "ru-RU", label: "Русский · ru-RU" },
        { code: "ja-JP", label: "日本語 · ja-JP" },
        { code: "zh-CN", label: "中文 · zh-CN" },
        { code: "ko-KR", label: "한국어 · ko-KR" }
    ]

    Timer {
        id: searchDebounce
        interval: 180
        onTriggered: panel.debouncedQuery = panel.query
    }

    function updateQuery(text) {
        query = text
        if (text.trim() === "") {
            searchDebounce.stop()
            debouncedQuery = ""
        } else {
            searchDebounce.restart()
        }
    }

    function show() {
        open = true
        searchDebounce.stop()
        query = ""
        debouncedQuery = ""
        builtinFilter = reader.language !== "" ? "__book__" : ""
        source = studioVoice ? "studio"
               : builtinVoice || (!ai.hasKey && voicePacks.supported) ? "builtin" : "gemini"
        if (ai.localSpeechModel !== "")
            voices.loadStudioVoices()
        if (ai.hasKey)
            voices.refresh()
        sampleWanted()
        forceActiveFocus()
    }
    function close() {
        if (!open)
            return
        open = false
        voices.stopPreview()
        if (resumeAfter) {
            resumeAfter = false
            narration.resume()
        }
        closed()
    }
    function preview(id) {
        if (voices.previewing === id) {
            voices.stopPreview()
            return
        }
        if (narration.playing) {
            narration.pause()
            resumeAfter = true
        }
        voices.preview(id, sample)
    }
    function choose(voice) {
        voices.stopPreview()
        if (voice.kind === "builtin" && !voice.installed) {
            pendingVoice = voice.id
            voicePacks.installVoice(voice.id)
            return
        }
        narration.setVoice(voice.id, voice.title || voice.name)
    }
    function details(voice) {
        const parts = []
        if (voice.kind === "builtin") {
            parts.push(voice.engine, voice.languageName)
            if (voice.grade)
                parts.push(qsTr("grade %1").arg(voice.grade))
            else if (voice.quality)
                parts.push(voice.quality === "high" ? qsTr("high quality") : voice.quality === "medium" ? qsTr("medium quality")
                                                                                                       : qsTr("low quality"))
            const speed = voice.installed ? localSpeech.speedOf(voice.id) : 0
            if (!voice.installed)
                parts.push(qsTr("%1 MB to download").arg(Math.round(voice.bytes / 1e6).toLocaleString(Qt.locale(), 'f', 0)))
            else if (speed > 0 && translator.language !== "")
                parts.push(qsTr("%1× real time here").arg(speed.toLocaleString(Qt.locale(), 'f', 1)))
            return parts.join(" · ")
        }
        if (voice.kind === "designed") {
            parts.push(qsTr("Designed"))
            if (voice.expires && !isNaN(voice.expires.getTime()) && translator.language !== "")
                parts.push(qsTr("kept until %1").arg(voice.expires.toLocaleDateString(Qt.locale(), "MMM yyyy")))
            return parts.join(" · ")
        }
        const gender = voice.gender === "male" ? qsTr("male") : voice.gender === "female" ? qsTr("female") : ""
        const pitch = voice.pitch === "low" ? qsTr("low") : voice.pitch === "medium" ? qsTr("medium")
                    : voice.pitch === "high" ? qsTr("high") : ""
        if (gender || pitch)
            parts.push([gender, pitch].filter(word => word).join(", "))
        if (voice.age > 0)
            parts.push(qsTr("age %1").arg(voice.age))
        return parts.join(" · ")
    }
    function removePack(packId) {
        if (voicePacks.busy)
            return
        if (narration.active && narration.voice.startsWith("builtin:")) {
            const cur = voicePacks.voice(narration.voice)
            if (packId === "engine" || (cur && cur.pack === packId))
                narration.stop()
        }
        voices.stopPreview()
        voicePacks.remove(packId)
    }

    // Voice families.
    readonly property bool builtinVoice: (narration.voice || "").startsWith("builtin:")
    readonly property bool studioVoice: (narration.voice || "").startsWith("studio:")
    readonly property bool localVoice: builtinVoice || studioVoice

    // Downloading for offline listening / audiobook export: this chapter, or to the end.
    readonly property int lastChapter: reader.chapterCount - 1
    readonly property var chapterEstimate: open && voices.count >= 0 && narration.voice !== undefined && narration.economy !== undefined && reader.chapterMarks.length >= 0
        ? download.estimate(reader.chapter, reader.chapter) : ({})
    readonly property var restEstimate: open && voices.count >= 0 && narration.voice !== undefined && narration.economy !== undefined && reader.chapterMarks.length >= 0
        ? download.estimate(reader.chapter, lastChapter) : ({})
    function money(dollars) {
        return dollars < 0.01 ? qsTr("under $0.01") : qsTr("about %1").arg("$" + dollars.toLocaleString(Qt.locale(), 'f', 2))
    }
    readonly property bool downloadingThisBook: download.active && download.bookId === reader.bookId
    readonly property bool offlineApplies: (ai.hasKey || ai.canSpeak || ai.localSpeechModel !== "" || voicePacks.engineInstalled || voicePacks.megabytes >= 0) &&
                                           narration.voice !== "" && ai.canSpeakWith(narration.voice)

    readonly property string language: reader.language
    readonly property var suggestions: open && voices.count >= 0 ? voices.suggestions(language) : []
    readonly property var narrators: suggestions.filter(voice => voice.kind === "library")
    readonly property var classics: suggestions.filter(voice => voice.kind === "classic")
    readonly property var yours: open && voices.count >= 0 ? voices.designedVoices() : []
    readonly property var geminiResults: open && debouncedQuery.trim() !== "" && voices.count >= 0
        ? voices.search(debouncedQuery, "", 120) : []
    readonly property var current: voices.count >= 0 && voices.studioVoices.length >= 0 && packsVersion >= 0 && narration.voice
        ? (narration.voice.startsWith("builtin:") ? voicePacks.voice(narration.voice) : voices.voice(narration.voice)) : ({})
    readonly property var castChoices: cast.available ? cast.choices() : []
    readonly property bool showCastCard: cast.available && (
        (panel.source === "builtin" && panel.builtinVoice) ||
        (panel.source === "gemini" && !panel.localVoice)
    )

    // Built-in voices & packs.
    readonly property var builtinsForBook: open && packsVersion >= 0 ? voicePacks.voices(language) : []
    readonly property var builtinInstalledForBook: builtinsForBook.filter(voice => voice.installed)
    readonly property var builtinOffer: open && packsVersion >= 0 ? voicePacks.offer(language) : ({})
    readonly property var installedPacks: open && packsVersion >= 0 ? voicePacks.installedPacks() : []
    readonly property var builtinLanguageOptions: {
        void(packsVersion)
        const opts = []
        if (panel.language !== "") {
            const bookLangName = Qt.locale(panel.language).nativeLanguageName || panel.language
            opts.push({ code: "__book__", label: qsTr("Book language (%1)").arg(bookLangName) })
        }
        opts.push({ code: "", label: qsTr("All languages") })
        opts.push({ code: "__installed__", label: qsTr("Installed only") })
        if (open) {
            const langs = voicePacks.languages()
            for (let i = 0; i < langs.length; ++i) {
                const l = langs[i]
                const suffix = l.installed > 0
                    ? qsTr(" (%1 installed)").arg(l.installed)
                    : qsTr(" (%1)").arg(l.count)
                opts.push({ code: l.code, label: l.name + suffix })
            }
        }
        return opts
    }

    // Virtualized flat model for the active tab (zero synchronous 222-item Repeaters).
    readonly property var listItems: {
        void(packsVersion)
        if (!open)
            return []
        const items = []
        if (source === "studio") {
            const list = voices.studioVoices || []
            for (let i = 0; i < list.length; ++i)
                items.push({ group: "", voice: list[i] })
            return items
        }
        if (source === "builtin" && voicePacks.supported) {
            const q = debouncedQuery.trim()
            const installedOnly = builtinFilter === "__installed__"
            const langCode = builtinFilter === "__book__" ? language
                           : builtinFilter === "__installed__" ? "" : builtinFilter
            let list = voicePacks.searchVoices(q, langCode, installedOnly)
            if (q === "" && builtinFilter === "__book__" && !allBuiltins && builtinInstalledForBook.length > 0) {
                list = builtinInstalledForBook.filter(v => v.recommended || v.id === narration.voice)
            }
            for (let i = 0; i < list.length; ++i) {
                const v = list[i]
                const grp = q !== "" ? "" : (v.installed ? qsTr("Installed") : qsTr("Available to download"))
                items.push({ group: grp, voice: v })
            }
            return items
        }
        // Gemini tab
        if (debouncedQuery.trim() !== "") {
            for (let i = 0; i < geminiResults.length; ++i)
                items.push({ group: "", voice: geminiResults[i] })
            return items
        }
        for (let i = 0; i < yours.length; ++i)
            items.push({ group: qsTr("Your voices"), voice: yours[i] })
        for (let i = 0; i < narrators.length; ++i)
            items.push({ group: qsTr("Narrators for this book"), voice: narrators[i] })
        for (let i = 0; i < classics.length; ++i)
            items.push({ group: qsTr("Classic voices"), voice: classics[i] })
        return items
    }

    readonly property int hiddenBuiltinCount: {
        if (source !== "builtin" || debouncedQuery.trim() !== "" || builtinFilter !== "__book__" || allBuiltins || builtinInstalledForBook.length === 0)
            return 0
        return Math.max(0, builtinsForBook.length - listItems.length)
    }

    Connections {
        target: voicePacks
        function onChanged() { panel.packsVersion++ }
        function onReady(voiceId) {
            panel.packsVersion++
            if (voiceId !== panel.pendingVoice)
                return
            panel.pendingVoice = ""
            const voice = voicePacks.voice(voiceId)
            narration.setVoice(voice.id, voice.name)
            if (!narration.active && narration.hasPendingStart) {
                panel.close()
                narration.startPending()
            }
        }
    }

    Connections {
        target: voices
        function onCastingChanged() {
            if (voices.casting === "proposed") {
                panel.draftName = voices.proposal.name || ""
                panel.draftDescription = voices.proposal.description || ""
                panel.draftGender = voices.proposal.gender || ""
                panel.draftLanguage = voices.proposal.language || ""
            }
        }
    }

    Keys.onEscapePressed: close()

    width: 420
    x: open ? parent.width - width : parent.width + 24
    Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    visible: x < parent.width + 20


    Rectangle {
        anchors.fill: parent
        color: panel.look.popover
        Rectangle { width: 1; height: parent.height; color: panel.look.line }
    }
    // The panel keeps its clicks and the wheel: nothing falls through to the page.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }
    Rectangle {
        anchors.right: parent.left
        width: 18
        height: parent.height
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: "transparent" }
            GradientStop { position: 1; color: Ui.tint("black", panel.look.dark ? 0.35 : 0.12) }
        }
    }

    component Heading: Label {
        color: panel.look.muted
        font.pixelSize: 12
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.8
        Layout.topMargin: 14
        Layout.fillWidth: true
        elide: Text.ElideRight
    }

    component Note: Label {
        Layout.fillWidth: true
        color: panel.look.muted
        font.pixelSize: 12
        wrapMode: Text.Wrap
    }

    component Choice: AbstractButton {
        id: choice
        property bool selected: false
        Layout.fillWidth: true
        implicitHeight: 34
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        background: Rectangle {
            radius: Math.min(6, theme.radius)
            color: choice.selected ? Ui.tint(panel.look.link, 0.2) : choice.hovered ? Ui.tint(panel.look.fg, 0.07) : Ui.tint(panel.look.fg, 0.035)
            border.width: choice.selected ? 1.5 : 0
            border.color: panel.look.link
        }
        contentItem: Label {
            text: choice.text
            color: panel.look.fg
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    component Action: AbstractButton {
        id: action
        property bool primary: false
        property bool danger: false
        implicitHeight: 34
        implicitWidth: actionLabel.implicitWidth + 24
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        background: Rectangle {
            radius: Math.min(8, theme.radius)
            color: action.danger ? Ui.tint(theme.colors.red || "#e05d5d", action.hovered ? 0.28 : 0.18)
                 : action.primary ? (action.hovered ? Qt.lighter(panel.look.link, 1.08) : panel.look.link)
                 : action.hovered ? Ui.tint(panel.look.fg, 0.1) : Ui.tint(panel.look.fg, 0.06)
            opacity: action.enabled ? 1 : 0.5
        }
        contentItem: Label {
            id: actionLabel
            text: action.text
            color: action.danger ? (theme.colors.red || "#e05d5d")
                 : action.primary ? Ui.readableOn(panel.look.link) : panel.look.fg
            font.pixelSize: 13
            font.weight: (action.primary || action.danger) ? Font.DemiBold : Font.Normal
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    // Plays a voice reading the page; a second press stops it.
    component Listen: AbstractButton {
        id: listen
        property string voiceId
        property var press: null
        readonly property bool playing: voices.previewing === voiceId
        implicitWidth: 34
        implicitHeight: 34
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        background: Rectangle {
            radius: 17
            color: listen.playing ? Ui.tint(panel.look.link, 0.22) : listen.hovered ? Ui.tint(panel.look.fg, 0.1) : Ui.tint(panel.look.fg, 0.05)
        }
        contentItem: Item {
            Glyph {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: listen.playing ? 0 : 1
                width: 15
                height: 15
                visible: !(listen.playing && voices.previewBuffering)
                path: listen.playing ? Icons.pause : Icons.play
                stroke: 2.2
                color: listen.playing ? panel.look.link : panel.look.fg
            }
            BusyIndicator {
                anchors.centerIn: parent
                implicitWidth: 26
                implicitHeight: 26
                visible: listen.playing && voices.previewBuffering
                running: visible
            }
        }
        onClicked: press ? press() : panel.preview(voiceId)
        ToolTip.visible: hovered
        ToolTip.text: playing ? qsTr("Stop") : qsTr("Hear it read this page")
        ToolTip.delay: 600
    }

    component VoiceRow: AbstractButton {
        id: row
        property var voice: ({})
        readonly property bool chosen: narration.voice === voice.id
        property bool confirming: false
        Timer {
            running: row.confirming
            interval: 4000
            onTriggered: row.confirming = false
        }
        implicitHeight: Math.max(54, rowColumn.implicitHeight + 16)
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        onClicked: panel.choose(voice)
        background: Rectangle {
            radius: Math.min(8, theme.radius)
            color: row.chosen ? Ui.tint(panel.look.link, 0.15) : row.hovered ? Ui.tint(panel.look.fg, 0.06) : "transparent"
            border.width: row.chosen ? 1.5 : 0
            border.color: panel.look.link
        }
        contentItem: RowLayout {
            spacing: 10
            ColumnLayout {
                id: rowColumn
                Layout.fillWidth: true
                Layout.leftMargin: 10
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        text: row.voice.title || row.voice.name || ""
                        color: panel.look.fg
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                        Layout.maximumWidth: 180
                    }
                    Label {
                        Layout.fillWidth: true
                        text: panel.details(row.voice)
                        color: panel.look.muted
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: row.voice.summary || ""
                    color: panel.look.muted
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }
            }
            AbstractButton {
                id: forgetButton
                visible: row.voice.kind === "designed" && (row.hovered || row.confirming)
                implicitHeight: 30
                implicitWidth: row.confirming ? confirmLabel.implicitWidth + 20 : 30
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                background: Rectangle {
                    radius: Math.min(6, theme.radius)
                    color: row.confirming ? Ui.tint(theme.colors.red || "#e05d5d", 0.2) : forgetButton.hovered ? Ui.tint(panel.look.fg, 0.1) : "transparent"
                }
                contentItem: Item {
                    Glyph {
                        anchors.centerIn: parent
                        visible: !row.confirming
                        width: 16
                        height: 16
                        path: Icons.trash
                        color: panel.look.muted
                    }
                    Label {
                        id: confirmLabel
                        anchors.centerIn: parent
                        visible: row.confirming
                        text: qsTr("Delete")
                        color: theme.colors.red || "#e05d5d"
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                }
                onClicked: {
                    if (!row.confirming) {
                        row.confirming = true
                        return
                    }
                    row.confirming = false
                    if (narration.voice === row.voice.id)
                        narration.setVoice("sulafat", "Sulafat")
                    voices.forget(row.voice.id)
                }
                ToolTip.visible: hovered && !row.confirming
                ToolTip.text: qsTr("Delete this voice")
                ToolTip.delay: 600
            }
            Listen {
                visible: !(row.voice.kind === "builtin" && !row.voice.installed)
                voiceId: row.voice.id || ""
                Layout.rightMargin: 8
            }
        }
    }

    component CastCard: Rectangle {
        Layout.fillWidth: true
        Layout.topMargin: 6
        visible: panel.showCastCard
        radius: Math.min(10, theme.radius)
        color: Ui.tint(panel.look.fg, 0.04)
        border.color: panel.look.line
        implicitHeight: castColumn.implicitHeight + 20
        ColumnLayout {
            id: castColumn
            x: 12
            y: 10
            width: parent.width - 24
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Full cast")
                    color: panel.look.fg
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
                Switch {
                    checked: cast.enabled
                    onToggled: cast.enabled = checked
                }
            }
            Label {
                Layout.fillWidth: true
                text: cast.enabled
                      ? qsTr("Each character's dialogue in a voice of their own; %1 reads the narration. Who speaks is worked out a chapter at a time (%2).").arg(narration.voiceName).arg(ai.whereFor("cast"))
                      : qsTr("Give each character a voice of their own, chosen from the narrator's family of voices.")
                color: panel.look.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
            Repeater {
                model: cast.enabled ? cast.members.slice(0, 12) : []
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: modelData.speaker
                        color: panel.look.fg
                        font.pixelSize: 13
                        elide: Text.ElideRight
                    }
                    ComboBox {
                        Layout.preferredWidth: 170
                        readonly property var choices: panel.castChoices
                        model: choices.map(c => c.name + (c.gender ? " · " + c.gender : ""))
                        currentIndex: choices.findIndex(c => c.id === modelData.voice)
                        onActivated: (index) => {
                            if (index >= 0 && index < choices.length)
                                cast.setVoice(modelData.speaker, choices[index].id)
                        }
                    }
                    Listen { voiceId: modelData.voice }
                }
            }
            Label {
                Layout.fillWidth: true
                visible: cast.enabled && cast.members.length === 0
                text: qsTr("Characters appear here as they're heard.")
                color: panel.look.muted
                font.pixelSize: 12
            }
        }
    }

    component OfflineCard: ColumnLayout {
        Layout.fillWidth: true
        visible: panel.offlineApplies || panel.downloadingThisBook || audiobook.saved !== "" || audiobook.problem !== ""
        spacing: 6

        Heading {
            text: panel.localVoice ? qsTr("Audiobook") : qsTr("Offline")
        }
        Note {
            visible: panel.offlineApplies && !panel.downloadingThisBook
            text: panel.localVoice
                  ? qsTr("Render the narration ahead on this computer so you can save it as an M4B audiobook with chapters.")
                  : qsTr("Download the narration to listen without a connection. It's made at Gemini's slower Flex tier, for half the price.")
        }
        RowLayout {
            visible: panel.offlineApplies && !panel.downloadingThisBook
            Layout.fillWidth: true
            spacing: 8
            Action {
                text: qsTr("This chapter")
                enabled: (panel.chapterEstimate.chapters || 0) > 0
                onClicked: download.start(reader.chapter, reader.chapter)
            }
            Note {
                Layout.fillWidth: true
                text: (panel.chapterEstimate.chapters || 0) > 0 && translator.language !== ""
                      ? Format.duration(panel.chapterEstimate.minutes)
                        + (panel.chapterEstimate.local ? "" : " · " + panel.money(panel.chapterEstimate.cost))
                      : ""
            }
        }
        RowLayout {
            visible: panel.offlineApplies && !panel.downloadingThisBook && (panel.restEstimate.chapters || 0) > 1
            Layout.fillWidth: true
            spacing: 8
            Action {
                text: qsTr("To the end")
                onClicked: download.start(reader.chapter, panel.lastChapter)
            }
            Note {
                Layout.fillWidth: true
                text: translator.language !== ""
                      ? qsTr("%n chapters", "", panel.restEstimate.chapters || 0) + " · " + Format.duration(panel.restEstimate.minutes || 0)
                        + (panel.restEstimate.local ? "" : " · " + panel.money(panel.restEstimate.cost || 0))
                      : ""
            }
        }
        Note {
            visible: panel.offlineApplies && !panel.downloadingThisBook && !panel.restEstimate.local && (panel.restEstimate.cost || 0) > 0
                     && !ai.fitsBudget(panel.restEstimate.cost)
            text: qsTr("Downloading to the end would pass your monthly budget; it stops there.")
            color: theme.colors.orange || "#e5a050"
        }

        // A download under way, or done.
        ColumnLayout {
            visible: panel.downloadingThisBook
            Layout.fillWidth: true
            spacing: 8
            ProgressBar {
                Layout.fillWidth: true
                value: download.fraction
                indeterminate: download.running && download.fraction === 0
            }
            Note {
                text: download.finished ? (download.firstChapter === download.lastChapter
                                           ? qsTr("This chapter is ready to listen offline.")
                                           : qsTr("Ready to listen offline, to the end of the book."))
                    : translator.language === "" ? ""
                    : qsTr("%1 of %2 ready").arg(Format.duration(download.minutesReady)).arg(Format.duration(download.minutesTotal))
                      + (download.running && download.secondsLeft > 0
                         ? " · " + qsTr("about %1 left").arg(Format.duration(download.secondsLeft / 60)) : "")
                      + (!download.running ? " · " + qsTr("paused") : "")
            }
            Label {
                Layout.fillWidth: true
                visible: download.problem !== ""
                text: download.problem
                color: theme.colors.orange || "#e5a050"
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
            RowLayout {
                spacing: 8
                Action {
                    visible: !download.finished
                    text: download.running ? qsTr("Pause") : qsTr("Go on")
                    onClicked: download.running ? download.pause() : download.resume()
                }
                Action {
                    visible: download.finished
                    enabled: !audiobook.busy
                    text: audiobook.busy ? qsTr("Saving… %1%").arg(Math.round(audiobook.progress * 100))
                                         : qsTr("Save as an audiobook")
                    onClicked: audiobook.save()
                }
                Action {
                    visible: audiobook.busy
                    text: qsTr("Cancel")
                    onClicked: audiobook.cancel()
                }
                Action {
                    enabled: !audiobook.busy
                    text: download.finished ? qsTr("Done") : qsTr("Stop")
                    onClicked: download.cancel()
                }
            }
        }

        // Completed M4B card (stays visible with Reveal in folder even after Done).
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 4
            visible: audiobook.saved !== "" || audiobook.problem !== ""
            radius: Math.min(10, theme.radius)
            color: Ui.tint(panel.look.fg, 0.04)
            border.color: panel.look.line
            implicitHeight: m4bCol.implicitHeight + 20
            ColumnLayout {
                id: m4bCol
                x: 12
                y: 10
                width: parent.width - 24
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    text: audiobook.problem !== "" ? audiobook.problem
                        : qsTr("Saved as %1, with its chapters, for any audiobook player.").arg(audiobook.saved.replace(/^.*\/(Music\/)/, "$1"))
                    color: audiobook.problem !== "" ? (theme.colors.orange || "#e5a050") : panel.look.fg
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                Action {
                    visible: audiobook.saved !== ""
                    text: qsTr("Reveal in folder")
                    onClicked: audiobook.revealSaved()
                }
            }
        }
    }

    component CastingCard: Rectangle {
        Layout.fillWidth: true
        Layout.topMargin: 12
        visible: ai.canDesignVoices
        radius: Math.min(10, theme.radius)
        color: Ui.tint(panel.look.link, 0.07)
        border.color: Ui.tint(panel.look.link, 0.28)
        implicitHeight: castingColumn.implicitHeight + 28

        ColumnLayout {
            id: castingColumn
            x: 14
            y: 14
            width: parent.width - 28
            spacing: 8

            RowLayout {
                spacing: 8
                Glyph {
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                    path: Icons.sparkle
                    color: panel.look.link
                }
                Label {
                    Layout.fillWidth: true
                    text: voices.casting === "designed" ? (voices.designed.name || "") : qsTr("A narrator for this book")
                    color: panel.look.fg
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Listen {
                    visible: voices.casting === "designed"
                    voiceId: voices.designed.id || ""
                    press: () => voices.previewing === voiceId ? voices.stopPreview() : voices.playSample()
                }
            }

            Note {
                visible: voices.casting === ""
                text: qsTr("Gemini reads a passage, describes a narrator to suit the book, and designs the voice. About $0.03, and half a minute.")
            }
            Action {
                visible: voices.casting === ""
                primary: true
                text: qsTr("Design a narrator")
                onClicked: voices.cast(reader.passageFrom(reader.chapter, 4000))
            }

            RowLayout {
                visible: voices.casting === "thinking" || voices.casting === "designing"
                spacing: 10
                BusyIndicator {
                    implicitWidth: 28
                    implicitHeight: 28
                    running: parent.visible
                }
                Note {
                    text: voices.casting === "thinking" ? qsTr("Reading the book…")
                        : qsTr("Designing “%1”. This takes about half a minute.").arg(panel.draftName)
                }
            }

            TextField {
                Layout.fillWidth: true
                visible: voices.casting === "proposed"
                text: panel.draftName
                placeholderText: qsTr("Name")
                color: panel.look.fg
                font.pixelSize: 14
                onTextEdited: panel.draftName = text
            }
            TextArea {
                Layout.fillWidth: true
                visible: voices.casting === "proposed"
                text: panel.draftDescription
                placeholderText: qsTr("How the voice sounds")
                wrapMode: TextArea.Wrap
                color: panel.look.fg
                font.pixelSize: 13
                onTextChanged: if (activeFocus) panel.draftDescription = text
            }
            // Editable gender on AI Casting proposals.
            RowLayout {
                Layout.fillWidth: true
                visible: voices.casting === "proposed"
                spacing: 8
                Choice {
                    Layout.preferredWidth: 1
                    text: qsTr("Female")
                    selected: panel.draftGender === "female"
                    onClicked: panel.draftGender = "female"
                }
                Choice {
                    Layout.preferredWidth: 1
                    text: qsTr("Male")
                    selected: panel.draftGender === "male"
                    onClicked: panel.draftGender = "male"
                }
            }
            // Editable accent / locale code on AI Casting proposals.
            RowLayout {
                Layout.fillWidth: true
                visible: voices.casting === "proposed"
                spacing: 8
                ComboBox {
                    id: accentPicker
                    Layout.fillWidth: true
                    readonly property var options: {
                        const list = panel.accentChoices.slice()
                        const cur = panel.draftLanguage.trim()
                        if (cur !== "" && !list.some(o => o.code.toLowerCase() === cur.toLowerCase()))
                            list.unshift({ code: cur, label: cur })
                        return list
                    }
                    model: options.map(o => o.label)
                    currentIndex: {
                        const cur = panel.draftLanguage.trim().toLowerCase()
                        const idx = options.findIndex(o => o.code.toLowerCase() === cur)
                        return idx >= 0 ? idx : 0
                    }
                    onActivated: (index) => {
                        if (index >= 0 && index < options.length)
                            panel.draftLanguage = options[index].code
                    }
                }
                TextField {
                    Layout.preferredWidth: 88
                    text: panel.draftLanguage
                    placeholderText: "en-GB"
                    color: panel.look.fg
                    font.pixelSize: 13
                    onTextEdited: panel.draftLanguage = text
                }
            }
            RowLayout {
                visible: voices.casting === "proposed"
                spacing: 8
                Action {
                    primary: true
                    text: qsTr("Design this voice")
                    enabled: panel.draftDescription.trim() !== ""
                    onClicked: voices.design({ name: panel.draftName, description: panel.draftDescription,
                                               gender: panel.draftGender, language: panel.draftLanguage.trim() })
                }
                Action {
                    text: qsTr("Suggest another")
                    onClicked: voices.cast(reader.passageFrom(reader.chapter, 4000))
                }
            }

            Note {
                visible: voices.casting === "designed"
                text: voices.designed.summary || ""
                maximumLineCount: 3
                elide: Text.ElideRight
            }
            RowLayout {
                visible: voices.casting === "designed"
                spacing: 8
                Action {
                    primary: true
                    text: narration.voice === voices.designed.id ? qsTr("Reading the book") : qsTr("Use this voice")
                    enabled: narration.voice !== voices.designed.id
                    onClicked: panel.choose(voices.designed)
                }
                Action {
                    text: qsTr("Try again")
                    onClicked: voices.design({ name: panel.draftName, description: panel.draftDescription,
                                               gender: panel.draftGender, language: panel.draftLanguage.trim() })
                }
                Action {
                    text: qsTr("Change it")
                    onClicked: voices.revise()
                }
            }

            Label {
                Layout.fillWidth: true
                visible: voices.casting === "failed"
                text: voices.castingProblem
                color: theme.colors.orange || "#e5a050"
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
            Action {
                visible: voices.casting === "failed"
                text: qsTr("Try again")
                onClicked: voices.cast(reader.passageFrom(reader.chapter, 4000))
            }
        }
    }

    component PackRow: RowLayout {
        id: packRow
        property var pack: ({})
        property bool confirming: false
        Timer {
            running: packRow.confirming
            interval: 4000
            onTriggered: packRow.confirming = false
        }
        Layout.fillWidth: true
        spacing: 8
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            Label {
                Layout.fillWidth: true
                text: packRow.pack.title || packRow.pack.id || ""
                color: panel.look.fg
                font.pixelSize: 13
                elide: Text.ElideRight
            }
            Label {
                Layout.fillWidth: true
                text: Format.size((packRow.pack.megabytes || 0) * 1024 * 1024, Qt.locale())
                color: panel.look.muted
                font.pixelSize: 11
            }
        }
        Action {
            enabled: !voicePacks.busy
            danger: packRow.confirming
            text: packRow.confirming ? qsTr("Remove?") : qsTr("Remove")
            onClicked: {
                if (!packRow.confirming) {
                    packRow.confirming = true
                    return
                }
                packRow.confirming = false
                panel.removePack(packRow.pack.id)
            }
        }
    }

    // The title and close stay put while the virtualized voice list scrolls.
    RowLayout {
        id: titleBar
        x: 22
        y: 22
        width: panel.width - 44
        Label {
            Layout.fillWidth: true
            text: qsTr("Narrator")
            color: panel.look.fg
            font.pixelSize: 18
            font.weight: Font.DemiBold
        }
        IconButton {
            look: panel.look
            glyph: Icons.close
            tone: panel.look.fg
            tip: qsTr("Close (Esc)")
            onClicked: panel.close()
        }
    }

    // Virtualized ListView: renders only visible voice rows even with 222+ voices.
    ListView {
        id: voiceList
        anchors.fill: parent
        anchors.topMargin: titleBar.y + titleBar.height + 6
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        spacing: 2
        cacheBuffer: 360
        model: panel.listItems
        ScrollBar.vertical: ScrollBar {}

        section.property: "group"
        section.criteria: ViewSection.FullString
        section.delegate: Item {
            required property string section
            width: voiceList.width
            height: section !== "" ? sectionLabel.implicitHeight + 20 : 0
            visible: section !== ""
            Label {
                id: sectionLabel
                x: 22
                y: 14
                width: parent.width - 44
                text: parent.section
                color: panel.look.muted
                font.pixelSize: 12
                font.weight: Font.DemiBold
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 0.8
                elide: Text.ElideRight
            }
        }

        delegate: Item {
            required property var modelData
            width: voiceList.width
            height: delegateRow.implicitHeight
            VoiceRow {
                id: delegateRow
                x: 22
                width: parent.width - 44
                voice: parent.modelData.voice
            }
        }

        header: Item {
            width: voiceList.width
            implicitHeight: headerContent.implicitHeight + 10
            height: implicitHeight

            ColumnLayout {
                id: headerContent
                x: 22
                y: 4
                width: parent.width - 44
                spacing: 6

                // The voice reading now.
                VoiceRow {
                    Layout.fillWidth: true
                    visible: !!panel.current.id
                    voice: panel.current
                }
                Note {
                    visible: !panel.current.id && narration.voiceName !== ""
                    text: narration.voiceName
                }

                // Where the voice comes from: made here, Gemini, or Studio.
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    spacing: 8
                    visible: voicePacks.supported || ai.localSpeechModel !== ""
                    Choice {
                        Layout.preferredWidth: 1
                        visible: voicePacks.supported
                        text: qsTr("On this computer")
                        selected: panel.source === "builtin"
                        onClicked: panel.source = "builtin"
                    }
                    Choice {
                        Layout.preferredWidth: 1
                        text: qsTr("Gemini")
                        selected: panel.source === "gemini"
                        onClicked: panel.source = "gemini"
                    }
                    Choice {
                        Layout.preferredWidth: 1
                        visible: ai.localSpeechModel !== ""
                        text: qsTr("Studio")
                        selected: panel.source === "studio"
                        onClicked: { panel.source = "studio"; voices.loadStudioVoices() }
                    }
                }
                Note {
                    visible: voicePacks.supported || ai.localSpeechModel !== ""
                    text: panel.source === "builtin" ? qsTr("Free, private, and offline. Plainer than Gemini's voices, and without styles.")
                        : panel.source === "studio" ? qsTr("Made by %1 on this computer with %2: free and private.").arg(ai.localName).arg(ai.localSpeechModel)
                        : qsTr("The most natural voices, with styles; they use your own Gemini key.")
                }

                CastCard {}

                // Performance style & Flash Lite toggle: available for Gemini / Studio.
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: panel.source === "gemini" || panel.source === "studio" || !voicePacks.supported
                    spacing: 6

                    Heading { text: qsTr("Style") }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Choice { text: qsTr("Plain"); selected: panel.builtinVoice || narration.performance === "plain"; onClicked: narration.performance = "plain" }
                        Choice { text: qsTr("Natural"); enabled: !panel.builtinVoice; selected: !panel.builtinVoice && narration.performance === "natural"; onClicked: narration.performance = "natural" }
                        Choice { text: qsTr("Dramatic"); enabled: !panel.builtinVoice; selected: !panel.builtinVoice && narration.performance === "dramatic"; onClicked: narration.performance = "dramatic" }
                    }
                    Note {
                        text: panel.builtinVoice ? qsTr("Built-in voices read evenly; the other styles need a Gemini or Studio voice.")
                            : narration.performance === "plain" ? qsTr("An even, clear reading with no direction. Good for study and reference.")
                            : narration.performance === "dramatic" ? qsTr("A fuller performance, for adventure, horror and genre fiction.")
                            : qsTr("A storyteller's reading: the mood follows the scene, never acted.")
                    }

                    // Flash Lite (narration.economy) toggle for Gemini speech.
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: 4
                        visible: panel.source === "gemini" || !voicePacks.supported
                        radius: Math.min(10, theme.radius)
                        color: Ui.tint(panel.look.fg, 0.04)
                        border.color: panel.look.line
                        implicitHeight: economyCol.implicitHeight + 18
                        ColumnLayout {
                            id: economyCol
                            x: 12
                            y: 9
                            width: parent.width - 24
                            spacing: 2
                            RowLayout {
                                Layout.fillWidth: true
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Flash Lite voice")
                                    color: panel.look.fg
                                    font.pixelSize: 13
                                    font.weight: Font.DemiBold
                                }
                                Switch {
                                    checked: narration.economy
                                    onToggled: narration.economy = checked
                                }
                            }
                            Note {
                                text: qsTr("Faster and about a third cheaper, with slightly simpler delivery.")
                            }
                        }
                    }
                }

                OfflineCard {}

                // Studio tab status.
                Label {
                    Layout.fillWidth: true
                    visible: panel.source === "studio" && voices.problem !== ""
                    text: voices.problem
                    color: theme.colors.orange || "#e5a050"
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }

                // Built-in tab: one-click offline setup offer, download progress, language filter.
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: voicePacks.supported && panel.source === "builtin"
                    spacing: 6

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        visible: panel.builtinInstalledForBook.length === 0 && !!panel.builtinOffer.voice
                        radius: Math.min(10, theme.radius)
                        color: Ui.tint(panel.look.link, 0.08)
                        border.color: Ui.tint(panel.look.link, 0.3)
                        implicitHeight: offerColumn.implicitHeight + 24
                        ColumnLayout {
                            id: offerColumn
                            x: 12
                            y: 12
                            width: parent.width - 24
                            spacing: 8
                            Label {
                                Layout.fillWidth: true
                                text: panel.builtinOffer.voice
                                      ? qsTr("Read this book offline with %1 (%2, %3).").arg(panel.builtinOffer.voice.name)
                                            .arg(panel.builtinOffer.voice.engine).arg(panel.builtinOffer.voice.languageName) : ""
                                color: panel.look.fg
                                font.pixelSize: 13
                                wrapMode: Text.Wrap
                            }
                            Note {
                                visible: !voicePacks.busy
                                text: panel.builtinOffer.voice
                                      ? qsTr("It downloads %1 MB once — free and private, no API key needed.").arg(Math.round(panel.builtinOffer.bytes / 1e6).toLocaleString(Qt.locale(), 'f', 0)) : ""
                            }
                            Action {
                                visible: !voicePacks.busy
                                primary: true
                                text: narration.hasPendingStart ? qsTr("Download & listen") : qsTr("Download")
                                onClicked: panel.choose(panel.builtinOffer.voice)
                            }
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: voicePacks.busy
                        spacing: 6
                        ProgressBar {
                            Layout.fillWidth: true
                            value: voicePacks.progress
                            indeterminate: voicePacks.progress >= 1.0
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Note {
                                Layout.fillWidth: true
                                text: voicePacks.progress >= 1.0
                                      ? qsTr("Installing %1…").arg(voicePacks.installing)
                                      : qsTr("Downloading %1…").arg(voicePacks.installing)
                            }
                            Action {
                                text: qsTr("Cancel")
                                onClicked: {
                                    panel.pendingVoice = ""
                                    voicePacks.cancel()
                                }
                            }
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: voicePacks.problem !== ""
                        text: voicePacks.problem
                        color: theme.colors.orange || "#e5a050"
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }

                    ComboBox {
                        Layout.fillWidth: true
                        Layout.topMargin: 6
                        model: panel.builtinLanguageOptions.map(o => o.label)
                        currentIndex: {
                            const idx = panel.builtinLanguageOptions.findIndex(o => o.code === panel.builtinFilter)
                            return idx >= 0 ? idx : 0
                        }
                        onActivated: (index) => {
                            if (index >= 0 && index < panel.builtinLanguageOptions.length) {
                                panel.builtinFilter = panel.builtinLanguageOptions[index].code
                                panel.allBuiltins = true
                            }
                        }
                    }
                }

                // Gemini tab: API key card & AI Casting card.
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: panel.source === "gemini" || (!voicePacks.supported && panel.source !== "studio")
                    spacing: 6

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                        visible: !ai.hasKey
                        radius: Math.min(10, theme.radius)
                        color: Ui.tint(panel.look.fg, 0.04)
                        border.color: panel.look.line
                        implicitHeight: keyColumn.implicitHeight + 24
                        ColumnLayout {
                            id: keyColumn
                            x: 12
                            y: 12
                            width: parent.width - 24
                            spacing: 8
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Gemini's voices need your own key. It's free to get, and you choose what it costs.")
                                color: panel.look.fg
                                font.pixelSize: 13
                                wrapMode: Text.Wrap
                            }
                            RowLayout {
                                spacing: 8
                                Action {
                                    primary: true
                                    text: qsTr("Add your key")
                                    onClicked: {
                                        panel.close()
                                        win.showAiSettings()
                                    }
                                }
                                Action {
                                    visible: voicePacks.supported
                                    text: qsTr("Use offline voices")
                                    onClicked: panel.source = "builtin"
                                }
                            }
                        }
                    }

                    CastingCard {}
                }

                // Debounced search box (shared for Built-in and Gemini tabs).
                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 10
                    visible: panel.source !== "studio"
                    implicitHeight: 36
                    radius: Math.min(8, theme.radius)
                    color: Ui.tint(panel.look.fg, searchInput.activeFocus ? 0.09 : 0.05)
                    border.width: searchInput.activeFocus ? 1.5 : 0
                    border.color: panel.look.link
                    Glyph {
                        id: searchIcon
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        path: Icons.search
                        color: panel.look.muted
                    }
                    TextInput {
                        id: searchInput
                        anchors.left: searchIcon.right
                        anchors.right: clearSearchBtn.visible ? clearSearchBtn.left : parent.right
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        color: panel.look.fg
                        selectionColor: Ui.tint(panel.look.link, 0.4)
                        font.pixelSize: 14
                        clip: true
                        text: panel.query
                        onTextEdited: panel.updateQuery(text)
                        Keys.onEscapePressed: (event) => {
                            if (text !== "") {
                                panel.updateQuery("")
                                event.accepted = true
                            } else {
                                event.accepted = false
                            }
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: !parent.text && !parent.activeFocus
                            text: panel.source === "builtin"
                                  ? qsTr("Search built-in voices")
                                  : (voices.count > 0 ? qsTr("Search %Ln voices", "", voices.count) : qsTr("Search voices"))
                            color: panel.look.muted
                            font.pixelSize: 14
                        }
                    }
                    AbstractButton {
                        id: clearSearchBtn
                        anchors.right: parent.right
                        anchors.rightMargin: 6
                        anchors.verticalCenter: parent.verticalCenter
                        visible: panel.query !== ""
                        implicitWidth: 24
                        implicitHeight: 24
                        focusPolicy: Qt.NoFocus
                        contentItem: Glyph {
                            anchors.centerIn: parent
                            width: 12
                            height: 12
                            path: Icons.close
                            color: panel.look.muted
                        }
                        background: Rectangle {
                            radius: 12
                            color: clearSearchBtn.hovered ? Ui.tint(panel.look.fg, 0.1) : "transparent"
                        }
                        onClicked: panel.updateQuery("")
                    }
                }

                Note {
                    visible: panel.source === "gemini" && (voices.loading || voices.problem !== "")
                    text: voices.loading ? qsTr("Fetching the voices…") : voices.problem
                    color: voices.problem !== "" ? (theme.colors.orange || "#e5a050") : panel.look.muted
                }
            }
        }

        footer: Item {
            width: voiceList.width
            implicitHeight: footerContent.implicitHeight + 28
            height: implicitHeight

            ColumnLayout {
                id: footerContent
                x: 22
                y: 8
                width: parent.width - 44
                spacing: 8

                Note {
                    visible: panel.listItems.length === 0 && !(panel.source === "gemini" && voices.loading)
                    text: panel.source === "studio"
                          ? (voices.problem === "" ? qsTr("The server lists no voices. It may need a moment, or a voice name set up on its side.") : "")
                          : panel.debouncedQuery.trim() !== ""
                            ? qsTr("No voice matches.")
                            : panel.source === "builtin"
                              ? qsTr("No built-in voice matches this filter.")
                              : ""
                }

                Action {
                    visible: panel.hiddenBuiltinCount > 0 || (panel.source === "builtin" && panel.builtinFilter === "__book__" && panel.debouncedQuery.trim() === "" && panel.allBuiltins && panel.builtinInstalledForBook.length > 0)
                    text: panel.allBuiltins ? qsTr("Fewer")
                        : qsTr("More built-in voices (%1)").arg(panel.hiddenBuiltinCount)
                    onClicked: panel.allBuiltins = !panel.allBuiltins
                }

                // Installed built-in voice packs management & removal.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 10
                    visible: voicePacks.supported && panel.source === "builtin" && panel.installedPacks.length > 0
                    radius: Math.min(10, theme.radius)
                    color: Ui.tint(panel.look.fg, 0.04)
                    border.color: panel.look.line
                    implicitHeight: packsCol.implicitHeight + 22
                    ColumnLayout {
                        id: packsCol
                        x: 12
                        y: 11
                        width: parent.width - 24
                        spacing: 6
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Installed voice packs · %1").arg(Format.size(voicePacks.megabytes * 1024 * 1024, Qt.locale()))
                            color: panel.look.muted
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                        }
                        Repeater {
                            model: panel.installedPacks
                            delegate: PackRow {
                                required property var modelData
                                pack: modelData
                            }
                        }
                    }
                }

                // Audio cache size + Clear audio cache.
                Rectangle {
                    id: cacheCard
                    property bool confirmingClear: false
                    Timer {
                        running: cacheCard.confirmingClear
                        interval: 4000
                        onTriggered: cacheCard.confirmingClear = false
                    }
                    Layout.fillWidth: true
                    Layout.topMargin: 10
                    radius: Math.min(10, theme.radius)
                    color: Ui.tint(panel.look.fg, 0.04)
                    border.color: panel.look.line
                    implicitHeight: cacheRow.implicitHeight + 20
                    RowLayout {
                        id: cacheRow
                        x: 12
                        y: 10
                        width: parent.width - 24
                        spacing: 8
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 2
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Audio cache")
                                color: panel.look.fg
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                            Label {
                                Layout.fillWidth: true
                                text: audioCache.megabytes > 0.001
                                      ? qsTr("%1 kept on disk").arg(Format.size(audioCache.megabytes * 1024 * 1024, Qt.locale()))
                                      : qsTr("Empty")
                                color: panel.look.muted
                                font.pixelSize: 12
                            }
                        }
                        Action {
                            enabled: audioCache.megabytes > 0.001
                            danger: cacheCard.confirmingClear
                            text: cacheCard.confirmingClear ? qsTr("Clear cache?") : qsTr("Clear audio cache")
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
        }
    }
}
