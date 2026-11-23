import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Format.js" as Format
import "Icons.js" as Icons
import "Ui.js" as Ui

// Choose a narrator: centered 700×668 app-chrome modal sheet over a dimmed page
// scrim. Fixed header with 2-line source tabs (On this computer / Gemini /
// Studio), virtualized 222+ voice ListView with avatars, "Suits this book"
// pills, inline search/language filter, AI Voice Designer, collapsible pack &
// cache storage, and a fixed 3-row footer for Style, Full cast, and Download /
// M4B export + primary Listen CTA.
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

    // Which voices the sheet shows: "builtin", "gemini", or "studio".
    property string source: "gemini"
    // Built-in language filter: "__book__" (book's language), "" (all),
    // "__installed__" (installed only), or a BCP-47 base code ("en", "de"...).
    property string builtinFilter: "__book__"
    property bool allBuiltins: false
    property string pendingVoice: ""
    property int packsVersion: 0
    property bool storageExpanded: false

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
        downloadMenu.close()
        castPopup.close()
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
    function voiceSubtitle(voice) {
        const parts = []
        if (voice.kind === "builtin") {
            parts.push(voice.engine || qsTr("Built-in"))
            if (!voice.installed) {
                parts.push(qsTr("%1 MB to download").arg(Math.round((voice.bytes || 0) / 1e6).toLocaleString(Qt.locale(), 'f', 0)))
            } else {
                if (voice.note)
                    parts.push(voice.note)
                const speed = localSpeech.speedOf(voice.id)
                if (speed > 0 && translator.language !== "")
                    parts.push(qsTr("%1× real time").arg(speed.toLocaleString(Qt.locale(), 'f', 1)))
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
        if (voice.kind === "studio")
            return voice.note || voice.summary || qsTr("On your server")
        // Gemini library / classic voice
        parts.push("Gemini")
        if (voice.note)
            parts.push(voice.note)
        else if (voice.summary)
            parts.push(voice.summary)
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
    readonly property bool styleDisabled: source === "builtin" || builtinVoice

    // Downloading for offline listening / audiobook export.
    readonly property int lastChapter: reader.chapterCount - 1
    readonly property var chapterEstimate: open && voices.count >= 0 && narration.voice !== undefined && narration.economy !== undefined && reader.chapterMarks.length >= 0
        ? download.estimate(reader.chapter, reader.chapter) : ({})
    readonly property var restEstimate: open && voices.count >= 0 && narration.voice !== undefined && narration.economy !== undefined && reader.chapterMarks.length >= 0
        ? download.estimate(reader.chapter, lastChapter) : ({})
    function money(dollars) {
        return dollars < 0.01 ? qsTr("under $0.01") : qsTr("about %1").arg("$" + dollars.toLocaleString(Qt.locale(), 'f', 2))
    }
    readonly property bool downloadingThisBook: download.active && download.bookId === reader.bookId
    readonly property bool offlineApplies: (ai.hasKey || ai.canSpeak || ai.localSpeechModel !== "" || voicePacks.engineInstalled || voicePacks.megabytes >= 0)
                                           && narration.voice !== "" && ai.canSpeakWith(narration.voice)

    readonly property string language: reader.language
    readonly property var suggestions: open && voices.count >= 0 ? voices.suggestions(language) : []
    readonly property var narrators: suggestions.filter(voice => voice.kind === "library")
    readonly property var classics: suggestions.filter(voice => voice.kind === "classic")
    readonly property var yours: open && voices.count >= 0 ? voices.designedVoices() : []
    readonly property var geminiResults: open && debouncedQuery.trim() !== "" && voices.count >= 0
        ? voices.search(debouncedQuery, "", 120) : []
    readonly property var castChoices: cast.available ? cast.choices() : []

    // Built-in voices & packs.
    readonly property var builtinsForBook: open && packsVersion >= 0 ? voicePacks.voices(language) : []
    readonly property var builtinInstalledForBook: builtinsForBook.filter(voice => voice.installed)
    readonly property var installedPacks: open && packsVersion >= 0 ? voicePacks.installedPacks() : []
    readonly property var builtinLanguageOptions: {
        void(packsVersion)
        const opts = []
        if (panel.language !== "") {
            const bookLangName = Qt.locale(panel.language).nativeLanguageName || panel.language
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
        if (source === "studio") {
            const list = voices.studioVoices || []
            for (let i = 0; i < list.length; ++i)
                items.push({ designEntry: false, voice: list[i] })
            return items
        }
        if (source === "builtin" && voicePacks.supported) {
            const q = debouncedQuery.trim()
            const installedOnly = builtinFilter === "__installed__"
            const langCode = builtinFilter === "__book__" ? language
                           : builtinFilter === "__installed__" ? "" : builtinFilter
            let list = voicePacks.searchVoices(q, langCode, installedOnly)
            if (q === "" && builtinFilter === "__book__" && !allBuiltins && builtinInstalledForBook.length > 0) {
                const compact = builtinInstalledForBook.filter(v => v.recommended || v.id === narration.voice)
                // Also surface one uninstalled pack option (e.g. Piper) if space allows.
                const uninstalled = list.filter(v => !v.installed).slice(0, 2)
                list = compact.concat(uninstalled)
            }
            for (let i = 0; i < list.length; ++i)
                items.push({ designEntry: false, voice: list[i] })
            return items
        }
        // Gemini tab
        if (debouncedQuery.trim() !== "") {
            for (let i = 0; i < geminiResults.length; ++i)
                items.push({ designEntry: false, voice: geminiResults[i] })
            return items
        }
        for (let i = 0; i < yours.length; ++i)
            items.push({ designEntry: false, voice: yours[i] })
        for (let i = 0; i < narrators.length; ++i)
            items.push({ designEntry: false, voice: narrators[i] })
        for (let i = 0; i < classics.length; ++i)
            items.push({ designEntry: false, voice: classics[i] })
        if (ai.canDesignVoices)
            items.push({ designEntry: true, voice: { id: "__design__" } })
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

    Keys.onEscapePressed: {
        if (downloadMenu.opened)
            downloadMenu.close()
        else if (castPopup.opened)
            castPopup.close()
        else
            close()
    }

    // Fill the reader window so scrim & centered 700×668 modal always align.
    x: 0
    y: 0
    z: 10
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    visible: opacity > 0.01
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: theme.reducedMotion ? 140 : 180; easing.type: Easing.OutCubic } }

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
        readonly property bool playing: voices.previewing === voiceId
        implicitWidth: 30
        implicitHeight: 30
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.name: playing ? qsTr("Stop preview") : qsTr("Preview")
        background: Rectangle {
            radius: 15
            color: listen.playing ? theme.accentSoft
                 : listen.pressed ? theme.fillPressed
                 : listen.hovered ? theme.fillHover : theme.fill
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
                implicitWidth: 22
                implicitHeight: 22
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
        readonly property bool needsDownload: voice.kind === "builtin" && !voice.installed
        property bool confirming: false

        Timer {
            running: row.confirming
            interval: 4000
            onTriggered: row.confirming = false
        }

        implicitHeight: Math.max(52, rowLayout.implicitHeight + 16)
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.name: (voice.title || voice.name || "") + (voice.accent ? ", " + voice.accent : "")
        Accessible.checked: chosen
        onClicked: panel.choose(voice)

        background: Rectangle {
            radius: Ui.radius(10, theme.radius)
            color: row.chosen ? Ui.tint(theme.ink1, 0.07)
                 : row.pressed ? Ui.tint(theme.ink1, 0.06)
                 : row.hovered ? Ui.tint(theme.ink1, 0.04) : "transparent"
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
                        visible: text !== ""
                        text: row.voice.accent || ""
                        color: theme.ink3
                        font.pixelSize: 13
                        elide: Text.ElideRight
                        Layout.maximumWidth: 160
                    }
                    Rectangle {
                        visible: !!row.voice.suggested
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

            // 30×30 preview button (or download trigger when uninstalled).
            ListenBtn {
                visible: !row.needsDownload
                Layout.alignment: Qt.AlignVCenter
                voiceId: row.voice.id || ""
            }
            AbstractButton {
                id: dlRowBtn
                visible: row.needsDownload
                Layout.alignment: Qt.AlignVCenter
                implicitWidth: 30
                implicitHeight: 30
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                enabled: !voicePacks.busy
                onClicked: panel.choose(row.voice)
                background: Rectangle {
                    radius: 15
                    color: dlRowBtn.hovered ? theme.fillHover : theme.fill
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
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Download this voice")
                ToolTip.delay: 600
            }

            // 18 px checkmark slot so preview buttons align cleanly across rows.
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

    // "+ Design a narrator…" entry that expands into the full editable AI Casting card.
    component DesignNarratorEntry: Item {
        id: entry
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
                color: designRow.pressed ? Ui.tint(theme.ink1, 0.06)
                     : designRow.hovered ? Ui.tint(theme.ink1, 0.04) : "transparent"
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
        Label {
            Layout.fillWidth: true
            text: packRow.pack.title || packRow.pack.id || ""
            color: theme.ink1
            font.pixelSize: 12
            elide: Text.ElideRight
        }
        Label {
            text: Format.size((packRow.pack.megabytes || 0) * 1024 * 1024, Qt.locale())
            color: theme.ink4
            font.pixelSize: 11
            font.features: ({ "tnum": 1 })
        }
        TextButton {
            size: "compact"
            enabled: !voicePacks.busy
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

    // --- Centered 700×668 App-Chrome Modal Sheet ---
    Rectangle {
        id: sheetCard
        anchors.centerIn: parent
        width: Math.min(700, Math.max(380, panel.width - 48))
        height: Math.min(668, Math.max(420, panel.height - 64))
        radius: Ui.radius(16, theme.radius)
        color: theme.raised
        border.width: 1
        border.color: theme.hairline
        scale: panel.open || theme.reducedMotion ? 1 : 0.97
        Behavior on scale {
            enabled: !theme.reducedMotion
            NumberAnimation { duration: 320; easing.type: Easing.OutQuart }
        }

        // Deep modal shadow.
        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.topMargin: 18
            anchors.bottomMargin: -14
            anchors.leftMargin: -6
            anchors.rightMargin: -6
            radius: parent.radius > 0 ? parent.radius + 6 : 0
            color: theme.shadow
            opacity: 0.65
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
                implicitHeight: headerCol.implicitHeight + 36

                ColumnLayout {
                    id: headerCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.topMargin: 20
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
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Close (Esc)")
                            ToolTip.delay: 600
                        }
                    }

                    // 3-column 2-line Source Tabs.
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
                                subtitle: qsTr("Most natural")
                            },
                            {
                                value: "studio",
                                label: qsTr("Studio"),
                                subtitle: qsTr("Your server")
                            }
                        ]
                        current: panel.source
                        onActivated: (value) => {
                            panel.source = value
                            if (value === "studio" && ai.localSpeechModel !== "")
                                voices.loadStudioVoices()
                        }
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

            // === 2. Middle Virtualized Voice List ===
            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true

                ColumnLayout {
                    anchors.fill: parent
                    spacing: 0

                    // Compact section title + inline search & language filter.
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.leftMargin: 22
                        Layout.rightMargin: 18
                        Layout.topMargin: 12
                        Layout.bottomMargin: 6
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            text: panel.source === "builtin" ? qsTr("Made on this computer: instant, free, private")
                                : panel.source === "gemini" ? qsTr("Gemini voices, with your key")
                                : ai.localSpeechModel !== "" ? qsTr("From the server you set up · %1").arg(ai.localSpeechModel)
                                : qsTr("From the server you set up")
                            color: theme.ink4
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.2
                            elide: Text.ElideRight
                        }

                        ComboBox {
                            visible: voicePacks.supported && panel.source === "builtin"
                            Layout.preferredWidth: 146
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
                                }
                            }
                        }

                        Rectangle {
                            visible: panel.source !== "studio"
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
                                focusPolicy: Qt.NoFocus
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

                    // Virtualized ListView for 222+ built-in & Gemini voices.
                    ListView {
                        id: voiceList
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        spacing: 4
                        cacheBuffer: 360
                        model: panel.listItems
                        ScrollBar.vertical: ScrollBar {}

                        header: Item {
                            width: voiceList.width
                            implicitHeight: headerAlerts.implicitHeight > 0 ? headerAlerts.implicitHeight + 8 : 0
                            height: implicitHeight

                            ColumnLayout {
                                id: headerAlerts
                                x: 14
                                y: 2
                                width: parent.width - 28
                                spacing: 8

                                // Built-in pack download progress.
                                Rectangle {
                                    Layout.fillWidth: true
                                    visible: voicePacks.supported && panel.source === "builtin" && voicePacks.busy
                                    radius: Ui.radius(10, theme.radius)
                                    color: theme.fill
                                    border.width: 1
                                    border.color: theme.hairline
                                    implicitHeight: dlProgCol.implicitHeight + 20
                                    ColumnLayout {
                                        id: dlProgCol
                                        x: 12
                                        y: 10
                                        width: parent.width - 24
                                        spacing: 6
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Label {
                                                Layout.fillWidth: true
                                                text: voicePacks.progress >= 1.0
                                                      ? qsTr("Installing %1…").arg(voicePacks.installing)
                                                      : qsTr("Downloading %1… %2%")
                                                            .arg(voicePacks.installing)
                                                            .arg(Math.round(voicePacks.progress * 100))
                                                color: theme.ink1
                                                font.pixelSize: 12
                                                font.features: ({ "tnum": 1 })
                                            }
                                            TextButton {
                                                size: "compact"
                                                text: qsTr("Cancel")
                                                onClicked: {
                                                    panel.pendingVoice = ""
                                                    voicePacks.cancel()
                                                }
                                            }
                                        }
                                        ProgressBar {
                                            Layout.fillWidth: true
                                            value: voicePacks.progress
                                            indeterminate: voicePacks.progress >= 1.0
                                        }
                                    }
                                }

                                // Built-in pack error.
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 8
                                    Layout.rightMargin: 8
                                    visible: voicePacks.supported && panel.source === "builtin" && voicePacks.problem !== ""
                                    text: voicePacks.problem
                                    color: theme.cloudInk
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }

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
                                                onClicked: panel.source = "builtin"
                                            }
                                        }
                                    }
                                }

                                // Gemini fetch loading / problem.
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 8
                                    Layout.rightMargin: 8
                                    visible: panel.source === "gemini" && (voices.loading || voices.problem !== "")
                                    text: voices.loading ? qsTr("Fetching the voices…") : voices.problem
                                    color: voices.problem !== "" ? theme.cloudInk : theme.ink3
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }

                                // Studio honest unconfigured card or server error.
                                Rectangle {
                                    Layout.fillWidth: true
                                    visible: panel.source === "studio" && ai.localSpeechModel === ""
                                    radius: Ui.radius(10, theme.radius)
                                    color: theme.fill
                                    border.width: 1
                                    border.color: theme.hairline
                                    implicitHeight: studioEmptyCol.implicitHeight + 22
                                    ColumnLayout {
                                        id: studioEmptyCol
                                        x: 14
                                        y: 11
                                        width: parent.width - 28
                                        spacing: 8
                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("Connect a speech server on your computer or LAN (such as Qwen3-TTS) in Settings › Intelligence.")
                                            color: theme.ink1
                                            font.pixelSize: 13
                                            wrapMode: Text.Wrap
                                        }
                                        TextButton {
                                            primary: true
                                            text: qsTr("Open Intelligence settings")
                                            onClicked: {
                                                panel.close()
                                                win.showAiSettings()
                                            }
                                        }
                                    }
                                }
                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 8
                                    Layout.rightMargin: 8
                                    visible: panel.source === "studio" && ai.localSpeechModel !== "" && voices.problem !== ""
                                    text: voices.problem
                                    color: theme.cloudInk
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                            }
                        }

                        delegate: Item {
                            id: delWrap
                            required property var modelData
                            width: voiceList.width
                            height: modelData.designEntry ? designItem.implicitHeight : voiceItem.implicitHeight

                            VoiceRow {
                                id: voiceItem
                                visible: !delWrap.modelData.designEntry
                                x: 14
                                width: parent.width - 28
                                voice: delWrap.modelData.voice
                            }
                            DesignNarratorEntry {
                                id: designItem
                                visible: !!delWrap.modelData.designEntry
                                x: 14
                                width: parent.width - 28
                            }
                        }

                        footer: Item {
                            width: voiceList.width
                            implicitHeight: footerCol.implicitHeight + 16
                            height: implicitHeight

                            ColumnLayout {
                                id: footerCol
                                x: 14
                                y: 6
                                width: parent.width - 28
                                spacing: 8

                                Label {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 8
                                    visible: panel.listItems.length === 0
                                             && !(panel.source === "gemini" && (voices.loading || !ai.hasKey))
                                             && !(panel.source === "studio" && (ai.localSpeechModel === "" || voices.problem !== ""))
                                    text: panel.source === "studio"
                                          ? qsTr("The server lists no voices yet.")
                                          : panel.debouncedQuery.trim() !== ""
                                            ? qsTr("No voice matches.")
                                            : qsTr("No built-in voice matches this filter.")
                                    color: theme.ink3
                                    font.pixelSize: 12
                                }

                                TextButton {
                                    visible: panel.hiddenBuiltinCount > 0
                                             || (panel.source === "builtin" && panel.builtinFilter === "__book__"
                                                 && panel.debouncedQuery.trim() === "" && panel.allBuiltins
                                                 && panel.builtinInstalledForBook.length > 0)
                                    size: "compact"
                                    ghost: true
                                    text: panel.allBuiltins ? qsTr("Show fewer voices")
                                        : qsTr("Show %1 more voices for this language…").arg(panel.hiddenBuiltinCount)
                                    onClicked: panel.allBuiltins = !panel.allBuiltins
                                }

                                // Preserved Flash Lite (narration.economy) toggle on Gemini tab.
                                Rectangle {
                                    Layout.fillWidth: true
                                    visible: panel.source === "gemini" && ai.hasKey
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
                                            onToggled: narration.economy = checked
                                        }
                                    }
                                }

                                // Collapsible storage disclosure (installed packs + audio cache) — preserved cleanly.
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    visible: (voicePacks.supported && panel.installedPacks.length > 0) || audioCache.megabytes > 0.001
                                    spacing: 6

                                    AbstractButton {
                                        id: storageToggle
                                        Layout.fillWidth: true
                                        implicitHeight: 28
                                        hoverEnabled: true
                                        focusPolicy: Qt.TabFocus
                                        onClicked: panel.storageExpanded = !panel.storageExpanded
                                        background: Rectangle {
                                            radius: Ui.radius(7, theme.radius)
                                            color: storageToggle.hovered ? theme.fill : "transparent"
                                        }
                                        contentItem: RowLayout {
                                            spacing: 6
                                            Glyph {
                                                Layout.leftMargin: 6
                                                width: 12
                                                height: 12
                                                path: panel.storageExpanded ? Icons.chevronDown : Icons.chevronRight
                                                color: theme.ink4
                                            }
                                            Label {
                                                Layout.fillWidth: true
                                                text: {
                                                    const parts = [qsTr("Packs & audio cache")]
                                                    if (voicePacks.megabytes > 0.1)
                                                        parts.push(qsTr("%1 voices").arg(Format.size(voicePacks.megabytes * 1024 * 1024, Qt.locale())))
                                                    if (audioCache.megabytes > 0.001)
                                                        parts.push(qsTr("%1 cached").arg(Format.size(audioCache.megabytes * 1024 * 1024, Qt.locale())))
                                                    return parts.join(" · ")
                                                }
                                                color: theme.ink4
                                                font.pixelSize: 12
                                                font.features: ({ "tnum": 1 })
                                            }
                                        }
                                    }

                                    Rectangle {
                                        id: storageCard
                                        visible: panel.storageExpanded
                                        Layout.fillWidth: true
                                        radius: Ui.radius(10, theme.radius)
                                        color: Ui.tint(theme.ink1, 0.04)
                                        border.width: 1
                                        border.color: theme.hairline
                                        implicitHeight: storageInner.implicitHeight + 20

                                        property bool confirmingClear: false
                                        Timer {
                                            running: storageCard.confirmingClear
                                            interval: 4000
                                            onTriggered: storageCard.confirmingClear = false
                                        }

                                        ColumnLayout {
                                            id: storageInner
                                            x: 12
                                            y: 10
                                            width: parent.width - 24
                                            spacing: 8

                                            Repeater {
                                                model: voicePacks.supported ? panel.installedPacks : []
                                                delegate: PackRow {
                                                    required property var modelData
                                                    pack: modelData
                                                }
                                            }

                                            Rectangle {
                                                visible: voicePacks.supported && panel.installedPacks.length > 0
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 1
                                                color: theme.hairline
                                            }

                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 8
                                                Label {
                                                    Layout.fillWidth: true
                                                    text: audioCache.megabytes > 0.001
                                                          ? qsTr("Audio cache · %1 kept on disk").arg(Format.size(audioCache.megabytes * 1024 * 1024, Qt.locale()))
                                                          : qsTr("Audio cache · Empty")
                                                    color: theme.ink3
                                                    font.pixelSize: 12
                                                    font.features: ({ "tnum": 1 })
                                                }
                                                TextButton {
                                                    size: "compact"
                                                    enabled: audioCache.megabytes > 0.001
                                                    text: storageCard.confirmingClear ? qsTr("Clear cache?") : qsTr("Clear cache")
                                                    onClicked: {
                                                        if (!storageCard.confirmingClear) {
                                                            storageCard.confirmingClear = true
                                                            return
                                                        }
                                                        storageCard.confirmingClear = false
                                                        audioCache.clear()
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // === 3. Fixed 3-Row Footer (Style · Full Cast · Download + Listen) ===
            Item {
                Layout.fillWidth: true
                implicitHeight: footerPanel.implicitHeight + 28

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
                    anchors.topMargin: 14
                    anchors.leftMargin: 22
                    anchors.rightMargin: 22
                    spacing: 12

                    // Row 1: Style (always visible, Natural/Dramatic disabled on local voices).
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16

                        Label {
                            Layout.preferredWidth: 88
                            text: qsTr("Style")
                            color: theme.ink1
                            font.pixelSize: 13
                            font.weight: Font.Medium
                        }

                        Segmented {
                            Layout.preferredWidth: Math.min(300, Math.max(210, sheetCard.width * 0.43))
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

                    // Row 2: Full cast (dynamic speaker preview + Edit cast popup + 38×22 Switch).
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 16

                        Label {
                            Layout.preferredWidth: 88
                            text: qsTr("Full cast")
                            color: theme.ink1
                            font.pixelSize: 13
                            font.weight: Font.Medium
                        }

                        Label {
                            Layout.fillWidth: true
                            text: !cast.available
                                  ? qsTr("Available with Kokoro or Gemini voices (several voices in one family).")
                                  : cast.members.length > 0
                                    ? qsTr("A voice for each character: %1… Chosen from this narrator's family.")
                                          .arg(cast.members.slice(0, 3).map(m => m.speaker).join(", "))
                                    : qsTr("A voice for each character, chosen from this narrator's family (%1).").arg(ai.whereFor("cast"))
                            color: theme.ink3
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                            maximumLineCount: 2
                            elide: Text.ElideRight
                        }

                        TextButton {
                            id: editCastBtn
                            visible: cast.available && cast.enabled && cast.members.length > 0
                            size: "compact"
                            ghost: true
                            text: qsTr("Cast (%1)").arg(cast.members.length)
                            onClicked: castPopup.opened ? castPopup.close() : castPopup.open()

                            Popup {
                                id: castPopup
                                y: -height - 10
                                x: Math.min(0, editCastBtn.width - width)
                                width: 390
                                height: Math.min(340, castListCol.implicitHeight + 24)
                                padding: 12
                                background: Rectangle {
                                    radius: Ui.radius(12, theme.radius)
                                    color: theme.overlay
                                    border.width: 1
                                    border.color: theme.hairline
                                }
                                contentItem: Flickable {
                                    clip: true
                                    contentHeight: castListCol.implicitHeight
                                    boundsBehavior: Flickable.StopAtBounds
                                    ScrollBar.vertical: ScrollBar {}
                                    ColumnLayout {
                                        id: castListCol
                                        width: parent.width
                                        spacing: 8
                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("Characters heard so far")
                                            color: theme.ink3
                                            font.pixelSize: 11
                                            font.weight: Font.DemiBold
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
                                                    color: theme.ink1
                                                    font.pixelSize: 13
                                                    elide: Text.ElideRight
                                                }
                                                ComboBox {
                                                    Layout.preferredWidth: 165
                                                    readonly property var choices: panel.castChoices
                                                    model: choices.map(c => c.name + (c.gender ? " · " + c.gender : ""))
                                                    currentIndex: choices.findIndex(c => c.id === modelData.voice)
                                                    onActivated: (index) => {
                                                        if (index >= 0 && index < choices.length)
                                                            cast.setVoice(modelData.speaker, choices[index].id)
                                                    }
                                                }
                                                ListenBtn { voiceId: modelData.voice }
                                            }
                                        }
                                    }
                                }
                            }
                        }

                        Switch {
                            checked: cast.enabled
                            enabled: cast.available
                            Accessible.name: qsTr("Full cast")
                            onToggled: cast.enabled = checked
                        }
                    }

                    // Row 3: Download the book (M4B / offline popover) + Primary Listen CTA.
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.topMargin: 2
                        spacing: 8

                        // Idle Download trigger + anchored chapter/book choice popover.
                        TextButton {
                            id: downloadBtn
                            visible: !panel.downloadingThisBook && !audiobook.busy && audiobook.saved === "" && audiobook.problem === ""
                            glyph: Icons.download
                            text: qsTr("Download the book")
                            enabled: panel.offlineApplies
                            onClicked: downloadMenu.opened ? downloadMenu.close() : downloadMenu.open()

                            Popup {
                                id: downloadMenu
                                y: -height - 10
                                x: 0
                                width: 356
                                padding: 14
                                background: Rectangle {
                                    radius: Ui.radius(12, theme.radius)
                                    color: theme.overlay
                                    border.width: 1
                                    border.color: theme.hairline
                                }
                                contentItem: ColumnLayout {
                                    spacing: 10
                                    Label {
                                        Layout.fillWidth: true
                                        text: panel.localVoice
                                              ? qsTr("Render narration ahead on this computer so you can save an M4B audiobook with chapters.")
                                              : qsTr("Download narration for offline listening at Gemini's slower Flex tier (half price).")
                                        color: theme.ink3
                                        font.pixelSize: 12
                                        wrapMode: Text.Wrap
                                        lineHeight: 1.35
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 8
                                        TextButton {
                                            text: qsTr("This chapter")
                                            enabled: (panel.chapterEstimate.chapters || 0) > 0
                                            onClicked: {
                                                downloadMenu.close()
                                                download.start(reader.chapter, reader.chapter)
                                            }
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: (panel.chapterEstimate.chapters || 0) > 0 && translator.language !== ""
                                                  ? Format.duration(panel.chapterEstimate.minutes)
                                                    + (panel.chapterEstimate.local ? "" : " · " + panel.money(panel.chapterEstimate.cost))
                                                  : ""
                                            color: theme.ink4
                                            font.pixelSize: 12
                                            elide: Text.ElideRight
                                        }
                                    }
                                    RowLayout {
                                        visible: (panel.restEstimate.chapters || 0) > 1
                                        Layout.fillWidth: true
                                        spacing: 8
                                        TextButton {
                                            text: qsTr("To the end")
                                            onClicked: {
                                                downloadMenu.close()
                                                download.start(reader.chapter, panel.lastChapter)
                                            }
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: translator.language !== ""
                                                  ? qsTr("%n chapters", "", panel.restEstimate.chapters || 0)
                                                    + " · " + Format.duration(panel.restEstimate.minutes || 0)
                                                    + (panel.restEstimate.local ? "" : " · " + panel.money(panel.restEstimate.cost || 0))
                                                  : ""
                                            color: theme.ink4
                                            font.pixelSize: 12
                                            elide: Text.ElideRight
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        visible: !panel.restEstimate.local && (panel.restEstimate.cost || 0) > 0
                                                 && !ai.fitsBudget(panel.restEstimate.cost)
                                        text: qsTr("Downloading to the end would pass your monthly budget; it stops there.")
                                        color: theme.cloudInk
                                        font.pixelSize: 12
                                        wrapMode: Text.Wrap
                                    }
                                }
                            }
                        }

                        Label {
                            visible: !panel.downloadingThisBook && !audiobook.busy && audiobook.saved === "" && audiobook.problem === ""
                            Layout.fillWidth: true
                            text: panel.source === "builtin" ? qsTr("Free · made in the background")
                                : panel.source === "studio" ? qsTr("Free · runs on your server")
                                : qsTr("At half price, shown before it starts")
                            color: theme.ink4
                            font.pixelSize: 12
                            elide: Text.ElideRight
                        }

                        // Active offline download / M4B save / Saved reveal controls.
                        RowLayout {
                            visible: panel.downloadingThisBook || audiobook.busy || audiobook.saved !== "" || audiobook.problem !== ""
                            Layout.fillWidth: true
                            spacing: 8

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                ProgressBar {
                                    visible: panel.downloadingThisBook || audiobook.busy
                                    Layout.fillWidth: true
                                    value: audiobook.busy ? audiobook.progress : download.fraction
                                    indeterminate: !audiobook.busy && download.running && download.fraction === 0
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: audiobook.problem !== "" ? audiobook.problem
                                        : audiobook.busy ? qsTr("Saving audiobook… %1%").arg(Math.round(audiobook.progress * 100))
                                        : audiobook.saved !== "" ? qsTr("Saved %1").arg(audiobook.saved.replace(/^.*\/(Music\/)/, "$1"))
                                        : download.problem !== "" ? download.problem
                                        : download.finished ? qsTr("Ready offline")
                                        : translator.language === "" ? ""
                                        : qsTr("%1 of %2 ready").arg(Format.duration(download.minutesReady)).arg(Format.duration(download.minutesTotal))
                                          + (download.running && download.secondsLeft > 0
                                             ? " · " + qsTr("%1 left").arg(Format.duration(download.secondsLeft / 60)) : "")
                                          + (!download.running ? " · " + qsTr("paused") : "")
                                    color: (audiobook.problem !== "" || download.problem !== "") ? theme.cloudInk
                                         : audiobook.saved !== "" ? theme.positiveInk : theme.ink3
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }
                            }

                            TextButton {
                                visible: panel.downloadingThisBook && !download.finished
                                size: "compact"
                                text: download.running ? qsTr("Pause") : qsTr("Resume")
                                onClicked: download.running ? download.pause() : download.resume()
                            }
                            TextButton {
                                visible: panel.downloadingThisBook && download.finished && !audiobook.busy
                                size: "compact"
                                text: qsTr("Save M4B")
                                onClicked: audiobook.save()
                            }
                            TextButton {
                                visible: audiobook.busy
                                size: "compact"
                                text: qsTr("Cancel")
                                onClicked: audiobook.cancel()
                            }
                            TextButton {
                                visible: audiobook.saved !== "" && !audiobook.busy
                                size: "compact"
                                text: qsTr("Reveal")
                                onClicked: audiobook.revealSaved()
                            }
                            TextButton {
                                visible: panel.downloadingThisBook && !audiobook.busy
                                size: "compact"
                                ghost: true
                                text: download.finished ? qsTr("Done") : qsTr("Stop")
                                onClicked: download.cancel()
                            }
                        }

                        // Primary Listen CTA.
                        TextButton {
                            primary: true
                            text: narration.active && narration.playing ? qsTr("Done") : qsTr("Listen")
                            onClicked: {
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
                        }
                    }
                }
            }
        }
    }
}
