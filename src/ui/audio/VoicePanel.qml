import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui

// Choose a narrator: centered 680×620 app-chrome modal sheet over a dimmed page
// scrim. Focused strictly on browsing, previewing, downloading, and choosing
// narration voices (On this computer / Gemini), with keyboard navigation
// (Up/Down, Space to preview, Enter to choose, Esc to close) and a subtle
// single-line mention of Studio mode in Settings › Voices.
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
    signal listenRequested()

    // Which voices the sheet shows: "builtin" or "gemini".
    property string source: "builtin"
    // Built-in language filter: "__book__" (book's language), "" (all),
    // "__installed__" (installed only), or a BCP-47 base code ("en", "de"...).
    property string builtinFilter: "__book__"
    property bool allBuiltins: false
    property string pendingVoice: ""
    property string pendingPack: ""
    property int packsVersion: 0
    property bool keyboardActive: false

    // Deterministic 6-color editorial avatar palette matching 05_narrator.html.
    readonly property var avatarPalette: ["#3E5F99", "#7A4E2D", "#2F6B57", "#6B4A82", "#8A3F4A", "#4A5A6B"]
    function avatarColor(name) {
        return avatarPalette[Ui.hash(String(name || "")) % avatarPalette.length]
    }

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

    // Reset palette to app chrome (independent of Paper/Sepia reading theme).
    palette.window: theme.raised
    palette.windowText: theme.ink1
    palette.base: theme.fill
    palette.alternateBase: theme.fillHover
    palette.text: theme.ink1
    palette.placeholderText: theme.ink4
    palette.button: theme.fill
    palette.buttonText: theme.ink1
    palette.mid: theme.ink3
    palette.midlight: theme.hairline
    palette.dark: theme.separator
    palette.light: theme.overlay
    palette.shadow: theme.shadow
    palette.highlight: theme.accent
    palette.highlightedText: theme.accentOn
    palette.link: theme.accentInk
    palette.linkVisited: theme.accentSoft
    palette.toolTipBase: theme.overlay
    palette.toolTipText: theme.ink1

    readonly property bool builtinVoice: (narration.voice || "").startsWith("builtin:")
    readonly property bool studioVoice: (narration.voice || "").startsWith("studio:")
    readonly property bool styleDisabled: source === "builtin" || builtinVoice
    readonly property bool typingInDesignField: {
        const item = panel.Window.activeFocusItem
        return item !== null && item !== searchInput && item.cursorPosition !== undefined && !item.readOnly
    }
    readonly property string activeProblem: {
        if (source === "builtin" && voicePacks.supported)
            return voicePacks.problem
        if (source === "gemini")
            return voices.problem
        return ""
    }

    Timer {
        id: searchDebounce
        interval: 160
        onTriggered: {
            panel.debouncedQuery = panel.query
            panel.resetListPosition()
        }
    }

    function updateQuery(text) {
        query = text
        if (text.trim() === "") {
            searchDebounce.stop()
            debouncedQuery = ""
            resetListPosition()
        } else {
            searchDebounce.restart()
        }
    }

    function resetListPosition() {
        voiceList.positionViewAtBeginning()
        if (listItems.length === 0) {
            voiceList.currentIndex = -1
            return
        }
        const activeIdx = listItems.findIndex(item => !item.designEntry && item.voice && item.voice.id === narration.voice)
        voiceList.currentIndex = activeIdx >= 0 ? activeIdx : 0
    }

    function switchSource(nextSource) {
        if (source === nextSource)
            return
        voices.stopPreview()
        source = nextSource
        keyboardActive = false
        if (nextSource === "gemini" && ai.hasKey)
            voices.refresh()
        resetListPosition()
    }

    function moveHighlight(delta) {
        const count = listItems.length
        if (count <= 0)
            return
        keyboardActive = true
        const cur = voiceList.currentIndex
        const next = cur < 0 ? (delta > 0 ? 0 : count - 1)
                             : Math.max(0, Math.min(count - 1, cur + delta))
        voiceList.currentIndex = next
        voiceList.positionViewAtIndex(next, ListView.Contain)
    }

    function activateHighlighted() {
        const idx = voiceList.currentIndex
        if (idx < 0 || idx >= listItems.length)
            return
        const entry = listItems[idx]
        if (entry.designEntry) {
            if (voices.casting === "")
                voices.cast(reader.passageFrom(reader.chapter, 4000))
            return
        }
        if (!entry.voice)
            return
        // Pressing Enter on the already-chosen playable voice starts listening right away.
        if (narration.voice === entry.voice.id && (entry.voice.kind !== "builtin" || entry.voice.installed)
            && !(entry.voice.kind !== "builtin" && !ai.hasKey)) {
            startOrFinishListening()
            return
        }
        choose(entry.voice)
    }

    function previewHighlighted() {
        const idx = voiceList.currentIndex
        if (idx < 0 || idx >= listItems.length)
            return
        const entry = listItems[idx]
        if (entry.designEntry || !entry.voice)
            return
        if (entry.voice.kind === "builtin" && !entry.voice.installed)
            return
        if (entry.voice.kind !== "builtin" && !ai.hasKey)
            return
        preview(entry.voice.id)
    }

    function startOrFinishListening() {
        panel.resumeAfter = false
        voices.stopPreview()
        const shouldResume = narration.active && !narration.playing
        const shouldStartPending = !narration.active && narration.hasPendingStart
        const shouldListen = !narration.active && !narration.hasPendingStart
        panel.close()
        if (shouldResume)
            narration.resume()
        else if (shouldStartPending)
            narration.startPending()
        else if (shouldListen)
            panel.listenRequested()
    }

    function openVoicesSettings() {
        panel.close()
        if (typeof settingsView !== "undefined" && typeof settingsView.show === "function")
            settingsView.show("listening")
        else if (typeof win !== "undefined" && typeof win.showAiSettings === "function")
            win.showAiSettings()
    }

    function show() {
        open = true
        keyboardActive = false
        searchDebounce.stop()
        query = ""
        debouncedQuery = ""
        builtinFilter = reader.language !== "" ? "__book__" : ""
        source = !voicePacks.supported ? "gemini"
               : builtinVoice || !ai.hasKey ? "builtin" : "gemini"
        if (ai.hasKey)
            voices.refresh()
        resetListPosition()
        sampleWanted()
        forceActiveFocus()
    }

    function close() {
        if (!open)
            return
        open = false
        keyboardActive = false
        voices.stopPreview()
        if (resumeAfter) {
            resumeAfter = false
            narration.resume()
        }
        closed()
    }

    Component.onDestruction: voices.stopPreview()

    function preview(id) {
        if (!id)
            return
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
        if (!voice || !voice.id)
            return
        voices.stopPreview()
        if (voice.kind === "builtin" && !voice.installed) {
            // Never overwrite an active download's target voice.
            if (voicePacks.busy)
                return
            pendingVoice = voice.id
            pendingPack = voice.pack || ""
            voicePacks.installVoice(voice.id)
            return
        }
        if (voice.kind !== "builtin" && !ai.hasKey)
            return
        narration.setVoice(voice.id, voice.title || voice.name)
    }

    function voiceAccentTag(voice) {
        const title = String(voice.title || voice.name || "")
        const accent = String(voice.accent || "")
        if (accent !== "" && accent.toLowerCase() !== title.toLowerCase())
            return accent
        if (voice.gender)
            return voice.gender === "female" ? qsTr("Female")
                 : voice.gender === "male" ? qsTr("Male") : voice.gender
        return ""
    }

    function voiceSubtitle(voice) {
        const parts = []
        if (voice.kind === "builtin") {
            parts.push(voice.engine || qsTr("Built-in"))
            if (voice.note)
                parts.push(voice.note)
            if (!voice.installed) {
                const mb = Math.max(1, Math.round((voice.bytes || 0) / 1e6))
                parts.push(qsTr("%1 MB download").arg(mb.toLocaleString(Qt.locale(), 'f', 0)))
            } else {
                const speed = localSpeech.speedOf(voice.id)
                if (speed > 0 && translator.language !== "")
                    parts.push(qsTr("%1× real time").arg(speed.toLocaleString(Qt.locale(), 'f', 1)))
                else
                    parts.push(qsTr("Ready offline"))
            }
            return parts.join(" · ")
        }
        if (voice.kind === "designed") {
            parts.push(qsTr("Designed"))
            if (voice.expires && !isNaN(voice.expires.getTime()) && translator.language !== "")
                parts.push(qsTr("kept until %1").arg(voice.expires.toLocaleDateString(Qt.locale(), "MMM yyyy")))
            if (voice.summary)
                parts.push(voice.summary)
            return parts.join(" · ")
        }
        // Gemini library / classic voice
        parts.push("Gemini")
        if (voice.note)
            parts.push(voice.note)
        else if (voice.summary)
            parts.push(voice.summary)
        return parts.join(" · ")
    }

    readonly property string language: reader.language
    readonly property var suggestions: open && voices.count >= 0 ? voices.suggestions(language) : []
    readonly property var narrators: suggestions.filter(voice => voice.kind === "library")
    readonly property var classics: suggestions.filter(voice => voice.kind === "classic")
    readonly property var yours: open && voices.count >= 0 ? voices.designedVoices() : []
    readonly property var geminiResults: open && debouncedQuery.trim() !== "" && voices.count >= 0
        ? voices.search(debouncedQuery, "", 120) : []

    // Built-in voices & language filter options.
    readonly property var builtinsForBook: open && packsVersion >= 0 ? voicePacks.voices(language) : []
    readonly property var builtinInstalledForBook: builtinsForBook.filter(voice => voice.installed)
    readonly property var builtinLanguageOptions: {
        void(packsVersion)
        const opts = []
        if (panel.language !== "") {
            const base = panel.language.split(/[-_]/)[0]
            const bookLangName = Qt.locale(base).nativeLanguageName || Qt.locale(panel.language).nativeLanguageName || panel.language
            opts.push({ code: "__book__", label: qsTr("Book (%1)").arg(bookLangName) })
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
        const q = debouncedQuery.trim()
        if (source === "builtin" && voicePacks.supported) {
            const installedOnly = builtinFilter === "__installed__"
            const langCode = builtinFilter === "__book__" ? language
                           : builtinFilter === "__installed__" ? "" : builtinFilter
            let list = voicePacks.searchVoices(q, langCode, installedOnly)
            if (q === "" && builtinFilter === "__book__" && !allBuiltins && builtinInstalledForBook.length > 0) {
                const compact = builtinInstalledForBook.filter(v => v.recommended || v.id === narration.voice)
                const uninstalled = list.filter(v => !v.installed).slice(0, 2)
                list = compact.concat(uninstalled)
            }
            let badgeCount = 0
            for (let i = 0; i < list.length; ++i) {
                const v = list[i]
                const showBadge = q === "" && !!v.suggested && badgeCount < 2
                if (showBadge)
                    badgeCount++
                items.push({ designEntry: false, voice: v, badgeSuggested: showBadge })
            }
            return items
        }
        // Gemini tab
        if (q !== "") {
            for (let i = 0; i < geminiResults.length; ++i)
                items.push({ designEntry: false, voice: geminiResults[i], badgeSuggested: false })
            return items
        }
        for (let i = 0; i < yours.length; ++i)
            items.push({ designEntry: false, voice: yours[i], badgeSuggested: false })
        let badgeCount = 0
        for (let i = 0; i < narrators.length; ++i) {
            const v = narrators[i]
            const showBadge = !!v.suggested && badgeCount < 2
            if (showBadge)
                badgeCount++
            items.push({ designEntry: false, voice: v, badgeSuggested: showBadge })
        }
        for (let i = 0; i < classics.length; ++i) {
            const v = classics[i]
            const showBadge = !!v.suggested && badgeCount < 2
            if (showBadge)
                badgeCount++
            items.push({ designEntry: false, voice: v, badgeSuggested: showBadge })
        }
        if (ai.canDesignVoices)
            items.push({ designEntry: true, voice: { id: "__design__" }, badgeSuggested: false })
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
        function onFailed() {
            panel.pendingVoice = ""
            panel.pendingPack = ""
            panel.packsVersion++
        }
        function onProgressChanged() {
            if (!voicePacks.busy && panel.pendingVoice !== "") {
                const v = voicePacks.voice(panel.pendingVoice)
                if (!v || !v.installed) {
                    panel.pendingVoice = ""
                    panel.pendingPack = ""
                }
            }
        }
        function onReady(voiceId) {
            panel.packsVersion++
            if (voiceId !== panel.pendingVoice)
                return
            panel.pendingVoice = ""
            panel.pendingPack = ""
            const voice = voicePacks.voice(voiceId)
            if (voice && voice.id)
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

    Keys.onEscapePressed: (event) => {
        if (searchInput.activeFocus && panel.query !== "") {
            panel.updateQuery("")
            event.accepted = true
            return
        }
        panel.close()
        event.accepted = true
    }
    Keys.onDownPressed: (event) => {
        if (panel.typingInDesignField)
            return
        panel.moveHighlight(1)
        event.accepted = true
    }
    Keys.onUpPressed: (event) => {
        if (panel.typingInDesignField)
            return
        panel.moveHighlight(-1)
        event.accepted = true
    }
    Keys.onReturnPressed: (event) => {
        if (panel.typingInDesignField)
            return
        panel.activateHighlighted()
        event.accepted = true
    }
    Keys.onEnterPressed: (event) => {
        if (panel.typingInDesignField)
            return
        panel.activateHighlighted()
        event.accepted = true
    }
    Keys.onPressed: (event) => {
        if (panel.typingInDesignField)
            return
        if (event.key === Qt.Key_Space && !searchInput.activeFocus) {
            panel.previewHighlighted()
            event.accepted = true
            return
        }
        if ((event.key === Qt.Key_F && (event.modifiers & Qt.ControlModifier))
            || (event.key === Qt.Key_Slash && event.modifiers === Qt.NoModifier && !searchInput.activeFocus)) {
            searchInput.forceActiveFocus()
            searchInput.selectAll()
            event.accepted = true
        }
    }

    // Fill the reader window so scrim & centered modal always align.
    x: 0
    y: 0
    z: 10
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    visible: opacity > 0.01
    opacity: open ? 1 : 0
    Behavior on opacity {
        NumberAnimation {
            duration: theme.reducedMotion ? 100 : (panel.open ? 180 : 130)
            easing.type: Easing.OutCubic
        }
    }

    // Dimmed modal scrim over the page.
    Rectangle {
        anchors.fill: parent
        color: theme.light ? Qt.rgba(24 / 255, 20 / 255, 14 / 255, 0.34) : Qt.rgba(24 / 255, 20 / 255, 14 / 255, 0.42)
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onClicked: panel.close()
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    // Plays a voice reading the page; a second press stops it.
    component ListenBtn: AbstractButton {
        id: listen
        property string voiceId
        property var press: null
        property string disabledTip: ""
        readonly property bool playing: voiceId !== "" && voices.previewing === voiceId
        implicitWidth: 30
        implicitHeight: 30
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        opacity: enabled ? 1.0 : 0.38
        Accessible.name: playing ? qsTr("Stop preview") : qsTr("Preview")
        background: Rectangle {
            radius: 15
            color: listen.playing ? theme.accentSoft
                 : listen.enabled && listen.pressed ? theme.fillPressed
                 : listen.enabled && listen.hovered ? theme.fillHover : theme.fill
            border.width: listen.playing ? 1 : 0
            border.color: Ui.tint(theme.accent, 0.48)
            Behavior on color { ColorAnimation { duration: 100 } }
            Rectangle {
                anchors.fill: parent
                anchors.margins: -2
                visible: listen.visualFocus
                radius: 17
                color: "transparent"
                border.width: 2
                border.color: theme.accent
            }
        }
        contentItem: Item {
            Glyph {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: listen.playing ? 0 : 1
                width: 12
                height: 12
                visible: !(listen.playing && voices.previewBuffering)
                path: listen.playing ? Icons.pauseSolid : Icons.playSolid
                filled: true
                color: listen.playing ? theme.accentInk : theme.ink1
            }
            BusyIndicator {
                anchors.centerIn: parent
                implicitWidth: 20
                implicitHeight: 20
                visible: listen.playing && voices.previewBuffering
                running: visible
            }
        }
        onClicked: press ? press() : panel.preview(voiceId)
        HoverHandler { cursorShape: listen.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
        ToolTip.visible: hovered
        ToolTip.text: !enabled && disabledTip !== "" ? disabledTip
                    : playing ? qsTr("Stop preview") : qsTr("Hear it read this page")
        ToolTip.delay: 500
    }

    component VoiceRow: AbstractButton {
        id: row
        property var voice: ({})
        property bool badgeSuggested: false
        property bool keyboardHighlighted: false
        readonly property bool chosen: narration.voice === voice.id
        readonly property bool previewingThis: voice.id !== undefined && voices.previewing === voice.id
        readonly property bool needsDownload: voice.kind === "builtin" && !voice.installed
        readonly property bool downloadingThis: voicePacks.busy && needsDownload && panel.pendingVoice === voice.id
        readonly property bool needsKey: voice.kind !== "builtin" && !ai.hasKey
        readonly property string accentTag: panel.voiceAccentTag(voice)
        property bool confirming: false

        Timer {
            running: row.confirming
            interval: 4000
            onTriggered: row.confirming = false
        }

        implicitHeight: Math.max(52, rowLayout.implicitHeight + 14)
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.name: (voice.title || voice.name || "") + (accentTag !== "" ? ", " + accentTag : "")
        Accessible.checked: chosen
        onClicked: panel.choose(voice)
        onHoveredChanged: if (hovered) panel.keyboardActive = false

        background: Rectangle {
            radius: Ui.radius(10, theme.radius)
            color: row.chosen ? Ui.tint(theme.ink1, 0.08)
                 : row.previewingThis ? Ui.tint(theme.accent, 0.08)
                 : row.keyboardHighlighted ? Ui.tint(theme.ink1, 0.06)
                 : row.pressed ? Ui.tint(theme.ink1, 0.06)
                 : row.hovered ? Ui.tint(theme.ink1, 0.04) : "transparent"
            border.width: row.previewingThis ? 1 : row.keyboardHighlighted ? 1.5 : 0
            border.color: row.previewingThis ? Ui.tint(theme.accent, 0.35) : Ui.tint(theme.accent, 0.65)
            Behavior on color { ColorAnimation { duration: 100 } }
            Rectangle {
                anchors.fill: parent
                anchors.margins: -2
                visible: row.visualFocus
                radius: Ui.radius(12, theme.radius)
                color: "transparent"
                border.width: 2
                border.color: theme.accent
            }
        }

        contentItem: RowLayout {
            id: rowLayout
            spacing: 12

            // 34×34 circular initial avatar.
            Rectangle {
                Layout.leftMargin: 10
                Layout.preferredWidth: 34
                Layout.preferredHeight: 34
                Layout.alignment: Qt.AlignVCenter
                radius: 17
                color: panel.avatarColor(row.voice.title || row.voice.name || row.voice.id)
                Label {
                    anchors.centerIn: parent
                    text: String(row.voice.title || row.voice.name || "?").charAt(0).toUpperCase()
                    color: "#F2F2F3"
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }
            }

            // 2-line metadata stack.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                Layout.alignment: Qt.AlignVCenter
                spacing: 2

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8

                    Label {
                        text: row.voice.title || row.voice.name || ""
                        color: theme.ink1
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                        Layout.maximumWidth: 200
                    }
                    Label {
                        visible: row.accentTag !== ""
                        text: row.accentTag
                        color: theme.ink3
                        font.pixelSize: 13
                        elide: Text.ElideRight
                        Layout.maximumWidth: 160
                    }
                    Rectangle {
                        visible: row.badgeSuggested
                        Layout.alignment: Qt.AlignVCenter
                        implicitWidth: suitsLabel.implicitWidth + 12
                        implicitHeight: 17
                        radius: 9
                        color: theme.accentSoft
                        Label {
                            id: suitsLabel
                            anchors.centerIn: parent
                            text: qsTr("Suits this book")
                            color: theme.light ? theme.accentInk : "#D6E2FB"
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                        }
                    }
                    Item { Layout.fillWidth: true }
                }

                Label {
                    Layout.fillWidth: true
                    text: panel.voiceSubtitle(row.voice)
                    color: theme.ink4
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
            }

            // Delete button for user-designed voices (4s confirmation).
            AbstractButton {
                id: forgetButton
                visible: row.voice.kind === "designed" && (row.hovered || row.confirming)
                Layout.alignment: Qt.AlignVCenter
                implicitHeight: 28
                implicitWidth: row.confirming ? confirmLabel.implicitWidth + 16 : 28
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                background: Rectangle {
                    radius: Ui.radius(6, theme.radius)
                    color: row.confirming ? Ui.tint(theme.danger, 0.2)
                         : forgetButton.hovered ? theme.fillHover : "transparent"
                }
                contentItem: Item {
                    Glyph {
                        anchors.centerIn: parent
                        visible: !row.confirming
                        width: 15
                        height: 15
                        path: Icons.trash
                        color: theme.ink3
                    }
                    Label {
                        id: confirmLabel
                        anchors.centerIn: parent
                        visible: row.confirming
                        text: qsTr("Delete")
                        color: theme.danger
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

            // 30×30 action slot: preview play/stop, active download spinner, or download trigger.
            ListenBtn {
                visible: !row.needsDownload
                Layout.alignment: Qt.AlignVCenter
                voiceId: row.voice.id || ""
                enabled: !row.needsKey
                disabledTip: qsTr("Add your Gemini API key to preview")
            }
            Item {
                visible: row.downloadingThis
                Layout.alignment: Qt.AlignVCenter
                Layout.preferredWidth: 30
                Layout.preferredHeight: 30
                BusyIndicator {
                    anchors.centerIn: parent
                    implicitWidth: 22
                    implicitHeight: 22
                    running: parent.visible
                }
            }
            AbstractButton {
                id: dlRowBtn
                visible: row.needsDownload && !row.downloadingThis
                Layout.alignment: Qt.AlignVCenter
                implicitWidth: 30
                implicitHeight: 30
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                enabled: !voicePacks.busy
                opacity: enabled ? 1.0 : 0.38
                Accessible.name: qsTr("Download this voice")
                onClicked: panel.choose(row.voice)
                background: Rectangle {
                    radius: 15
                    color: dlRowBtn.enabled && dlRowBtn.pressed ? theme.fillPressed
                         : dlRowBtn.enabled && dlRowBtn.hovered ? theme.fillHover : theme.fill
                }
                contentItem: Item {
                    Glyph {
                        anchors.centerIn: parent
                        width: 14
                        height: 14
                        path: Icons.download
                        color: theme.ink3
                    }
                }
                HoverHandler { cursorShape: dlRowBtn.enabled ? Qt.PointingHandCursor : Qt.ArrowCursor }
                ToolTip.visible: hovered
                ToolTip.text: voicePacks.busy ? qsTr("Wait for the current download to finish") : qsTr("Download this voice")
                ToolTip.delay: 500
            }

            // 18 px checkmark slot so action buttons align cleanly across every row.
            Item {
                Layout.preferredWidth: 18
                Layout.preferredHeight: 18
                Layout.rightMargin: 10
                Layout.alignment: Qt.AlignVCenter
                Glyph {
                    anchors.centerIn: parent
                    visible: row.chosen
                    width: 16
                    height: 16
                    stroke: 2.6
                    path: Icons.check
                    color: theme.accent
                }
            }
        }
    }

    // "+ Design a narrator…" entry that expands into the editable AI Casting card (Gemini tab).
    component DesignNarratorEntry: Item {
        id: entry
        property bool keyboardHighlighted: false
        implicitHeight: voices.casting === "" ? designRow.implicitHeight : castingCard.implicitHeight + 8

        AbstractButton {
            id: designRow
            visible: voices.casting === ""
            anchors.left: parent.left
            anchors.right: parent.right
            implicitHeight: 52
            hoverEnabled: true
            focusPolicy: Qt.TabFocus
            onClicked: voices.cast(reader.passageFrom(reader.chapter, 4000))
            background: Rectangle {
                radius: Ui.radius(10, theme.radius)
                color: entry.keyboardHighlighted ? Ui.tint(theme.ink1, 0.06)
                     : designRow.pressed ? Ui.tint(theme.ink1, 0.06)
                     : designRow.hovered ? Ui.tint(theme.ink1, 0.04) : "transparent"
                border.width: entry.keyboardHighlighted ? 1.5 : 0
                border.color: Ui.tint(theme.accent, 0.65)
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -2
                    visible: designRow.visualFocus
                    radius: Ui.radius(12, theme.radius)
                    color: "transparent"
                    border.width: 2
                    border.color: theme.accent
                }
            }
            contentItem: RowLayout {
                spacing: 12
                Rectangle {
                    Layout.leftMargin: 10
                    Layout.preferredWidth: 34
                    Layout.preferredHeight: 34
                    Layout.alignment: Qt.AlignVCenter
                    radius: 17
                    color: "#4A5A6B"
                    Label {
                        anchors.centerIn: parent
                        text: "+"
                        color: "#F2F2F3"
                        font.pixelSize: 16
                        font.weight: Font.DemiBold
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 2
                    Label {
                        text: qsTr("Design a narrator…")
                        color: theme.ink1
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                    }
                    Label {
                        text: qsTr("Describe a voice for this book · about 3¢")
                        color: theme.ink4
                        font.pixelSize: 12
                    }
                }
                Item { Layout.preferredWidth: 58; Layout.rightMargin: 10 }
            }
        }

        Rectangle {
            id: castingCard
            visible: voices.casting !== ""
            anchors.left: parent.left
            anchors.right: parent.right
            y: 4
            radius: Ui.radius(10, theme.radius)
            color: theme.accentSoft
            border.width: 1
            border.color: Ui.tint(theme.accent, 0.28)
            implicitHeight: castingCol.implicitHeight + 24

            ColumnLayout {
                id: castingCol
                x: 14
                y: 12
                width: parent.width - 28
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Glyph {
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        path: Icons.sparkle
                        color: theme.accentInk
                    }
                    Label {
                        Layout.fillWidth: true
                        text: voices.casting === "designed" ? (voices.designed.name || "") : qsTr("Design a narrator")
                        color: theme.ink1
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    ListenBtn {
                        visible: voices.casting === "designed"
                        voiceId: voices.designed.id || ""
                        press: () => voices.previewing === voiceId ? voices.stopPreview() : voices.playSample()
                    }
                    IconButton {
                        implicitWidth: 26
                        implicitHeight: 26
                        glyph: Icons.close
                        tone: theme.ink3
                        tip: qsTr("Cancel")
                        onClicked: voices.resetCasting()
                    }
                }

                RowLayout {
                    visible: voices.casting === "thinking" || voices.casting === "designing"
                    spacing: 10
                    BusyIndicator {
                        implicitWidth: 22
                        implicitHeight: 22
                        running: parent.visible
                    }
                    Label {
                        Layout.fillWidth: true
                        text: voices.casting === "thinking" ? qsTr("Reading the book…")
                            : qsTr("Designing “%1”. This takes about half a minute.").arg(panel.draftName)
                        color: theme.ink3
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }

                RowLayout {
                    visible: voices.casting === "proposed"
                    Layout.fillWidth: true
                    spacing: 8
                    TextField {
                        Layout.fillWidth: true
                        text: panel.draftName
                        placeholderText: qsTr("Name")
                        font.pixelSize: 13
                        onTextEdited: panel.draftName = text
                    }
                    Segmented {
                        compact: true
                        model: [
                            { value: "female", label: qsTr("Female") },
                            { value: "male", label: qsTr("Male") }
                        ]
                        current: panel.draftGender
                        onActivated: (v) => panel.draftGender = v
                    }
                }

                TextArea {
                    Layout.fillWidth: true
                    visible: voices.casting === "proposed"
                    text: panel.draftDescription
                    placeholderText: qsTr("How the voice sounds")
                    wrapMode: TextArea.Wrap
                    font.pixelSize: 13
                    onTextChanged: if (activeFocus) panel.draftDescription = text
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: voices.casting === "proposed"
                    spacing: 8
                    ComboBox {
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
                        font.pixelSize: 13
                        onTextEdited: panel.draftLanguage = text
                    }
                }

                RowLayout {
                    visible: voices.casting === "proposed"
                    spacing: 8
                    TextButton {
                        primary: true
                        text: qsTr("Design this voice")
                        enabled: panel.draftDescription.trim() !== ""
                        onClicked: voices.design({ name: panel.draftName, description: panel.draftDescription,
                                                   gender: panel.draftGender, language: panel.draftLanguage.trim() })
                    }
                    TextButton {
                        text: qsTr("Suggest another")
                        onClicked: voices.cast(reader.passageFrom(reader.chapter, 4000))
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: voices.casting === "designed"
                    text: voices.designed.summary || ""
                    color: theme.ink3
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                }
                RowLayout {
                    visible: voices.casting === "designed"
                    spacing: 8
                    TextButton {
                        primary: true
                        text: narration.voice === voices.designed.id ? qsTr("Reading the book") : qsTr("Use this voice")
                        enabled: narration.voice !== voices.designed.id
                        onClicked: panel.choose(voices.designed)
                    }
                    TextButton {
                        text: qsTr("Try again")
                        onClicked: voices.design({ name: panel.draftName, description: panel.draftDescription,
                                                   gender: panel.draftGender, language: panel.draftLanguage.trim() })
                    }
                    TextButton {
                        text: qsTr("Change it")
                        onClicked: voices.revise()
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: voices.casting === "failed"
                    text: voices.castingProblem
                    color: theme.cloudInk
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
                TextButton {
                    visible: voices.casting === "failed"
                    text: qsTr("Try again")
                    onClicked: voices.cast(reader.passageFrom(reader.chapter, 4000))
                }
            }
        }
    }

    // --- Centered 680×620 App-Chrome Modal Sheet ---
    Rectangle {
        id: sheetCard
        anchors.centerIn: parent
        width: Math.min(680, Math.max(380, panel.width - 48))
        height: Math.min(620, Math.max(420, panel.height - 64))
        radius: Ui.radius(16, theme.radius)
        color: theme.raised
        border.width: 1
        border.color: theme.hairline
        scale: panel.open || theme.reducedMotion ? 1.0 : 0.97
        transform: Translate {
            y: panel.open || theme.reducedMotion ? 0 : 8
            Behavior on y {
                enabled: !theme.reducedMotion
                NumberAnimation { duration: 210; easing.type: Easing.OutCubic }
            }
        }
        Behavior on scale {
            enabled: !theme.reducedMotion
            NumberAnimation { duration: 210; easing.type: Easing.OutCubic }
        }

        // Soft layered modal shadow (works identically in rounded and sharp modes).
        Repeater {
            model: [
                { spread: 2, yOff: 4, alpha: 0.30 },
                { spread: 6, yOff: 9, alpha: 0.18 },
                { spread: 12, yOff: 15, alpha: 0.09 }
            ]
            Rectangle {
                required property var modelData
                z: -1
                x: -modelData.spread
                y: modelData.yOff
                width: sheetCard.width + modelData.spread * 2
                height: sheetCard.height + modelData.spread
                radius: sheetCard.radius > 0 ? sheetCard.radius + modelData.spread : 0
                color: theme.shadow
                opacity: modelData.alpha
            }
        }

        // Keep clicks/wheels inside the modal card.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: (wheel) => wheel.accepted = true
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            // === 1. Fixed Header ===
            Item {
                Layout.fillWidth: true
                implicitHeight: headerCol.implicitHeight + 34

                ColumnLayout {
                    id: headerCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: 18
                    anchors.leftMargin: 22
                    anchors.rightMargin: 22
                    spacing: 14

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 3
                            Label {
                                text: qsTr("Narrator")
                                color: theme.ink1
                                font.pixelSize: 17
                                font.weight: Font.DemiBold
                                font.letterSpacing: -0.17
                            }
                            Label {
                                Layout.fillWidth: true
                                text: reader.title !== ""
                                      ? qsTr("For %1. Previews read the page you're on.").arg(reader.title)
                                      : qsTr("Previews read the page you're on.")
                                color: theme.ink3
                                font.pixelSize: 13
                                elide: Text.ElideRight
                            }
                        }

                        AbstractButton {
                            id: closeBtn
                            Layout.alignment: Qt.AlignTop
                            implicitWidth: 28
                            implicitHeight: 28
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            Accessible.name: qsTr("Close")
                            onClicked: panel.close()
                            background: Rectangle {
                                radius: Ui.radius(7, theme.radius)
                                color: closeBtn.pressed ? theme.fillPressed
                                     : closeBtn.hovered ? theme.fillHover : theme.fill
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -2
                                    visible: closeBtn.visualFocus
                                    radius: Ui.radius(9, theme.radius)
                                    color: "transparent"
                                    border.width: 2
                                    border.color: theme.accent
                                }
                            }
                            contentItem: Item {
                                Glyph {
                                    anchors.centerIn: parent
                                    width: 13
                                    height: 13
                                    stroke: 2.2
                                    path: Icons.close
                                    color: theme.ink2
                                }
                            }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Close (Esc)")
                            ToolTip.delay: 600
                        }
                    }

                    // Clean 2-source toggle: On this computer vs Gemini.
                    Segmented {
                        Layout.fillWidth: true
                        model: [
                            {
                                value: "builtin",
                                label: qsTr("On this computer"),
                                subtitle: voicePacks.supported ? qsTr("Free · offline") : qsTr("Unavailable here"),
                                enabled: voicePacks.supported
                            },
                            {
                                value: "gemini",
                                label: qsTr("Gemini"),
                                subtitle: ai.hasKey ? qsTr("Most natural") : qsTr("Needs your key")
                            }
                        ]
                        current: panel.source
                        onActivated: (value) => panel.switchSource(value)
                    }
                }

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    height: 1
                    color: Ui.tint(theme.ink1, 0.06)
                }
            }

            // === 2. Filter Bar + Persistent Download/Error Banners + Virtualized Voice List ===
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    // Compact section title + inline language filter & search.
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 22
                        Layout.rightMargin: 18
                        Layout.topMargin: 12
                        Layout.bottomMargin: 8
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            text: panel.source === "builtin"
                                  ? qsTr("Made on this computer: instant, free, private")
                                  : qsTr("Gemini voices, with your key")
                            color: theme.ink4
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.2
                            elide: Text.ElideRight
                        }

                        ComboBox {
                            visible: voicePacks.supported && panel.source === "builtin"
                            Layout.preferredWidth: 192
                            implicitHeight: 28
                            font.pixelSize: 12
                            model: panel.builtinLanguageOptions.map(o => o.label)
                            currentIndex: {
                                const idx = panel.builtinLanguageOptions.findIndex(o => o.code === panel.builtinFilter)
                                return idx >= 0 ? idx : 0
                            }
                            onActivated: (index) => {
                                if (index >= 0 && index < panel.builtinLanguageOptions.length) {
                                    panel.builtinFilter = panel.builtinLanguageOptions[index].code
                                    panel.allBuiltins = true
                                    panel.resetListPosition()
                                }
                            }
                        }

                        Rectangle {
                            visible: panel.source === "builtin" || ai.hasKey
                            Layout.preferredWidth: 164
                            implicitHeight: 28
                            radius: Ui.radius(7, theme.radius)
                            color: searchInput.activeFocus ? theme.fillHover : theme.fill
                            border.width: searchInput.activeFocus ? 1.5 : 0
                            border.color: theme.accent

                            Glyph {
                                id: searchIcon
                                anchors.left: parent.left
                                anchors.leftMargin: 8
                                anchors.verticalCenter: parent.verticalCenter
                                width: 13
                                height: 13
                                path: Icons.search
                                color: theme.ink4
                            }
                            TextInput {
                                id: searchInput
                                anchors.left: searchIcon.right
                                anchors.right: clearSearchBtn.visible ? clearSearchBtn.left : parent.right
                                anchors.leftMargin: 6
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                color: theme.ink1
                                selectionColor: theme.accentSoft
                                font.pixelSize: 12
                                clip: true
                                text: panel.query
                                onTextEdited: panel.updateQuery(text)
                                Keys.onDownPressed: (event) => { panel.moveHighlight(1); event.accepted = true }
                                Keys.onUpPressed: (event) => { panel.moveHighlight(-1); event.accepted = true }
                                Keys.onReturnPressed: (event) => { panel.activateHighlighted(); event.accepted = true }
                                Keys.onEnterPressed: (event) => { panel.activateHighlighted(); event.accepted = true }
                                Keys.onEscapePressed: (event) => {
                                    if (text !== "") {
                                        panel.updateQuery("")
                                        event.accepted = true
                                    } else {
                                        panel.forceActiveFocus()
                                        event.accepted = false
                                    }
                                }
                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    visible: !parent.text && !parent.activeFocus
                                    text: qsTr("Filter voices…")
                                    color: theme.ink4
                                    font.pixelSize: 12
                                }
                            }
                            AbstractButton {
                                id: clearSearchBtn
                                anchors.right: parent.right
                                anchors.rightMargin: 4
                                anchors.verticalCenter: parent.verticalCenter
                                visible: panel.query !== ""
                                implicitWidth: 20
                                implicitHeight: 20
                                hoverEnabled: true
                                focusPolicy: Qt.NoFocus
                                Accessible.name: qsTr("Clear filter")
                                contentItem: Glyph {
                                    anchors.centerIn: parent
                                    width: 10
                                    height: 10
                                    path: Icons.close
                                    color: theme.ink3
                                }
                                background: Rectangle {
                                    radius: 10
                                    color: clearSearchBtn.hovered ? theme.fillHover : "transparent"
                                }
                                onClicked: panel.updateQuery("")
                            }
                        }
                    }

                    // Persistent built-in download progress card (stays reachable while scrolling or switching tabs).
                    Rectangle {
                        visible: voicePacks.supported && voicePacks.busy
                        Layout.fillWidth: true
                        Layout.leftMargin: 18
                        Layout.rightMargin: 18
                        Layout.bottomMargin: 8
                        radius: Ui.radius(10, theme.radius)
                        color: theme.fill
                        border.width: 1
                        border.color: theme.hairline
                        implicitHeight: dlProgCol.implicitHeight + 18

                        ColumnLayout {
                            id: dlProgCol
                            x: 12
                            y: 9
                            width: parent.width - 24
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
                                Label {
                                    Layout.fillWidth: true
                                    text: voicePacks.progress >= 1.0
                                          ? qsTr("Installing %1…").arg(voicePacks.installing)
                                          : voicePacks.progress > 0
                                            ? qsTr("Downloading %1… %2%")
                                                  .arg(voicePacks.installing)
                                                  .arg(Math.round(Math.min(1, voicePacks.progress) * 100).toLocaleString(Qt.locale(), 'f', 0))
                                            : qsTr("Downloading %1…").arg(voicePacks.installing)
                                    color: theme.ink1
                                    font.pixelSize: 12
                                    font.weight: Font.Medium
                                    font.features: ({ "tnum": 1 })
                                    elide: Text.ElideRight
                                }
                                TextButton {
                                    size: "compact"
                                    text: qsTr("Cancel")
                                    onClicked: {
                                        panel.pendingVoice = ""
                                        panel.pendingPack = ""
                                        voicePacks.cancel()
                                    }
                                }
                            }
                            ProgressBar {
                                Layout.fillWidth: true
                                value: Math.max(0, Math.min(1, voicePacks.progress))
                                indeterminate: voicePacks.progress <= 0 || voicePacks.progress >= 1.0
                            }
                        }
                    }

                    // Persistent inline error banner with retry affordance for Gemini.
                    Rectangle {
                        visible: panel.activeProblem !== ""
                        Layout.fillWidth: true
                        Layout.leftMargin: 18
                        Layout.rightMargin: 18
                        Layout.bottomMargin: 8
                        radius: Ui.radius(10, theme.radius)
                        color: theme.cloudSoft
                        border.width: 1
                        border.color: Ui.tint(theme.cloud, 0.30)
                        implicitHeight: errBannerRow.implicitHeight + 16

                        RowLayout {
                            id: errBannerRow
                            x: 12
                            y: 8
                            width: parent.width - 24
                            spacing: 8

                            Glyph {
                                Layout.alignment: Qt.AlignTop
                                Layout.topMargin: 2
                                Layout.preferredWidth: 14
                                Layout.preferredHeight: 14
                                stroke: 2.0
                                path: Icons.infoCircle
                                color: theme.cloudInk
                            }
                            Label {
                                Layout.fillWidth: true
                                text: panel.activeProblem
                                color: theme.cloudInk
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                            TextButton {
                                visible: panel.source === "gemini" && ai.hasKey && !voices.loading
                                size: "compact"
                                text: qsTr("Retry")
                                onClicked: voices.refresh(true)
                            }
                        }
                    }

                    // Virtualized ListView for 222+ built-in & Gemini voices (zero blank gaps).
                    ListView {
                        id: voiceList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        maximumFlickVelocity: 4500
                        flickDeceleration: 2200
                        pixelAligned: true
                        topMargin: 2
                        bottomMargin: 8
                        spacing: 3
                        cacheBuffer: 360
                        highlightFollowsCurrentItem: false
                        model: panel.listItems
                        ScrollBar.vertical: ScrollBar {}
                        SmoothWheel { stepSize: 110 }

                        header: Item {
                            readonly property bool hasHeader:
                                panel.source === "gemini" && (!ai.hasKey || voices.loading)
                            width: voiceList.width
                            visible: hasHeader
                            implicitHeight: hasHeader ? headerCards.implicitHeight + 10 : 0
                            height: implicitHeight

                            ColumnLayout {
                                id: headerCards
                                x: 14
                                y: 2
                                width: parent.width - 28
                                spacing: 8

                                // Gemini honest missing-key card.
                                Rectangle {
                                    Layout.fillWidth: true
                                    visible: panel.source === "gemini" && !ai.hasKey
                                    radius: Ui.radius(10, theme.radius)
                                    color: theme.fill
                                    border.width: 1
                                    border.color: theme.hairline
                                    implicitHeight: keyCol.implicitHeight + 22
                                    ColumnLayout {
                                        id: keyCol
                                        x: 14
                                        y: 11
                                        width: parent.width - 28
                                        spacing: 8
                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("Gemini's voices need your own API key. It's free to get, and you set the monthly limit.")
                                            color: theme.ink1
                                            font.pixelSize: 13
                                            wrapMode: Text.Wrap
                                        }
                                        RowLayout {
                                            spacing: 8
                                            TextButton {
                                                primary: true
                                                text: qsTr("Add your key")
                                                onClicked: {
                                                    panel.close()
                                                    win.showAiSettings()
                                                }
                                            }
                                            TextButton {
                                                visible: voicePacks.supported
                                                text: qsTr("Use offline voices")
                                                onClicked: panel.switchSource("builtin")
                                            }
                                        }
                                    }
                                }

                                // Gemini loading state with spinner.
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 8
                                    Layout.rightMargin: 8
                                    visible: panel.source === "gemini" && voices.loading
                                    spacing: 8
                                    BusyIndicator {
                                        implicitWidth: 16
                                        implicitHeight: 16
                                        running: parent.visible
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Fetching the voices…")
                                        color: theme.ink3
                                        font.pixelSize: 12
                                    }
                                }
                            }
                        }

                        Component {
                            id: designNarratorComp
                            DesignNarratorEntry {}
                        }

                        delegate: Item {
                            id: delWrap
                            required property var modelData
                            required property int index
                            readonly property bool isDesign: !!modelData.designEntry
                            readonly property bool kbHighlight: panel.keyboardActive && voiceList.currentIndex === index
                            width: voiceList.width
                            height: isDesign ? designLoader.implicitHeight : voiceItem.implicitHeight

                            VoiceRow {
                                id: voiceItem
                                visible: !delWrap.isDesign
                                x: 14
                                width: parent.width - 28
                                voice: delWrap.modelData.voice
                                badgeSuggested: !!delWrap.modelData.badgeSuggested
                                keyboardHighlighted: delWrap.kbHighlight
                            }
                            Loader {
                                id: designLoader
                                active: delWrap.isDesign
                                visible: active
                                x: 14
                                width: parent.width - 28
                                sourceComponent: designNarratorComp
                                Binding {
                                    when: designLoader.status === Loader.Ready
                                    target: designLoader.item
                                    property: "width"
                                    value: designLoader.width
                                }
                                Binding {
                                    when: designLoader.status === Loader.Ready
                                    target: designLoader.item
                                    property: "keyboardHighlighted"
                                    value: delWrap.kbHighlight
                                }
                            }
                        }

                        footer: Item {
                            id: listFooter
                            readonly property bool showEmpty: panel.listItems.length === 0
                                && !(panel.source === "gemini" && (voices.loading || !ai.hasKey))
                            readonly property bool showMoreBtn: panel.hiddenBuiltinCount > 0
                                || (panel.source === "builtin" && panel.builtinFilter === "__book__"
                                    && panel.debouncedQuery.trim() === "" && panel.allBuiltins
                                    && panel.builtinInstalledForBook.length > 0)
                            readonly property bool showEconomy: panel.source === "gemini" && ai.hasKey && panel.debouncedQuery.trim() === ""
                            readonly property bool hasFooter: showEmpty || showMoreBtn || showEconomy
                            width: voiceList.width
                            visible: hasFooter
                            implicitHeight: hasFooter ? footerCol.implicitHeight + 12 : 0
                            height: implicitHeight

                            ColumnLayout {
                                id: footerCol
                                x: 14
                                y: 6
                                width: parent.width - 28
                                spacing: 8

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 8
                                    Layout.rightMargin: 8
                                    visible: listFooter.showEmpty
                                    spacing: 8
                                    Label {
                                        Layout.fillWidth: true
                                        text: panel.debouncedQuery.trim() !== ""
                                              ? qsTr("No voice matches.")
                                              : qsTr("No built-in voice matches this filter.")
                                        color: theme.ink3
                                        font.pixelSize: 12
                                    }
                                    TextButton {
                                        visible: panel.query !== ""
                                        size: "compact"
                                        ghost: true
                                        text: qsTr("Clear filter")
                                        onClicked: panel.updateQuery("")
                                    }
                                }

                                TextButton {
                                    visible: listFooter.showMoreBtn
                                    size: "compact"
                                    ghost: true
                                    text: panel.allBuiltins ? qsTr("Show fewer voices")
                                        : qsTr("Show %1 more voices for this language…").arg(panel.hiddenBuiltinCount)
                                    onClicked: panel.allBuiltins = !panel.allBuiltins
                                }

                                // Flash Lite (narration.economy) toggle on Gemini tab.
                                Rectangle {
                                    Layout.fillWidth: true
                                    visible: listFooter.showEconomy
                                    radius: Ui.radius(10, theme.radius)
                                    color: Ui.tint(theme.ink1, 0.04)
                                    border.width: 1
                                    border.color: theme.hairline
                                    implicitHeight: economyRow.implicitHeight + 16
                                    RowLayout {
                                        id: economyRow
                                        x: 12
                                        y: 8
                                        width: parent.width - 24
                                        spacing: 12
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 1
                                            Label {
                                                text: qsTr("Flash Lite voice")
                                                color: theme.ink1
                                                font.pixelSize: 13
                                                font.weight: Font.Medium
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                text: qsTr("Faster and about a third cheaper, with slightly simpler delivery.")
                                                color: theme.ink4
                                                font.pixelSize: 12
                                                wrapMode: Text.Wrap
                                            }
                                        }
                                        Switch {
                                            checked: narration.economy
                                            Accessible.name: qsTr("Flash Lite voice")
                                            onToggled: narration.economy = checked
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // === 3. Clean Fixed Footer (Optional Gemini Style · Subtle Studio Mention · Listen CTA) ===
            Item {
                Layout.fillWidth: true
                implicitHeight: footerPanel.implicitHeight + 24

                Rectangle {
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    height: 1
                    color: Ui.tint(theme.ink1, 0.06)
                }

                ColumnLayout {
                    id: footerPanel
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: 12
                    anchors.leftMargin: 22
                    anchors.rightMargin: 22
                    spacing: 10

                    // Compact Style selector when browsing Gemini voices with a key.
                    RowLayout {
                        visible: panel.source === "gemini" && ai.hasKey
                        Layout.fillWidth: true
                        spacing: 14

                        Label {
                            text: qsTr("Style")
                            color: theme.ink1
                            font.pixelSize: 13
                            font.weight: Font.Medium
                        }

                        Segmented {
                            Layout.preferredWidth: Math.min(280, Math.max(200, sheetCard.width * 0.42))
                            compact: true
                            model: [
                                { value: "plain", label: qsTr("Plain"), enabled: true },
                                { value: "natural", label: qsTr("Natural"), enabled: !panel.styleDisabled },
                                { value: "dramatic", label: qsTr("Dramatic"), enabled: !panel.styleDisabled }
                            ]
                            current: panel.styleDisabled ? "plain" : narration.performance
                            onActivated: (value) => narration.performance = value
                        }

                        Label {
                            Layout.fillWidth: true
                            text: panel.styleDisabled ? qsTr("Voices on this computer read plainly.")
                                : narration.performance === "plain" ? qsTr("Even, clear, no direction.")
                                : narration.performance === "dramatic" ? qsTr("Fuller performance.")
                                : qsTr("Mood follows each scene.")
                            color: theme.ink4
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }
                    }

                    // Bottom bar: understated single-line Studio mention + primary Listen CTA.
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 12

                        Item {
                            id: studioNoteArea
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            implicitHeight: 24

                            Row {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6

                                Label {
                                    id: studioNoteLabel
                                    anchors.verticalCenter: parent.verticalCenter
                                    readonly property real maxLabelW: Math.max(48, studioNoteArea.width - studioDot.implicitWidth - studioSettingsLink.implicitWidth - 12)
                                    width: Math.min(implicitWidth, maxLabelW)
                                    text: panel.studioVoice
                                          ? qsTr("Studio voice active")
                                          : qsTr("Studio mode & full-cast narration")
                                    color: theme.ink4
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }

                                Label {
                                    id: studioDot
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: "·"
                                    color: theme.ink4
                                    font.pixelSize: 12
                                }

                                AbstractButton {
                                    id: studioSettingsLink
                                    anchors.verticalCenter: parent.verticalCenter
                                    implicitHeight: 24
                                    implicitWidth: studioLinkRow.implicitWidth + 10
                                    hoverEnabled: true
                                    focusPolicy: Qt.TabFocus
                                    Accessible.name: qsTr("Open Studio mode in Settings › Voices")
                                    onClicked: panel.openVoicesSettings()
                                    background: Rectangle {
                                        radius: Ui.radius(5, theme.radius)
                                        color: studioSettingsLink.pressed ? theme.fillPressed
                                             : studioSettingsLink.hovered ? theme.fill : "transparent"
                                        Rectangle {
                                            anchors.fill: parent
                                            anchors.margins: -2
                                            visible: studioSettingsLink.visualFocus
                                            radius: Ui.radius(7, theme.radius)
                                            color: "transparent"
                                            border.width: 2
                                            border.color: theme.accent
                                        }
                                    }
                                    contentItem: Row {
                                        id: studioLinkRow
                                        anchors.centerIn: parent
                                        spacing: 3
                                        Label {
                                            anchors.verticalCenter: parent.verticalCenter
                                            text: qsTr("Settings › Voices")
                                            color: studioSettingsLink.hovered ? theme.accentInk : theme.ink3
                                            font.pixelSize: 12
                                            font.weight: Font.Medium
                                        }
                                        Glyph {
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: 11
                                            height: 11
                                            stroke: 2.0
                                            path: Icons.chevronRight
                                            color: studioSettingsLink.hovered ? theme.accentInk : theme.ink4
                                        }
                                    }
                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                }
                            }
                        }

                        // Primary Listen CTA.
                        TextButton {
                            primary: true
                            enabled: !voicePacks.busy || ai.canSpeakWith(narration.voice)
                            text: narration.active && narration.playing ? qsTr("Done") : qsTr("Listen")
                            onClicked: panel.startOrFinishListening()
                        }
                    }
                }
            }
        }
    }
}
