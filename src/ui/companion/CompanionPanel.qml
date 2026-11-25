import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts
import QtQuick.Window
import "../components"

import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui

// The reading companion, beside the page: Ask about the book, Previously…,
// Who's who, Pictures, and what was asked of a selection (Define, Explain,
// Translate). Nothing past the reader's place is ever seen.
FocusScope {
    id: panel

    property bool open: false
    // Kept for EpubReader binding; Companion wears app chrome (theme.drawer + theme.accent).
    property var look
    // "ask", "recap", "whoswho", "pictures", or "selection" (define, explain, translate).
    property string tab: "ask"
    property bool clearConfirmOpen: false
    property bool recapCopied: false
    property bool selectionCopied: false
    property string throttledAnswer: ""
    property string throttledRendered: ""
    property int throttledFormat: TextEdit.MarkdownText

    readonly property int defaultWidth: 384
    readonly property int minWidth: 320
    readonly property int maxWidth: Math.max(minWidth, Math.min(760, Math.floor((parent ? parent.width : 1280) * 0.56)))
    property real preferredWidth: uiSettings.companionWidth
    readonly property bool resizing: resizeDrag.active

    // Crisp 24x24 speaker glyph for response read-aloud actions (matches Icons.js grid).
    readonly property string speakerIcon: "M4.5 9.5 H8.5 L13.5 5.5 V18.5 L8.5 14.5 H4.5 Z M16.5 9 A4.2 4.2 0 0 1 16.5 15 M19 6.5 A7.5 7.5 0 0 1 19 17.5"

    signal closed()
    signal jumpRequested(int chapter, int offset)
    signal saveAsNote(string quote, string note)

    // App-chrome palette independent of Paper/Sepia reading themes.
    palette.window: theme.drawer
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
    palette.light: theme.raised
    palette.shadow: theme.shadow
    palette.highlight: theme.accent
    palette.highlightedText: theme.accentOn
    palette.link: theme.accentInk
    palette.linkVisited: theme.accentSoft
    palette.toolTipBase: theme.overlay
    palette.toolTipText: theme.ink1

    Settings {
        id: companionChoice
        location: settingsUrl
        category: "companion"
        property bool notNow: false
    }
    Settings {
        id: uiSettings
        location: settingsUrl
        category: "ui"
        property int companionWidth: 384
    }
    Connections {
        target: uiSettings
        function onCompanionWidthChanged() {
            if (!panel.resizing && !snapWidthAnim.running)
                panel.preferredWidth = uiSettings.companionWidth
        }
    }
    NumberAnimation {
        id: snapWidthAnim
        target: panel
        property: "preferredWidth"
        duration: theme.reducedMotion ? 0 : 180
        easing.type: Easing.OutCubic
        onFinished: uiSettings.companionWidth = Math.round(panel.width)
    }

    function openIntelligenceSettings() {
        if (typeof win !== "undefined" && win && typeof win.showAiSettings === "function")
            win.showAiSettings()
    }
    function focusComposer() {
        Qt.callLater(() => {
            if (panel.open && !panel.choosing && panel.tab === "ask")
                question.forceActiveFocus()
        })
    }
    function show(which, focusInput) {
        if (which)
            tab = which
        clearConfirmOpen = false
        open = true
        if (focusInput && !choosing && tab === "ask") {
            forceActiveFocus()
            focusComposer()
        }
    }
    function close() {
        if (!open)
            return
        resizeDrag.active = false
        clearConfirmOpen = false
        if (picturePreview.opened)
            picturePreview.close()
        open = false
        closed()
    }
    function showTab(which) {
        tab = which
        clearConfirmOpen = false
        if (which === "recap") {
            if (!ai.hasKey && !ai.hasLocal && !companion.recapLoaded)
                return
            const wholeStudy = companion.studyMode && companion.scope !== "upto"
            const stale = wholeStudy ? (companion.recapChapter !== -1) : (companion.recapChapter !== reader.chapter)
            if (!companion.recapLoaded || stale || companion.problem !== "")
                companion.recap()
        } else if (which === "whoswho") {
            if (!ai.hasKey && !ai.hasLocal && !companion.peopleLoaded)
                return
            companion.whosWho()
        }
    }
    function isMathSymbolBadge(name) {
        const s = String(name || "").trim()
        return companion.studyMode && s.length > 0 && s.length <= 4
    }
    function hasNonAsciiSymbol(name) {
        return /[^\x00-\x7F]/.test(String(name || ""))
    }
    function renderProse(text) {
        if (!text)
            return ""
        return companion.hasMath(text) ? companion.formatDisplayHtml(text, theme.fill) : companion.sanitizeMarkdown(text)
    }
    function proseFormat(text) {
        return text && companion.hasMath(text) ? TextEdit.RichText : TextEdit.MarkdownText
    }
    function flushStreamRender(force) {
        streamThrottle.stop()
        const raw = companion.answer || ""
        if (!force && raw === throttledAnswer)
            return
        throttledAnswer = raw
        if (raw === "") {
            throttledFormat = TextEdit.MarkdownText
            throttledRendered = ""
        } else {
            const hasM = companion.hasMath(raw)
            throttledFormat = hasM ? TextEdit.RichText : TextEdit.MarkdownText
            throttledRendered = hasM ? companion.formatDisplayHtml(raw, theme.fill) : companion.sanitizeMarkdown(raw)
        }
    }
    function syncConversation() {
        const turns = companion.conversation || []
        let prefixMatch = talkModel.count <= turns.length
        if (prefixMatch) {
            for (let i = 0; i < talkModel.count; ++i) {
                const existing = talkModel.get(i)
                const next = turns[i]
                const nextSnip = next && next.snipUrl ? String(next.snipUrl) : ""
                if (!next || existing.fromUser !== !!next.fromUser || existing.text !== String(next.text || "")
                    || existing.snipUrl !== nextSnip) {
                    prefixMatch = false
                    break
                }
            }
        }
        if (!prefixMatch)
            talkModel.clear()
        for (let i = talkModel.count; i < turns.length; ++i) {
            const t = turns[i]
            talkModel.append({
                fromUser: !!t.fromUser,
                text: String(t.text || ""),
                snipUrl: String(t.snipUrl || ""),
                local: !!t.local,
                provenance: String(t.provenance || ""),
                sourcesJson: t.sources && t.sources.length > 0 ? JSON.stringify(t.sources) : ""
            })
        }
    }
    Component.onCompleted: {
        syncConversation()
        flushStreamRender(true)
    }
    function jumpOrOpenCitation(cite) {
        if (!cite)
            return
        if (cite.bookId && cite.bookId > 0 && cite.bookId !== reader.bookId)
            companion.activateCitation(cite.bookId, cite.chapter || 0, cite.offset || 0, cite.excerpt || "")
        else
            panel.jumpRequested(cite.chapter || 0, cite.offset || 0)
    }
    function placeLabel(chapter) {
        const ch = chapter >= 0 ? chapter : reader.chapter
        const title = reader.titleOfChapter(ch)
        if (title && title !== "")
            return qsTr("“%1”").arg(title)
        if (reader.format === "pdf") {
            const pageLbl = reader.pdfPageLabel(ch)
            return qsTr("Page %1").arg(pageLbl && pageLbl !== "" ? pageLbl : (ch + 1))
        }
        return qsTr("Chapter %1").arg(ch + 1)
    }
    function formatCents(dollars) {
        const value = Number(dollars) || 0
        if (value <= 0)
            return qsTr("less than 1¢")
        if (value < 0.01)
            return qsTr("less than 1¢")
        if (value < 1.0)
            return qsTr("%1¢").arg(Math.max(1, Math.round(value * 100)))
        return value.toLocaleCurrencyString(Qt.locale("en_US"), "$")
    }
    function personInitials(name) {
        const clean = String(name || "").trim().replace(/^(?:the|an?|mr\.?|mrs\.?|ms\.?|miss|dr\.?|sir|lord|lady)\s+/i, "")
        if (clean.length === 0)
            return "?"
        const parts = clean.split(/\s+/).filter(p => p.length > 0)
        if (parts.length >= 2)
            return (parts[0].charAt(0) + parts[parts.length - 1].charAt(0)).toUpperCase()
        return parts[0].charAt(0).toUpperCase()
    }
    function personTint(name) {
        const key = String(name || "")
        let hash = 0
        for (let i = 0; i < key.length; ++i)
            hash = ((hash << 5) - hash + key.charCodeAt(i)) | 0
        const paletteDark = ["#2a3548", "#342d46", "#25393a", "#3f3328", "#3d2b36", "#2b382f"]
        const paletteLight = ["#dfe7f5", "#e9e2f6", "#dceeed", "#f3e7d8", "#f2e1ea", "#e0eee4"]
        const set = theme.light ? paletteLight : paletteDark
        return set[Math.abs(hash) % set.length]
    }
    function leftOffLabel() {
        const prev = Number(reader.previousOpenedAt) || 0
        if (prev > 0) {
            const elapsedSec = Math.max(0, Math.floor(Date.now() / 1000) - prev)
            const days = Math.floor(elapsedSec / 86400)
            if (days >= 2)
                return qsTr("Where you left off · %n days ago", "", days)
            if (days === 1)
                return qsTr("Where you left off · yesterday")
            const hours = Math.floor(elapsedSec / 3600)
            if (hours >= 2)
                return qsTr("Where you left off · %n hours ago", "", hours)
        }
        return qsTr("Where you left off")
    }
    function sendText(raw) {
        const asked = String(raw || "").trim()
        if (asked === "" || companion.busy)
            return
        if (!ai.hasKey && !ai.hasLocal) {
            companionChoice.notNow = false
            return
        }
        clearConfirmOpen = false
        if (panel.tab !== "ask")
            panel.tab = "ask"
        talk.stickToBottom = true
        companion.ask(asked)
        Qt.callLater(talk.scrollToBottom)
    }
    function send() {
        const hasText = question.text.trim() !== ""
        if ((!hasText && !companion.hasPendingSnip) || companion.busy)
            return
        if (!ai.hasKey && !ai.hasLocal) {
            companionChoice.notNow = false
            return
        }
        const asked = question.text
        question.text = ""
        clearConfirmOpen = false
        if (panel.tab !== "ask")
            panel.tab = "ask"
        talk.stickToBottom = true
        companion.ask(asked)
        Qt.callLater(talk.scrollToBottom)
    }
    function askAboutSelection() {
        const subj = activeSelectionSubject.trim()
        panel.tab = "ask"
        clearConfirmOpen = false
        if (subj !== "") {
            const snippet = subj.length > 100 ? subj.slice(0, 100).trim() + "…" : subj
            question.text = qsTr("Tell me more about “%1”").arg(snippet)
            question.cursorPosition = question.text.length
        }
        focusComposer()
    }

    readonly property bool activeSelectionBusy: companion.busy && ["define", "explain", "translate"].indexOf(companion.feature) >= 0
    readonly property string activeSelectionFeature: activeSelectionBusy
        ? companion.feature
        : (companion.selectionFeature !== "" ? companion.selectionFeature
           : (["define", "explain", "translate"].indexOf(companion.feature) >= 0 ? companion.feature : ""))
    readonly property string activeSelectionSubject: activeSelectionBusy
        ? companion.subject
        : (companion.selectionSubject !== "" ? companion.selectionSubject : companion.subject)
    readonly property string activeSelectionAnswer: activeSelectionBusy
        ? companion.answer
        : (companion.selectionAnswer !== "" ? companion.selectionAnswer
           : (["define", "explain", "translate"].indexOf(companion.feature) >= 0 ? companion.answer : ""))
    readonly property string activeSelectionWhere: activeSelectionBusy
        ? companion.where
        : (companion.selectionWhere !== "" ? companion.selectionWhere : companion.where)
    readonly property bool activeSelectionWhereLocal: activeSelectionBusy
        ? companion.whereLocal
        : (companion.selectionFeature !== "" ? companion.selectionWhereLocal : companion.whereLocal)

    readonly property bool selectionFeature: activeSelectionFeature !== ""
    readonly property string recapDisplayText: (companion.busy && companion.feature === "recap")
        ? companion.answer
        : (companion.recapText !== "" ? companion.recapText : (companion.feature === "recap" ? companion.answer : ""))
    readonly property string recapDisplayWhere: (companion.busy && companion.feature === "recap")
        ? companion.where
        : (companion.recapWhere !== "" ? companion.recapWhere : companion.where)
    readonly property bool recapDisplayWhereLocal: (companion.busy && companion.feature === "recap")
        ? companion.whereLocal
        : (companion.recapLoaded ? companion.recapWhereLocal : companion.whereLocal)

    // The routing feature behind the tab shown, for where its answers are made.
    readonly property string routeFeature: tab === "selection" ? (activeSelectionFeature || "explain")
                                         : tab === "pictures" ? "illustrate" : tab
    // With no key and no model, the panel first asks where the companion
    // should run, unless the reader said "Not now".
    readonly property bool choosing: !ai.hasKey && !ai.hasLocal && !companionChoice.notNow
    readonly property string featureName: ({ define: qsTr("Define"), explain: qsTr("Explain"), translate: qsTr("Translate") })[activeSelectionFeature] || ""
    readonly property string headerWhere: {
        void(ai.preset); void(ai.hasKey); void(ai.hasLocal); void(ai.localIsLoopback); void(ai.overrideCount)
        void(illustrator.available); void(illustrator.budgetOk)
        if (tab === "pictures")
            return (illustrator.available && illustrator.budgetOk) ? "cloud" : "none"
        if (tab === "selection" && activeSelectionFeature === "define" && activeSelectionWhereLocal)
            return "local"
        const route = ai.routeFor(routeFeature)
        if (route === "local")
            return ai.localIsLoopback ? "local" : "cloud"
        if (route === "cloud")
            return "cloud"
        return "none"
    }
    readonly property string headerWhereLabel: {
        void(ai.preset); void(ai.hasKey); void(ai.hasLocal); void(ai.localIsLoopback); void(ai.localName); void(ai.overrideCount)
        void(illustrator.available); void(illustrator.budgetOk)
        if (tab === "pictures") {
            if (!illustrator.available)
                return qsTr("Unavailable")
            return illustrator.budgetOk ? qsTr("Gemini") : qsTr("Budget cap")
        }
        if (tab === "selection" && activeSelectionFeature === "define" && activeSelectionWhereLocal) {
            return activeSelectionWhere === qsTr("Dictionary on this computer")
                ? qsTr("Dictionary") : qsTr("This computer")
        }
        const route = ai.routeFor(routeFeature)
        if (route === "local")
            return ai.localIsLoopback ? qsTr("This computer") : (ai.localName || qsTr("Remote"))
        if (route === "cloud")
            return qsTr("Gemini")
        return qsTr("Not set up")
    }
    readonly property var tryPrompts: {
        void(reader.bookId); void(reader.chapter); void(reader.chapterTitle)
        void(companion.conversation); void(companion.people); void(companion.peopleLoaded)
        void(companion.readingMode); void(companion.scope)
        return panel.open ? companion.suggestions() : []
    }
    readonly property var askEstimateInfo: {
        void(reader.chapter); void(reader.pageEnd); void(reader.progress)
        void(ai.preset); void(ai.localUrl); void(ai.localTextModel); void(ai.hasKey); void(ai.overrideCount)
        void(companion.scope); void(companion.hasPendingSnip)
        return (panel.open && (question.text !== "" || companion.hasPendingSnip)) ? companion.askEstimate() : ({})
    }
    readonly property string effectiveTrustScope: {
        if (panel.tab === "ask")
            return companion.scope
        if (companion.studyMode && companion.scope !== "upto")
            return "book"
        return "upto"
    }
    readonly property string trustScopeText: {
        if (effectiveTrustScope === "shelf")
            return companion.shelfLabel !== "" ? qsTr("Shelf · %1").arg(companion.shelfLabel) : qsTr("Across your library")
        if (effectiveTrustScope === "book")
            return qsTr("Whole document")
        return qsTr("Up to page (%1%). Never ahead.").arg(Math.round(reader.progress * 100))
    }
    readonly property string trustScopeTooltip: {
        if (effectiveTrustScope === "shelf")
            return companion.shelfLabel !== ""
                ? qsTr("Searches and cites passages across books in %1.").arg(companion.shelfLabel)
                : qsTr("Searches and cites passages across your library.")
        if (effectiveTrustScope === "book")
            return qsTr("Reads the whole document, including proofs, results, and appendices.")
        return qsTr("Knows the book up to your page (%1%). Never ahead.").arg(Math.round(reader.progress * 100))
    }

    // Seamless handoff straight from first-run onboarding into the user's pending question or tab.
    onChoosingChanged: {
        if (!choosing && (ai.hasKey || ai.hasLocal)) {
            if (companion.hasPendingSetup) {
                companion.resumePendingSetup()
                if (tab === "ask")
                    focusComposer()
            } else {
                companion.clearProblem()
                if (tab === "recap" && !companion.recapLoaded)
                    companion.recap()
                else if (tab === "whoswho" && !companion.peopleLoaded)
                    companion.whosWho()
                else if (tab === "ask")
                    focusComposer()
            }
        }
    }

    Connections {
        target: narration
        function onResponseSpeechProblem(message) {
            if (message && message !== "")
                toast.show(message)
        }
    }

    Connections {
        target: illustrator
        function onSavedCopy(path) {
            toast.show(qsTr("Saved to %1").arg(path.replace(/^.*\/(Pictures\/)/, "$1")))
        }
        function onSaveFailed(message) {
            toast.show(message || qsTr("Couldn't save a copy of this picture"))
        }
    }

    ListModel {
        id: talkModel
    }
    Timer {
        id: streamThrottle
        interval: 60
        repeat: false
        onTriggered: panel.flushStreamRender(false)
    }
    Connections {
        target: theme
        function onChanged() {
            if (panel.throttledAnswer !== "")
                panel.flushStreamRender(true)
        }
    }

    Connections {
        target: companion
        function onConversationChanged() {
            panel.syncConversation()
            if (talk.stickToBottom)
                Qt.callLater(talk.scrollToBottom)
        }
        function onAnswerChanged() {
            if (!companion.busy || companion.answer === "") {
                panel.flushStreamRender(false)
                return
            }
            if (panel.throttledAnswer === "")
                panel.flushStreamRender(false)
            else if (!streamThrottle.running)
                streamThrottle.start()
        }
        function onBusyChanged() {
            panel.flushStreamRender(false)
            if (!companion.busy && companion.feature === "ask" && talk.stickToBottom)
                Qt.callLater(talk.scrollToBottom)
        }
        function onReadingModeChanged() {
            if (companion.studyMode && panel.tab === "pictures")
                panel.tab = "ask"
        }
        function onOpenAskRequested() {
            panel.show("ask")
        }
    }

    Timer {
        id: recapCopiedTimer
        interval: 1500
        onTriggered: panel.recapCopied = false
    }
    Timer {
        id: selectionCopiedTimer
        interval: 1500
        onTriggered: panel.selectionCopied = false
    }

    Keys.onEscapePressed: {
        if (picturePreview.opened)
            picturePreview.close()
        else if (clearConfirmOpen)
            clearConfirmOpen = false
        else
            close()
    }

    // Updated only after the drawer finishes sliding so readingStage / WebEngine
    // never resize or reflow mid-animation.
    property bool settledOpen: false
    readonly property bool sliding: slideAnim.running

    onOpenChanged: {
        if (!open)
            resizeDrag.active = false
        if (theme.reducedMotion)
            settledOpen = open
    }

    property real slideProgress: open ? 1 : 0
    Behavior on slideProgress {
        NumberAnimation {
            id: slideAnim
            duration: theme.reducedMotion ? 110 : (panel.open ? 260 : 210)
            easing.type: panel.open ? Easing.OutQuint : Easing.OutCubic
            onRunningChanged: {
                if (!running)
                    panel.settledOpen = panel.open
            }
        }
    }

    width: Math.max(minWidth, Math.min(maxWidth, Math.round(preferredWidth)))
    readonly property real slideTravel: width + 24
    x: theme.reducedMotion
       ? (parent ? parent.width - width : 0)
       : (parent ? parent.width - width + slideTravel * (1 - slideProgress) : 0)
    opacity: theme.reducedMotion ? slideProgress : Math.min(1, slideProgress * 2.5)
    visible: theme.reducedMotion ? opacity > 0.01 : slideProgress > 0.005
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Companion")

    // Soft shadow toward the page.
    Rectangle {
        anchors.right: parent.left
        width: 18
        height: parent.height
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: "transparent" }
            GradientStop { position: 1; color: Ui.tint("#000000", theme.light ? 0.08 : 0.30) }
        }
    }
    Rectangle {
        anchors.fill: parent
        color: theme.drawer
        Rectangle {
            width: 1
            height: parent.height
            color: panel.look && !panel.look.dark && !theme.light ? Ui.tint("#000000", 0.20) : theme.hairline
        }
    }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    // Invisible 6px left-edge resize sash with a quiet 2px accent hairline on hover/drag.
    MouseArea {
        id: resizeDrag
        property bool active: false
        property real startParentX: 0
        property real startWidth: 384
        x: -3
        y: 0
        width: 6
        height: parent.height
        z: 20
        enabled: panel.open
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeHorCursor

        onPressed: (mouse) => {
            if (!panel.parent)
                return
            snapWidthAnim.stop()
            const pt = mapToItem(panel.parent, mouse.x, mouse.y)
            startParentX = pt.x
            startWidth = panel.width
            active = true
        }
        onPositionChanged: (mouse) => {
            if (!active || !pressed || !panel.parent)
                return
            const pt = mapToItem(panel.parent, mouse.x, mouse.y)
            const delta = startParentX - pt.x
            const clamped = Math.max(panel.minWidth, Math.min(panel.maxWidth, Math.round(startWidth + delta)))
            panel.preferredWidth = clamped
        }
        onReleased: {
            if (!active)
                return
            active = false
            uiSettings.companionWidth = Math.round(panel.width)
        }
        onCanceled: {
            if (!active)
                return
            active = false
            uiSettings.companionWidth = Math.round(panel.width)
        }
        onDoubleClicked: {
            active = false
            const target = Math.max(panel.minWidth, Math.min(panel.maxWidth, panel.defaultWidth))
            if (theme.reducedMotion) {
                panel.preferredWidth = target
                uiSettings.companionWidth = target
            } else {
                snapWidthAnim.stop()
                snapWidthAnim.from = panel.width
                snapWidthAnim.to = target
                snapWidthAnim.start()
            }
        }

        Rectangle {
            x: 2
            y: 0
            width: 2
            height: parent.height
            color: theme.accent
            opacity: resizeDrag.active ? 0.85 : (resizeDrag.containsMouse ? 0.55 : 0)
            Behavior on opacity {
                enabled: !theme.reducedMotion
                NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
            }
        }
    }

    component JumpChip: AbstractButton {
        id: jumpChip
        property string tipText: ""
        implicitHeight: 22
        implicitWidth: Math.min(Math.max(120, panel.width - 48), jumpLabel.implicitWidth + 16)
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        ToolTip.visible: hovered && tipText !== ""
        ToolTip.delay: 450
        ToolTip.text: tipText
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        background: Rectangle {
            radius: Ui.radius(10, theme.radius)
            color: jumpChip.pressed ? Ui.tint(theme.accent, 0.34)
                 : jumpChip.hovered ? Ui.tint(theme.accent, 0.28) : theme.accentSoft
            Behavior on color { enabled: !theme.reducedMotion; ColorAnimation { duration: 90 } }
            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                visible: jumpChip.visualFocus
                radius: Ui.radius(13, theme.radius)
                color: "transparent"
                border.width: 2
                border.color: theme.accent
            }
        }
        contentItem: Label {
            id: jumpLabel
            text: jumpChip.text
            color: theme.accentInk
            font.pixelSize: 11
            font.weight: Font.Medium
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            leftPadding: 8
            rightPadding: 8
        }
    }
    component SmallAction: AbstractButton {
        id: smallAction
        property bool positive: false
        implicitHeight: 22
        implicitWidth: smallActionLabel.implicitWidth + 12
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        opacity: enabled ? 1 : 0.42
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        background: Rectangle {
            radius: Ui.radius(5, theme.radius)
            color: smallAction.pressed ? theme.fillPressed
                 : smallAction.hovered ? theme.fillHover : "transparent"
            Rectangle {
                anchors.fill: parent
                anchors.margins: -2
                visible: smallAction.visualFocus
                radius: Ui.radius(7, theme.radius)
                color: "transparent"
                border.width: 2
                border.color: theme.accent
            }
        }
        contentItem: Label {
            id: smallActionLabel
            text: smallAction.text
            color: smallAction.positive ? theme.positiveInk
                 : smallAction.hovered ? theme.ink1 : theme.ink4
            font.pixelSize: 11
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
    // Understated Listen / Stop response speech button (inline 22px in Ask, 28px in Brief/Selection).
    component SpeakButton: AbstractButton {
        id: speakBtn
        property string itemId: ""
        property string responseText: ""
        property bool inlineMode: false

        readonly property bool isThisItem: itemId !== "" && narration.speakingResponseId === itemId
        readonly property bool bufferingThis: isThisItem && narration.responseBuffering
        readonly property bool activeThis: isThisItem && narration.speakingResponse
        readonly property bool stopping: activeThis && !bufferingThis

        implicitHeight: inlineMode ? 22 : 28
        implicitWidth: speakRow.implicitWidth + (inlineMode ? 12 : 20)
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.name: activeThis ? qsTr("Stop reading response") : qsTr("Listen to response")
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        ToolTip.visible: hovered
        ToolTip.delay: 500
        ToolTip.text: bufferingThis ? qsTr("Preparing voice…")
                    : stopping ? qsTr("Stop reading aloud")
                    : qsTr("Read response aloud")

        onClicked: {
            if (speakBtn.activeThis || speakBtn.bufferingThis)
                narration.stopResponse()
            else
                narration.speakResponse(speakBtn.responseText, speakBtn.itemId)
        }

        background: Rectangle {
            radius: Ui.radius(speakBtn.inlineMode ? 5 : 7, theme.radius)
            color: speakBtn.activeThis
                 ? (speakBtn.pressed ? Ui.tint(theme.accent, 0.28)
                    : speakBtn.hovered ? Ui.tint(theme.accent, 0.22) : theme.accentSoft)
                 : speakBtn.inlineMode
                 ? (speakBtn.pressed ? theme.fillPressed
                    : speakBtn.hovered ? theme.fillHover : "transparent")
                 : (speakBtn.pressed ? Ui.tint(theme.ink1, 0.16)
                    : speakBtn.hovered ? Ui.tint(theme.ink1, 0.12) : Ui.tint(theme.ink1, 0.08))
            border.width: speakBtn.activeThis ? 1 : 0
            border.color: Ui.tint(theme.accent, 0.42)
            Behavior on color { enabled: !theme.reducedMotion; ColorAnimation { duration: 100 } }
            Rectangle {
                anchors.fill: parent
                anchors.margins: speakBtn.inlineMode ? -2 : -3
                visible: speakBtn.visualFocus
                radius: Ui.radius(speakBtn.inlineMode ? 7 : 10, theme.radius)
                color: "transparent"
                border.width: 2
                border.color: theme.accent
            }
        }

        contentItem: Item {
            Row {
                id: speakRow
                anchors.centerIn: parent
                spacing: speakBtn.inlineMode ? 4 : 6

                Glyph {
                    visible: !speakBtn.activeThis && !speakBtn.bufferingThis
                    anchors.verticalCenter: parent.verticalCenter
                    width: speakBtn.inlineMode ? 12 : 14
                    height: speakBtn.inlineMode ? 12 : 14
                    stroke: 1.85
                    path: panel.speakerIcon
                    color: speakBtn.inlineMode
                         ? (speakBtn.hovered ? theme.ink1 : theme.ink4)
                         : theme.ink1
                }
                BusyIndicator {
                    visible: speakBtn.bufferingThis
                    running: visible
                    anchors.verticalCenter: parent.verticalCenter
                    padding: 0
                    implicitWidth: speakBtn.inlineMode ? 13 : 15
                    implicitHeight: speakBtn.inlineMode ? 13 : 15
                    palette.highlight: theme.accentInk
                }
                Rectangle {
                    visible: speakBtn.stopping
                    anchors.verticalCenter: parent.verticalCenter
                    width: speakBtn.inlineMode ? 8 : 9
                    height: speakBtn.inlineMode ? 8 : 9
                    radius: 2
                    color: theme.accentInk
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: speakBtn.activeThis ? qsTr("Stop") : qsTr("Listen")
                    color: speakBtn.activeThis ? theme.accentInk
                         : speakBtn.inlineMode ? (speakBtn.hovered ? theme.ink1 : theme.ink4)
                         : theme.ink1
                    font.pixelSize: speakBtn.inlineMode ? 11 : 12
                    font.weight: speakBtn.activeThis ? Font.DemiBold : Font.Medium
                }
            }
        }
    }
    // Words to read, selectable for copying (Literata for Previously… & selection prose).
    component Prose: TextEdit {
        Layout.fillWidth: true
        readOnly: true
        selectByMouse: true
        wrapMode: Text.Wrap
        textFormat: TextEdit.MarkdownText
        color: theme.ink2
        selectionColor: theme.accentSoft
        font.pixelSize: 15
        font.family: "Literata"
    }
    // Shimmering prose loading placeholder.
    component ProseSkeleton: ColumnLayout {
        property bool compact: false
        Layout.fillWidth: true
        spacing: compact ? 8 : 10
        Skeleton { Layout.fillWidth: true; Layout.preferredHeight: compact ? 12 : 14 }
        Skeleton { Layout.fillWidth: true; Layout.rightMargin: 22; Layout.preferredHeight: compact ? 12 : 14 }
        Skeleton { Layout.fillWidth: true; Layout.rightMargin: 48; Layout.preferredHeight: compact ? 12 : 14 }
        Skeleton { visible: !compact; Layout.fillWidth: true; Layout.topMargin: 6; Layout.rightMargin: 12; Layout.preferredHeight: 14 }
        Skeleton { visible: !compact; Layout.fillWidth: true; Layout.rightMargin: 76; Layout.preferredHeight: 14 }
    }
    // Provenance line with computer/cloud glyph under each result.
    component ProvenanceRow: RowLayout {
        id: prov
        property string text: ""
        property bool local: false
        Layout.fillWidth: true
        visible: text !== ""
        spacing: 6
        Glyph {
            Layout.preferredWidth: 12
            Layout.preferredHeight: 12
            stroke: 2.0
            path: prov.local ? Icons.computer : Icons.cloud
            color: theme.ink4
        }
        Label {
            Layout.fillWidth: true
            text: prov.text
            color: theme.ink4
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }
    }
    component Working: RowLayout {
        property bool active: true
        Layout.fillWidth: true
        visible: active && companion.busy
        spacing: 8
        BusyIndicator { running: parent.visible; implicitWidth: 18; implicitHeight: 18 }
        Label {
            Layout.fillWidth: true
            text: companion.status
            color: theme.ink3
            font.pixelSize: 12
            elide: Text.ElideRight
        }
        TextButton { size: "compact"; text: qsTr("Stop"); onClicked: companion.stop() }
    }
    component ErrorCard: Rectangle {
        id: errCard
        property string message: ""
        property bool showRetry: false
        property bool showSettings: false
        signal retryClicked()
        signal dismissClicked()

        Layout.fillWidth: true
        visible: message !== ""
        implicitHeight: errCol.implicitHeight + 20
        radius: Ui.radius(10, theme.radius)
        color: Ui.tint(theme.danger, 0.08)
        border.width: 1
        border.color: Ui.tint(theme.danger, 0.24)

        ColumnLayout {
            id: errCol
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: errCard.message
                color: theme.danger
                font.pixelSize: 12
                wrapMode: Text.Wrap
                lineHeight: 1.4
            }
            Flow {
                Layout.fillWidth: true
                visible: errCard.showRetry || errCard.showSettings
                spacing: 6
                TextButton {
                    visible: errCard.showRetry
                    size: "compact"
                    text: qsTr("Try again")
                    onClicked: errCard.retryClicked()
                }
                TextButton {
                    visible: errCard.showSettings
                    size: "compact"
                    primary: !errCard.showRetry
                    text: qsTr("Intelligence settings")
                    onClicked: panel.openIntelligenceSettings()
                }
                TextButton {
                    size: "compact"
                    ghost: true
                    text: qsTr("Dismiss")
                    onClicked: errCard.dismissClicked()
                }
            }
        }
    }
    component UnconfiguredCard: Rectangle {
        Layout.fillWidth: true
        implicitHeight: setupCol.implicitHeight + 24
        radius: Ui.radius(10, theme.radius)
        color: Ui.tint(theme.ink1, 0.04)
        border.width: 1
        border.color: theme.hairline
        ColumnLayout {
            id: setupCol
            anchors.fill: parent
            anchors.margins: 12
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: qsTr("Companion isn't set up yet")
                color: theme.ink1
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Choose a model on this computer or connect Gemini with your key to ask about the book.")
                color: theme.ink3
                font.pixelSize: 12
                wrapMode: Text.Wrap
                lineHeight: 1.45
            }
            RowLayout {
                spacing: 8
                TextButton {
                    size: "compact"
                    primary: true
                    text: qsTr("Choose where it runs")
                    onClicked: companionChoice.notNow = false
                }
                TextButton {
                    size: "compact"
                    ghost: true
                    text: qsTr("Settings")
                    onClicked: panel.openIntelligenceSettings()
                }
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- Header block (title, Story|Study pill, segmented tabs, trust/scope & provider) ---
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: 14
            Layout.bottomMargin: 12
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            spacing: 10

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Glyph { Layout.preferredWidth: 16; Layout.preferredHeight: 16; stroke: 1.8; path: Icons.sparkle; color: theme.accent }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Companion")
                    color: theme.ink1
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                // Quiet, compact Story | Study mode switcher pill in the Companion header.
                Rectangle {
                    id: modePill
                    visible: !panel.choosing
                    implicitHeight: 24
                    implicitWidth: modeRow.implicitWidth + 4
                    radius: Ui.radius(12, theme.radius)
                    color: Ui.tint(theme.ink1, 0.06)

                    Rectangle {
                        x: companion.studyMode ? (2 + storyBtn.width) : 2
                        y: 2
                        width: companion.studyMode ? studyBtn.width : storyBtn.width
                        height: parent.height - 4
                        radius: Ui.radius(10, theme.radius)
                        color: Ui.tint(theme.ink1, 0.14)
                        Behavior on x { enabled: !theme.reducedMotion; NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                        Behavior on width { enabled: !theme.reducedMotion; NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }
                    }
                    Row {
                        id: modeRow
                        x: 2
                        height: parent.height
                        AbstractButton {
                            id: storyBtn
                            height: parent.height
                            implicitWidth: storyLbl.implicitWidth + 14
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            Accessible.name: qsTr("Story mode")
                            Accessible.checked: !companion.studyMode
                            onClicked: companion.readingMode = "fiction"
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            ToolTip.text: qsTr("Story mode · spoiler-free reading, recaps & characters")
                            background: null
                            contentItem: Label {
                                id: storyLbl
                                text: qsTr("Story")
                                color: !companion.studyMode ? theme.ink1 : (storyBtn.hovered ? theme.ink2 : theme.ink3)
                                font.pixelSize: 11
                                font.weight: !companion.studyMode ? Font.Medium : Font.Normal
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        AbstractButton {
                            id: studyBtn
                            height: parent.height
                            implicitWidth: studyLbl.implicitWidth + 14
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            Accessible.name: qsTr("Study mode")
                            Accessible.checked: companion.studyMode
                            onClicked: companion.readingMode = "study"
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            ToolTip.visible: hovered
                            ToolTip.delay: 500
                            ToolTip.text: qsTr("Study mode · whole document, structured brief & notation glossary")
                            background: null
                            contentItem: Label {
                                id: studyLbl
                                text: qsTr("Study")
                                color: companion.studyMode ? theme.ink1 : (studyBtn.hovered ? theme.ink2 : theme.ink3)
                                font.pixelSize: 11
                                font.weight: companion.studyMode ? Font.Medium : Font.Normal
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }
                IconButton {
                    visible: !panel.choosing && panel.tab === "ask" && companion.conversation.length > 0 && !companion.busy
                    implicitWidth: 28
                    implicitHeight: 28
                    glyph: Icons.plus
                    tone: panel.clearConfirmOpen ? theme.accentInk : theme.ink3
                    tip: qsTr("New conversation")
                    onClicked: panel.clearConfirmOpen = !panel.clearConfirmOpen
                }
                IconButton {
                    implicitWidth: 28
                    implicitHeight: 28
                    glyph: Icons.close
                    tone: theme.ink3
                    tip: qsTr("Close companion (C)")
                    onClicked: panel.close()
                }
            }

            // Quiet inline confirmation before clearing a multi-turn thread.
            Rectangle {
                visible: !panel.choosing && panel.tab === "ask" && panel.clearConfirmOpen && companion.conversation.length > 0
                Layout.fillWidth: true
                implicitHeight: clearRow.implicitHeight + 16
                radius: Ui.radius(9, theme.radius)
                color: Ui.tint(theme.ink1, 0.05)
                border.width: 1
                border.color: theme.hairline
                RowLayout {
                    id: clearRow
                    anchors.fill: parent
                    anchors.leftMargin: 10
                    anchors.rightMargin: 8
                    anchors.topMargin: 8
                    anchors.bottomMargin: 8
                    spacing: 6
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Start a new thread?")
                        color: theme.ink2
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }
                    TextButton {
                        size: "compact"
                        ghost: true
                        text: qsTr("Cancel")
                        onClicked: panel.clearConfirmOpen = false
                    }
                    TextButton {
                        size: "compact"
                        primary: true
                        text: qsTr("Clear")
                        onClicked: {
                            panel.clearConfirmOpen = false
                            companion.newConversation()
                            panel.focusComposer()
                        }
                    }
                }
            }

            Segmented {
                visible: !panel.choosing
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                compact: true
                model: {
                    const tabs = companion.studyMode
                        ? [{ value: "ask", label: qsTr("Ask") },
                           { value: "recap", label: qsTr("Brief") },
                           { value: "whoswho", label: qsTr("Glossary") }]
                        : [{ value: "ask", label: qsTr("Ask") },
                           { value: "recap", label: qsTr("Previously") },
                           { value: "whoswho", label: qsTr("Who's who") },
                           { value: "pictures", label: qsTr("Pictures") }]
                    if (panel.selectionFeature)
                        tabs.push({ value: "selection", label: panel.featureName })
                    return tabs
                }
                current: panel.tab
                onActivated: (value) => panel.showTab(value)
            }

            // Honest scope / spoiler-shield line + quiet provider routing pill.
            RowLayout {
                visible: !panel.choosing
                Layout.fillWidth: true
                spacing: 6
                Glyph {
                    Layout.preferredWidth: 14
                    Layout.preferredHeight: 14
                    stroke: 1.9
                    path: panel.effectiveTrustScope === "shelf" ? Icons.books
                        : panel.effectiveTrustScope === "book" ? Icons.bookOpen
                        : Icons.shieldCheck
                    color: panel.effectiveTrustScope === "upto" ? theme.positive : theme.accentInk
                }
                Label {
                    Layout.fillWidth: true
                    text: panel.trustScopeText
                    color: theme.ink3
                    font.pixelSize: 12
                    elide: Text.ElideRight
                    HoverHandler { id: trustHover }
                    ToolTip.visible: trustHover.hovered
                    ToolTip.delay: 450
                    ToolTip.text: panel.trustScopeTooltip
                }
                ProviderPill {
                    where: panel.headerWhere
                    text: panel.headerWhereLabel
                    interactive: true
                    onClicked: panel.openIntelligenceSettings()
                    ToolTip.visible: hovered
                    ToolTip.delay: 450
                    ToolTip.text: {
                        const base = panel.tab === "pictures"
                            ? (illustrator.available && illustrator.budgetOk ? illustrator.where : illustrator.unavailableReason)
                            : ai.whereFor(panel.routeFeature)
                        return base ? qsTr("%1 · Open Intelligence settings").arg(base) : qsTr("Open Intelligence settings")
                    }
                }
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }

        // --- First time (J5): where the companion runs ---------------------
        Flickable {
            visible: panel.choosing
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentHeight: chooserItem.implicitHeight + 36
            boundsBehavior: Flickable.StopAtBounds
            maximumFlickVelocity: 4500
            flickDeceleration: 2200
            pixelAligned: true
            ScrollBar.vertical: ScrollBar {}
            SmoothWheel { stepSize: 115 }

            CompanionChooser {
                id: chooserItem
                x: 16
                y: 20
                width: parent.width - 32
                look: panel.look
                onNotNow: { companionChoice.notNow = true; panel.close() }
            }
        }

        // --- Ask -----------------------------------------------------------
        Item {
            visible: !panel.choosing && panel.tab === "ask"
            Layout.fillWidth: true
            Layout.fillHeight: true

            ListView {
                id: talk
                property bool stickToBottom: true
                property bool autoScrolling: false
                property real lastContentY: 0
                function scrollToBottom() {
                    autoScrolling = true
                    const minY = originY - topMargin
                    const targetY = Math.max(minY, originY + contentHeight + bottomMargin - height)
                    contentY = targetY
                    lastContentY = targetY
                    autoScrolling = false
                }
                anchors.fill: parent
                clip: true
                topMargin: 18
                bottomMargin: 18
                spacing: 16
                cacheBuffer: 600
                model: talkModel
                boundsBehavior: Flickable.StopAtBounds
                maximumFlickVelocity: 4500
                flickDeceleration: 2200
                pixelAligned: true
                ScrollBar.vertical: ScrollBar {}
                SmoothWheel { stepSize: 115 }

                // Respect manual upward wheel, drag, or scrollbar scrolling during streaming.
                onContentYChanged: {
                    if (!autoScrolling) {
                        const maxY = Math.max(originY - topMargin, originY + contentHeight + bottomMargin - height)
                        const nearBottom = (contentY >= maxY - 28)
                        if (nearBottom)
                            stickToBottom = true
                        else if (contentY < lastContentY - 2)
                            stickToBottom = false
                    }
                    lastContentY = contentY
                }
                onContentHeightChanged: {
                    if (stickToBottom && (talkModel.count > 0 || (companion.feature === "ask" && companion.busy)))
                        Qt.callLater(scrollToBottom)
                }

                header: Item {
                    width: ListView.view ? ListView.view.width : 0
                    visible: talkModel.count === 0
                    implicitHeight: visible ? headerCol.implicitHeight : 0
                    height: implicitHeight

                    ColumnLayout {
                        id: headerCol
                        x: 16
                        width: parent.width - 32
                        spacing: 16

                        // Re-entry card if the reader chose "Not now" earlier and still has no model.
                        UnconfiguredCard {
                            visible: companionChoice.notNow && !ai.hasKey && !ai.hasLocal && talkModel.count === 0
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: !(companionChoice.notNow && !ai.hasKey && !ai.hasLocal)
                                     && talkModel.count === 0 && !(companion.feature === "ask" && companion.busy)
                            text: companion.scope === "shelf"
                                  ? qsTr("Synthesize ideas, compare methods, and trace citations across documents on this shelf.")
                                  : companion.scope === "book"
                                  ? (companion.studyMode
                                     ? qsTr("Ask about equations, proofs, methods, or findings anywhere in this document — or snip any figure or formula.")
                                     : qsTr("Ask anything across the entire book, including later chapters and appendices."))
                                  : qsTr("Ask anything about the book so far: who someone is, why something happened, or what a phrase means. Nothing past your page is read.")
                            color: theme.ink3
                            font.pixelSize: 13
                            wrapMode: Text.Wrap
                            lineHeight: 1.5
                        }
                    }
                }

                delegate: Item {
                    id: turnWrap
                    required property int index
                    required property bool fromUser
                    required property string text
                    required property string snipUrl
                    required property bool local
                    required property string provenance
                    required property string sourcesJson
                    width: ListView.view ? ListView.view.width : 0
                    implicitHeight: turnBox.implicitHeight
                    height: implicitHeight

                    ColumnLayout {
                        id: turnBox
                        x: 16
                        width: parent.width - 32
                        property bool copied: false
                        readonly property bool hasUserSnip: turnWrap.fromUser && turnWrap.snipUrl !== ""
                        readonly property var turnSources: turnWrap.sourcesJson !== "" ? JSON.parse(turnWrap.sourcesJson) : []
                        readonly property bool turnHasMath: !turnWrap.fromUser && turnWrap.text !== "" && companion.hasMath(turnWrap.text)
                        spacing: 10

                        Timer {
                            id: copyReset
                            interval: 1500
                            onTriggered: turnBox.copied = false
                        }

                        // User bubble (neutral 8%, right-aligned, max 280px, optional local snip thumbnail)
                        // or Assistant reply (Inter 13px with offline math support).
                        Item {
                            Layout.fillWidth: true
                            implicitHeight: bubbleRect.implicitHeight

                            TextMetrics {
                                id: userMetrics
                                font: said.font
                                text: turnWrap.fromUser ? turnWrap.text : ""
                            }

                            Rectangle {
                                id: bubbleRect
                                readonly property real maxBubbleW: Math.min(Math.max(280, parent.width * 0.78), parent.width * 0.88)
                                width: turnWrap.fromUser
                                       ? (turnBox.hasUserSnip
                                          ? maxBubbleW
                                          : Math.min(maxBubbleW, Math.max(48, Math.ceil(userMetrics.advanceWidth) + 26)))
                                       : parent.width
                                x: turnWrap.fromUser ? Math.max(0, parent.width - width) : 0
                                implicitHeight: bubbleContent.implicitHeight + (turnWrap.fromUser ? 18 : 0)
                                height: implicitHeight
                                radius: Ui.radius(12, theme.radius)
                                color: turnWrap.fromUser ? Ui.tint(theme.ink1, 0.08) : "transparent"

                                Column {
                                    id: bubbleContent
                                    x: turnWrap.fromUser ? 12 : 0
                                    y: turnWrap.fromUser ? 9 : 0
                                    width: parent.width - (turnWrap.fromUser ? 24 : 0)
                                    spacing: 8

                                    // Trusted local user-snipped thumbnail (strictly from local snip attachment, never LLM Markdown).
                                    Rectangle {
                                        visible: turnBox.hasUserSnip
                                        width: parent.width
                                        height: visible
                                                ? (snipThumb.implicitWidth > 0 && snipThumb.implicitHeight > 0
                                                   ? Math.min(136, Math.max(56, Math.round((width - 8) * snipThumb.implicitHeight / snipThumb.implicitWidth) + 8))
                                                   : 84)
                                                : 0
                                        radius: Ui.radius(8, theme.radius)
                                        color: theme.drawer
                                        border.width: 1
                                        border.color: theme.hairline
                                        clip: true

                                        Image {
                                            id: snipThumb
                                            anchors.fill: parent
                                            anchors.margins: 4
                                            source: turnBox.hasUserSnip ? turnWrap.snipUrl : ""
                                            fillMode: Image.PreserveAspectFit
                                            asynchronous: true
                                            sourceSize.width: 512
                                            sourceSize.height: 272
                                            mipmap: true
                                            smooth: true
                                        }
                                    }

                                    TextEdit {
                                        id: said
                                        width: parent.width
                                        text: turnWrap.fromUser
                                              ? turnWrap.text
                                              : (turnWrap.text === "" ? ""
                                                 : (turnBox.turnHasMath
                                                    ? companion.formatDisplayHtml(turnWrap.text, theme.fill)
                                                    : companion.sanitizeMarkdown(turnWrap.text)))
                                        readOnly: true
                                        selectByMouse: true
                                        wrapMode: Text.Wrap
                                        textFormat: turnWrap.fromUser
                                                    ? TextEdit.PlainText
                                                    : (turnBox.turnHasMath ? TextEdit.RichText : TextEdit.MarkdownText)
                                        color: turnWrap.fromUser ? theme.ink1 : theme.ink2
                                        selectionColor: theme.accentSoft
                                        font.pixelSize: 13
                                    }
                                }
                            }
                        }

                        Flow {
                            Layout.fillWidth: true
                            visible: !turnWrap.fromUser && turnBox.turnSources.length > 0
                            spacing: 6
                            Repeater {
                                model: !turnWrap.fromUser ? turnBox.turnSources : []
                                delegate: JumpChip {
                                    required property var modelData
                                    text: modelData.label || panel.placeLabel(modelData.chapter)
                                    tipText: modelData.bookTitle
                                             ? (modelData.bookTitle + (modelData.excerpt ? " — " + modelData.excerpt : ""))
                                             : (modelData.excerpt || "")
                                    onClicked: panel.jumpOrOpenCitation(modelData)
                                }
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: !turnWrap.fromUser && turnWrap.text !== ""
                            spacing: 4

                            ProvenanceRow {
                                local: turnWrap.local
                                text: turnWrap.provenance
                            }

                            RowLayout {
                                spacing: 4
                                SmallAction {
                                    positive: turnBox.copied
                                    text: turnBox.copied ? qsTr("Copied ✓") : qsTr("Copy")
                                    onClicked: {
                                        reader.copyText(turnWrap.text)
                                        turnBox.copied = true
                                        copyReset.restart()
                                        toast.show(qsTr("Copied"))
                                    }
                                }
                                SpeakButton {
                                    inlineMode: true
                                    itemId: "ask-" + turnWrap.index
                                    responseText: turnWrap.text
                                }
                                SmallAction {
                                    text: qsTr("Save as note")
                                    onClicked: {
                                        const prev = turnWrap.index > 0 ? talkModel.get(turnWrap.index - 1) : null
                                        const quote = prev && prev.fromUser ? prev.text : panel.placeLabel(reader.chapter)
                                        panel.saveAsNote(quote, turnWrap.text)
                                    }
                                }
                            }
                        }
                    }
                }

                footer: Item {
                    width: ListView.view ? ListView.view.width : 0
                    readonly property real topGap: talkModel.count > 0 ? 16 : 0
                    readonly property real minEmptyH: talkModel.count === 0
                        ? Math.max(0, talk.height - 36 - (talk.headerItem ? talk.headerItem.height : 0))
                        : 0
                    implicitHeight: Math.max(footerCol.implicitHeight + topGap, minEmptyH)
                    height: implicitHeight

                    ColumnLayout {
                        id: footerCol
                        x: 16
                        y: parent.topGap
                        width: parent.width - 32
                        height: parent.height - parent.topGap
                        spacing: 16

                        // Crisp thinking skeleton while waiting for first streamed token.
                        ProseSkeleton {
                            compact: true
                            visible: companion.feature === "ask" && companion.busy && panel.throttledAnswer === ""
                        }

                        // Live streaming reply (throttled ~60 ms; matches committed assistant turn typography).
                        TextEdit {
                            Layout.fillWidth: true
                            visible: companion.feature === "ask" && companion.busy && panel.throttledAnswer !== ""
                            readOnly: true
                            selectByMouse: true
                            wrapMode: Text.Wrap
                            textFormat: panel.throttledFormat
                            text: panel.throttledRendered
                            color: theme.ink2
                            selectionColor: theme.accentSoft
                            font.pixelSize: 13
                        }
                        Flow {
                            Layout.fillWidth: true
                            visible: companion.feature === "ask" && companion.busy && companion.currentSources.length > 0
                            spacing: 6
                            Repeater {
                                model: companion.currentSources
                                delegate: JumpChip {
                                    required property var modelData
                                    text: modelData.label || panel.placeLabel(modelData.chapter)
                                    tipText: modelData.bookTitle
                                             ? (modelData.bookTitle + (modelData.excerpt ? " — " + modelData.excerpt : ""))
                                             : (modelData.excerpt || "")
                                    onClicked: panel.jumpOrOpenCitation(modelData)
                                }
                            }
                        }
                        Working { active: panel.tab === "ask" && companion.feature === "ask" }
                        ErrorCard {
                            message: panel.tab === "ask" && companion.feature === "ask" ? companion.problem : ""
                            showRetry: companion.canRetry
                            showSettings: companion.problemIsConfig
                            onRetryClicked: companion.retry()
                            onDismissClicked: companion.clearProblem()
                        }

                        Item {
                            Layout.fillHeight: true
                            visible: talkModel.count === 0
                        }

                        // Suggested starter / follow-up prompts ("Try").
                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: !companion.busy && (ai.hasKey || ai.hasLocal) && panel.tryPrompts.length > 0
                            spacing: 6
                            Label {
                                text: qsTr("Try")
                                color: theme.ink4
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }
                            Repeater {
                                model: panel.tryPrompts
                                delegate: AbstractButton {
                                    id: tryBtn
                                    required property string modelData
                                    Layout.fillWidth: true
                                    implicitHeight: tryLabel.implicitHeight + 16
                                    hoverEnabled: true
                                    focusPolicy: Qt.TabFocus
                                    onClicked: panel.sendText(modelData)
                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                    background: Rectangle {
                                        radius: Ui.radius(7, theme.radius)
                                        color: tryBtn.pressed ? theme.fillPressed
                                             : tryBtn.hovered ? theme.fill : "transparent"
                                        border.width: 1
                                        border.color: theme.hairline
                                        Behavior on color { enabled: !theme.reducedMotion; ColorAnimation { duration: 90 } }
                                        Rectangle {
                                            anchors.fill: parent
                                            anchors.margins: -3
                                            visible: tryBtn.visualFocus
                                            radius: Ui.radius(10, theme.radius)
                                            color: "transparent"
                                            border.width: 2
                                            border.color: theme.accent
                                        }
                                    }
                                    contentItem: Label {
                                        id: tryLabel
                                        text: tryBtn.modelData
                                        color: tryBtn.hovered ? theme.ink1 : theme.ink2
                                        font.pixelSize: 13
                                        wrapMode: Text.Wrap
                                        leftPadding: 10
                                        rightPadding: 10
                                        topPadding: 8
                                        bottomPadding: 8
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Floating "Jump to latest" pill when reader scrolls up during a thread or stream.
            AbstractButton {
                id: jumpLatestBtn
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 12
                readonly property bool wanted: !talk.stickToBottom && (talk.contentHeight + talk.topMargin + talk.bottomMargin) > talk.height + 40
                                               && (talkModel.count > 0 || (companion.feature === "ask" && companion.busy))
                opacity: wanted ? 1 : 0
                visible: opacity > 0.01
                Behavior on opacity { enabled: !theme.reducedMotion; NumberAnimation { duration: 140 } }
                implicitHeight: 28
                implicitWidth: jumpLatestRow.implicitWidth + 18
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                onClicked: {
                    talk.stickToBottom = true
                    talk.scrollToBottom()
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                background: Rectangle {
                    radius: Ui.radius(14, theme.radius)
                    color: jumpLatestBtn.hovered ? theme.raised : theme.overlay
                    border.width: 1
                    border.color: theme.separator
                }
                contentItem: Row {
                    id: jumpLatestRow
                    anchors.centerIn: parent
                    spacing: 5
                    Glyph {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 11
                        height: 11
                        stroke: 2.2
                        path: Icons.chevronDown
                        color: theme.ink1
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Latest")
                        color: theme.ink1
                        font.pixelSize: 11
                        font.weight: Font.Medium
                    }
                }
            }
        }

        // --- Previously…, and what was asked of a selection ----------------
        Flickable {
            visible: !panel.choosing && (panel.tab === "recap" || panel.tab === "selection")
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentHeight: proseColumn.implicitHeight + 36
            boundsBehavior: Flickable.StopAtBounds
            maximumFlickVelocity: 4500
            flickDeceleration: 2200
            pixelAligned: true
            ScrollBar.vertical: ScrollBar {}
            SmoothWheel { stepSize: 115 }

            ColumnLayout {
                id: proseColumn
                x: 16
                y: 18
                width: parent.width - 32
                spacing: 14
                readonly property bool isRecap: panel.tab === "recap"
                readonly property bool isSelection: panel.tab === "selection"
                readonly property bool tabBusy: isRecap
                    ? (companion.busy && companion.feature === "recap")
                    : panel.activeSelectionBusy
                readonly property string displayAnswer: isRecap ? panel.recapDisplayText : panel.activeSelectionAnswer
                readonly property string displayWhere: isRecap ? panel.recapDisplayWhere : panel.activeSelectionWhere
                readonly property bool displayWhereLocal: isRecap ? panel.recapDisplayWhereLocal : panel.activeSelectionWhereLocal
                readonly property string tabProblem: {
                    if (isRecap && companion.feature === "recap")
                        return companion.problem
                    if (isSelection && ["define", "explain", "translate"].indexOf(companion.feature) >= 0)
                        return companion.problem
                    return ""
                }

                // Recap / Study Brief header.
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: proseColumn.isRecap
                    spacing: 3
                    Label {
                        Layout.fillWidth: true
                        text: companion.studyMode ? qsTr("Study brief") : panel.leftOffLabel()
                        color: theme.ink4
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.45
                        font.capitalization: Font.AllUppercase
                    }
                    Label {
                        Layout.fillWidth: true
                        text: companion.studyMode && companion.scope !== "upto"
                              ? (reader.title ? qsTr("Whole document · %1").arg(reader.title) : qsTr("Whole document"))
                              : qsTr("%1 · %2%").arg(panel.placeLabel(reader.chapter)).arg(Math.round(reader.progress * 100))
                        color: theme.ink2
                        font.pixelSize: 13
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }
                }

                // Unconfigured prompt on Previously tab when user earlier chose "Not now".
                UnconfiguredCard {
                    visible: proseColumn.isRecap && !ai.hasKey && !ai.hasLocal && proseColumn.displayAnswer === "" && !proseColumn.tabBusy
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: proseColumn.isSelection && panel.activeSelectionFeature === "translate"
                    spacing: 8
                    Label {
                        text: qsTr("Translate into")
                        color: theme.ink3
                        font.pixelSize: 12
                    }
                    ComboBox {
                        id: languagePicker
                        Layout.fillWidth: true
                        model: companion.targetLanguages
                        currentIndex: Math.max(0, model.indexOf(companion.targetLanguage))
                        onActivated: (index) => {
                            const chosen = model[index]
                            if (chosen && chosen !== companion.targetLanguage) {
                                companion.targetLanguage = chosen
                                companion.retranslate()
                            }
                        }
                    }
                }

                // Source passage blockquote card for selection actions.
                Rectangle {
                    Layout.fillWidth: true
                    visible: proseColumn.isSelection && panel.activeSelectionSubject !== ""
                    implicitHeight: quoteLabel.implicitHeight + 18
                    radius: Ui.radius(8, theme.radius)
                    color: Ui.tint(theme.ink1, 0.03)
                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        anchors.topMargin: 8
                        anchors.bottomMargin: 8
                        anchors.leftMargin: 8
                        width: 2
                        radius: 1
                        color: theme.accentSoft
                    }
                    Label {
                        id: quoteLabel
                        anchors.fill: parent
                        anchors.leftMargin: 18
                        anchors.rightMargin: 12
                        anchors.topMargin: 9
                        anchors.bottomMargin: 9
                        text: "“" + panel.activeSelectionSubject + "”"
                        color: theme.ink3
                        font.pixelSize: 14
                        font.italic: true
                        font.family: "Literata"
                        wrapMode: Text.Wrap
                        maximumLineCount: 6
                        elide: Text.ElideRight
                        lineHeight: 1.45
                    }
                }

                // Clean re-trigger prompt if user stopped Recap/Brief before it finished.
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: proseColumn.isRecap && (ai.hasKey || ai.hasLocal)
                             && !proseColumn.tabBusy && proseColumn.tabProblem === "" && proseColumn.displayAnswer === ""
                    spacing: 10
                    Label {
                        Layout.fillWidth: true
                        text: companion.studyMode
                              ? qsTr("Distill core claims, methodology, key findings, and limitations from this document.")
                              : qsTr("Catch up on the story so far without reading ahead of your page.")
                        color: theme.ink3
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                        lineHeight: 1.5
                    }
                    TextButton {
                        size: "compact"
                        primary: true
                        text: companion.studyMode ? qsTr("Write brief") : qsTr("Write recap")
                        onClicked: companion.recap()
                    }
                }

                // Crisp shimmering skeleton while waiting for first streamed prose token.
                ProseSkeleton {
                    visible: proseColumn.tabBusy && panel.throttledAnswer === ""
                }

                Prose {
                    readonly property bool streaming: proseColumn.tabBusy
                    readonly property string staticRaw: streaming ? "" : proseColumn.displayAnswer
                    readonly property bool staticMath: staticRaw !== "" && companion.hasMath(staticRaw)
                    visible: streaming ? (panel.throttledAnswer !== "") : (staticRaw !== "")
                    textFormat: streaming ? panel.throttledFormat : (staticMath ? TextEdit.RichText : TextEdit.MarkdownText)
                    text: streaming ? panel.throttledRendered
                          : (staticRaw === "" ? ""
                             : (staticMath ? companion.formatDisplayHtml(staticRaw, theme.fill)
                                           : companion.sanitizeMarkdown(staticRaw)))
                }
                Working { active: proseColumn.tabBusy }
                ErrorCard {
                    message: proseColumn.tabProblem
                    showRetry: companion.canRetry
                    showSettings: companion.problemIsConfig
                    onRetryClicked: proseColumn.isRecap ? companion.refreshRecap() : companion.retry()
                    onDismissClicked: companion.clearProblem()
                }

                ProvenanceRow {
                    visible: !proseColumn.tabBusy && proseColumn.displayAnswer !== "" && proseColumn.displayWhere !== ""
                    local: proseColumn.displayWhereLocal
                    text: proseColumn.isRecap && companion.studyMode && companion.recapChapter < 0
                          ? qsTr("Whole document · %1").arg(proseColumn.displayWhere)
                          : (proseColumn.isRecap && companion.recapChapter >= 0 && companion.recapChapter !== reader.chapter
                             ? qsTr("Up to %1 · %2").arg(panel.placeLabel(companion.recapChapter)).arg(proseColumn.displayWhere)
                             : proseColumn.displayWhere)
                }

                Flow {
                    Layout.fillWidth: true
                    visible: !proseColumn.tabBusy && proseColumn.displayAnswer !== ""
                    spacing: 8
                    TextButton {
                        size: "compact"
                        text: (proseColumn.isRecap ? panel.recapCopied : panel.selectionCopied) ? qsTr("Copied ✓") : qsTr("Copy")
                        onClicked: {
                            reader.copyText(proseColumn.displayAnswer)
                            if (proseColumn.isRecap) {
                                panel.recapCopied = true
                                recapCopiedTimer.restart()
                            } else {
                                panel.selectionCopied = true
                                selectionCopiedTimer.restart()
                            }
                            toast.show(qsTr("Copied"))
                        }
                    }
                    SpeakButton {
                        itemId: proseColumn.isRecap ? "recap" : "selection"
                        responseText: proseColumn.displayAnswer
                    }
                    TextButton {
                        size: "compact"
                        text: qsTr("Save as note")
                        onClicked: {
                            const quote = proseColumn.isRecap
                                ? (companion.studyMode
                                   ? qsTr("Study brief (%1)").arg(reader.title || panel.placeLabel(reader.chapter))
                                   : qsTr("Previously… (%1)").arg(panel.placeLabel(reader.chapter)))
                                : (panel.activeSelectionSubject || panel.placeLabel(reader.chapter))
                            panel.saveAsNote(quote, proseColumn.displayAnswer)
                        }
                    }
                    TextButton {
                        size: "compact"
                        visible: proseColumn.isRecap
                        text: qsTr("Refresh")
                        onClicked: companion.refreshRecap()
                    }
                    TextButton {
                        size: "compact"
                        ghost: true
                        visible: proseColumn.isSelection && (ai.hasKey || ai.hasLocal)
                        text: qsTr("Ask follow-up")
                        onClicked: panel.askAboutSelection()
                    }
                }
            }
        }

        // --- Pictures ------------------------------------------------------
        ListView {
            id: pictureList
            visible: !panel.choosing && panel.tab === "pictures"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            topMargin: 16
            bottomMargin: 16
            clip: true
            spacing: 16
            model: (!panel.choosing && panel.tab === "pictures") ? illustrator.pictures : []
            boundsBehavior: Flickable.StopAtBounds
            maximumFlickVelocity: 4500
            flickDeceleration: 2200
            pixelAligned: true
            ScrollBar.vertical: ScrollBar {}
            SmoothWheel { stepSize: 115 }

            header: ColumnLayout {
                width: ListView.view.width
                spacing: 12

                // Style chips (preserved cleanly above hero/gallery).
                Flow {
                    Layout.fillWidth: true
                    spacing: 4
                    Repeater {
                        model: illustrator.styles
                        delegate: AbstractButton {
                            id: styleChip
                            required property var modelData
                            readonly property bool selected: illustrator.style === modelData.id
                            implicitHeight: 24
                            implicitWidth: styleChipLabel.implicitWidth + 11
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            onClicked: illustrator.style = modelData.id
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            background: Rectangle {
                                radius: Ui.radius(12, theme.radius)
                                color: styleChip.selected ? theme.accentSoft
                                     : styleChip.hovered ? theme.fillHover : theme.fill
                                border.width: styleChip.selected ? 1 : 0
                                border.color: theme.accent
                                Behavior on color { enabled: !theme.reducedMotion; ColorAnimation { duration: 90 } }
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -3
                                    visible: styleChip.visualFocus
                                    radius: Ui.radius(15, theme.radius)
                                    color: "transparent"
                                    border.width: 2
                                    border.color: theme.accent
                                }
                            }
                            contentItem: Label {
                                id: styleChipLabel
                                text: styleChip.modelData.label
                                color: styleChip.selected ? theme.accentInk : theme.ink3
                                font.pixelSize: 11
                                font.weight: styleChip.selected ? Font.Medium : Font.Normal
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }

                // 4:3 dashed empty state card when no illustrations exist for this book yet.
                Item {
                    visible: illustrator.pictures.length === 0 && !illustrator.busy
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.round(width * 0.75)

                    Rectangle {
                        anchors.fill: parent
                        radius: Ui.radius(12, theme.radius)
                        color: Ui.tint(theme.ink1, 0.025)
                    }

                    Canvas {
                        id: dashedFrame
                        anchors.fill: parent
                        readonly property real corner: Ui.radius(12, theme.radius)
                        readonly property color strokeColor: Ui.tint(theme.ink1, 0.24)
                        onVisibleChanged: if (visible) requestPaint()
                        onWidthChanged: if (visible) requestPaint()
                        onHeightChanged: if (visible) requestPaint()
                        onCornerChanged: if (visible) requestPaint()
                        onStrokeColorChanged: if (visible) requestPaint()
                        onPaint: {
                            const g = getContext("2d")
                            g.reset()
                            const w = width - 1
                            const h = height - 1
                            const r = Math.min(corner, w / 2, h / 2)
                            g.strokeStyle = strokeColor
                            g.lineWidth = 1
                            g.setLineDash([5, 4])
                            g.beginPath()
                            g.moveTo(0.5 + r, 0.5)
                            g.lineTo(0.5 + w - r, 0.5)
                            if (r > 0) g.arcTo(0.5 + w, 0.5, 0.5 + w, 0.5 + r, r)
                            g.lineTo(0.5 + w, 0.5 + h - r)
                            if (r > 0) g.arcTo(0.5 + w, 0.5 + h, 0.5 + w - r, 0.5 + h, r)
                            g.lineTo(0.5 + r, 0.5 + h)
                            if (r > 0) g.arcTo(0.5, 0.5 + h, 0.5, 0.5 + h - r, r)
                            g.lineTo(0.5, 0.5 + r)
                            if (r > 0) g.arcTo(0.5, 0.5, 0.5 + r, 0.5, r)
                            g.closePath()
                            g.stroke()
                        }
                    }

                    ColumnLayout {
                        anchors.centerIn: parent
                        width: parent.width - 40
                        spacing: 8
                        Glyph {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 28
                            Layout.preferredHeight: 28
                            stroke: 1.5
                            path: Icons.picture
                            color: theme.ink3
                        }
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: qsTr("Picture this scene")
                            color: theme.ink2
                            font.pixelSize: 13
                            font.weight: Font.Medium
                        }
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: qsTr("Drawn from the page you’re on, or from words you select and right-click.")
                            color: theme.ink3
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                            lineHeight: 1.45
                        }
                    }
                }

                // 4:3 shimmering generating preview card while an illustration is in progress.
                Item {
                    visible: illustrator.busy
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.round(width * 0.75)

                    Skeleton {
                        anchors.fill: parent
                        radius: Ui.radius(12, theme.radius)
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: Ui.radius(12, theme.radius)
                        color: "transparent"
                        border.width: 1
                        border.color: theme.hairline
                    }
                    ColumnLayout {
                        anchors.centerIn: parent
                        width: parent.width - 40
                        spacing: 10
                        BusyIndicator {
                            Layout.alignment: Qt.AlignHCenter
                            running: illustrator.busy
                            implicitWidth: 24
                            implicitHeight: 24
                        }
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: illustrator.status || qsTr("Drawing…")
                            color: theme.ink2
                            font.pixelSize: 13
                            font.weight: Font.Medium
                            wrapMode: Text.Wrap
                        }
                        TextButton {
                            Layout.alignment: Qt.AlignHCenter
                            size: "compact"
                            text: qsTr("Stop")
                            onClicked: illustrator.stop()
                        }
                    }
                }

                // Full-width 34px accent CTA with embedded cost (hidden when unavailable notice shows its own primary CTA).
                TextButton {
                    Layout.fillWidth: true
                    visible: illustrator.available && illustrator.budgetOk
                    primary: true
                    size: "large"
                    enabled: !illustrator.busy
                    text: illustrator.cost > 0
                          ? qsTr("Illustrate · about %1").arg(panel.formatCents(illustrator.cost))
                          : qsTr("Illustrate this page")
                    onClicked: illustrator.illustrate("", reader.chapter, reader.pageEnd)
                }

                // Crystal-clear notice when unavailable (local-only mode, missing key, or budget reached).
                Rectangle {
                    Layout.fillWidth: true
                    visible: (!illustrator.available || !illustrator.budgetOk) && illustrator.problem === ""
                    implicitHeight: unavailCol.implicitHeight + 20
                    radius: Ui.radius(10, theme.radius)
                    color: Ui.tint(theme.ink1, 0.04)
                    border.width: 1
                    border.color: theme.hairline
                    ColumnLayout {
                        id: unavailCol
                        anchors.fill: parent
                        anchors.margins: 10
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            text: illustrator.unavailableReason
                            color: theme.ink3
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                            lineHeight: 1.45
                        }
                        TextButton {
                            size: "compact"
                            primary: true
                            text: qsTr("Open Intelligence settings")
                            onClicked: panel.openIntelligenceSettings()
                        }
                    }
                }

                ErrorCard {
                    message: illustrator.problem
                    showRetry: illustrator.canRetry && illustrator.available && illustrator.budgetOk
                    showSettings: illustrator.problemIsConfig || !illustrator.available || !illustrator.budgetOk
                    onRetryClicked: illustrator.retryLast()
                    onDismissClicked: illustrator.clearProblem()
                }

                Item { Layout.preferredHeight: illustrator.pictures.length > 0 ? 4 : 0 }
            }

            delegate: ColumnLayout {
                id: picItem
                required property var modelData
                property bool confirmRemove: false
                readonly property bool fileMissing: modelData.exists === false || pictureImage.status === Image.Error
                readonly property int picRadius: Ui.radius(12, theme.radius)
                width: ListView.view.width
                spacing: 10

                Timer {
                    id: removeResetTimer
                    interval: 3500
                    onTriggered: picItem.confirmRemove = false
                }

                Item {
                    id: picFrame
                    visible: !picItem.fileMissing
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.round(width * 0.75)

                    Item {
                        anchors.fill: parent
                        layer.enabled: picItem.picRadius > 0
                        layer.effect: MultiEffect {
                            maskEnabled: true
                            maskSource: picMask
                            maskThresholdMin: 0.5
                            maskSpreadAtMin: 1.0
                        }
                        Image {
                            id: pictureImage
                            anchors.fill: parent
                            source: picItem.modelData.exists === false ? "" : picItem.modelData.url
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            sourceSize.width: 768
                            mipmap: true
                            smooth: true
                        }
                    }
                    Item {
                        id: picMask
                        anchors.fill: parent
                        visible: false
                        layer.enabled: true
                        Rectangle {
                            anchors.fill: parent
                            radius: picItem.picRadius
                        }
                    }
                    Rectangle {
                        anchors.fill: parent
                        radius: picItem.picRadius
                        color: "transparent"
                        border.width: 1
                        border.color: picMouse.containsMouse ? Ui.tint(theme.ink1, 0.22) : theme.hairline
                        Behavior on border.color { enabled: !theme.reducedMotion; ColorAnimation { duration: 110 } }
                    }
                    MouseArea {
                        id: picMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: picturePreview.show(picItem.modelData)
                        ToolTip.visible: containsMouse
                        ToolTip.delay: 500
                        ToolTip.text: qsTr("Click to view full size")
                    }
                }

                Rectangle {
                    visible: picItem.fileMissing
                    Layout.fillWidth: true
                    Layout.preferredHeight: 112
                    radius: picItem.picRadius
                    color: theme.fill
                    border.width: 1
                    border.color: theme.hairline
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 4
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Picture file is missing")
                            color: theme.ink3
                            font.pixelSize: 13
                            font.weight: Font.Medium
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Use Another take to draw it again.")
                            color: theme.ink4
                            font.pixelSize: 12
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: !!picItem.modelData.caption
                    text: picItem.modelData.caption
                    color: theme.quote
                    font.pixelSize: 13
                    font.italic: true
                    font.family: "Literata"
                    lineHeight: 1.5
                    wrapMode: Text.Wrap
                }

                ProvenanceRow {
                    local: false
                    text: {
                        const base = picItem.modelData.where || illustrator.where
                        const place = panel.placeLabel(picItem.modelData.chapter)
                        return picItem.modelData.style && picItem.modelData.style !== "auto" && picItem.modelData.styleLabel
                             ? qsTr("%1 · %2 · %3").arg(base).arg(place).arg(picItem.modelData.styleLabel)
                             : qsTr("%1 · %2").arg(base).arg(place)
                    }
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 8
                    TextButton {
                        size: "normal"
                        enabled: !picItem.fileMissing
                        text: qsTr("Save a copy")
                        onClicked: illustrator.chooseSaveCopy(picItem.modelData.id)
                    }
                    TextButton {
                        size: "normal"
                        enabled: illustrator.available && illustrator.budgetOk && !illustrator.busy
                        text: qsTr("Another take")
                        onClicked: illustrator.regenerate(picItem.modelData.id)
                    }
                    TextButton {
                        size: "compact"
                        ghost: true
                        text: qsTr("Go to %1").arg(panel.placeLabel(picItem.modelData.chapter))
                        onClicked: panel.jumpRequested(picItem.modelData.chapter, picItem.modelData.offset || 0)
                    }
                    TextButton {
                        size: "compact"
                        ghost: !picItem.confirmRemove
                        text: picItem.confirmRemove ? qsTr("Remove?") : qsTr("Remove")
                        onClicked: {
                            if (picItem.confirmRemove) {
                                removeResetTimer.stop()
                                picItem.confirmRemove = false
                                illustrator.remove(picItem.modelData.id)
                            } else {
                                picItem.confirmRemove = true
                                removeResetTimer.restart()
                            }
                        }
                    }
                }
            }
        }

        // --- Who's who -----------------------------------------------------
        ListView {
            visible: !panel.choosing && panel.tab === "whoswho"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            topMargin: 10
            bottomMargin: 10
            clip: true
            spacing: 0
            model: (!panel.choosing && panel.tab === "whoswho") ? companion.people : []
            boundsBehavior: Flickable.StopAtBounds
            maximumFlickVelocity: 4500
            flickDeceleration: 2200
            pixelAligned: true
            ScrollBar.vertical: ScrollBar {}
            SmoothWheel { stepSize: 115 }

            header: ColumnLayout {
                width: ListView.view.width
                spacing: 8

                UnconfiguredCard {
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    visible: !ai.hasKey && !ai.hasLocal && companion.people.length === 0 && !companion.busy
                }

                Working {
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    active: panel.tab === "whoswho" && companion.feature === "whoswho"
                }

                // Shimmering cast skeleton on initial scan.
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    visible: panel.tab === "whoswho" && companion.feature === "whoswho" && companion.busy && companion.people.length === 0
                    spacing: 14
                    Repeater {
                        model: 4
                        delegate: RowLayout {
                            Layout.fillWidth: true
                            spacing: 12
                            Skeleton { Layout.preferredWidth: 32; Layout.preferredHeight: 32; radius: 16 }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 6
                                Skeleton { Layout.preferredWidth: 110; Layout.preferredHeight: 12 }
                                Skeleton { Layout.fillWidth: true; Layout.preferredHeight: 11 }
                                Skeleton { Layout.fillWidth: true; Layout.rightMargin: 44; Layout.preferredHeight: 11 }
                            }
                        }
                    }
                }

                ErrorCard {
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    message: panel.tab === "whoswho" && companion.feature === "whoswho" ? companion.problem : ""
                    showRetry: companion.canRetry
                    showSettings: companion.problemIsConfig
                    onRetryClicked: companion.refreshWhosWho()
                    onDismissClicked: companion.clearProblem()
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    Layout.topMargin: 6
                    visible: (ai.hasKey || ai.hasLocal) && companion.peopleLoaded
                             && !companion.busy && companion.problem === "" && companion.people.length === 0
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: companion.studyMode
                              ? qsTr("No symbols, acronyms, or glossary terms found yet.")
                              : qsTr("No named characters found up to %1.").arg(panel.placeLabel(reader.chapter))
                        color: theme.ink3
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }
                    TextButton {
                        size: "compact"
                        text: qsTr("Scan again")
                        onClicked: companion.refreshWhosWho()
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    Layout.topMargin: 6
                    visible: (ai.hasKey || ai.hasLocal) && !companion.peopleLoaded
                             && !companion.busy && companion.problem === "" && companion.people.length === 0
                    spacing: 10
                    Label {
                        Layout.fillWidth: true
                        text: companion.studyMode
                              ? qsTr("Extract mathematical symbols, acronyms, and key terms with jump links to where each is first defined.")
                              : qsTr("Build a spoiler-free cast list of everyone introduced up to %1.").arg(panel.placeLabel(reader.chapter))
                        color: theme.ink3
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                        lineHeight: 1.5
                    }
                    TextButton {
                        size: "compact"
                        primary: true
                        text: companion.studyMode ? qsTr("Build glossary") : qsTr("Scan characters")
                        onClicked: companion.whosWho()
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    Layout.bottomMargin: 4
                    visible: companion.people.length > 0 && !companion.busy
                    spacing: 8
                    ProvenanceRow {
                        local: companion.peopleWhereLocal
                        readonly property bool isWholeDoc: companion.studyMode && companion.peopleChapter < 0
                        readonly property string place: panel.placeLabel(companion.peopleChapter >= 0 ? companion.peopleChapter : reader.chapter)
                        text: isWholeDoc
                              ? (companion.peopleWhere ? qsTr("Whole document · %1").arg(companion.peopleWhere) : qsTr("Whole document"))
                              : (companion.peopleWhere ? qsTr("Up to %1 · %2").arg(place).arg(companion.peopleWhere) : qsTr("Up to %1").arg(place))
                    }
                    TextButton {
                        size: "compact"
                        ghost: true
                        text: qsTr("Refresh")
                        onClicked: companion.refreshWhosWho()
                    }
                }
            }

            delegate: ColumnLayout {
                id: personItem
                required property var modelData
                required property int index
                readonly property bool symbolBadge: panel.isMathSymbolBadge(personItem.modelData.name)
                readonly property bool serifSymbol: symbolBadge && panel.hasNonAsciiSymbol(personItem.modelData.name)
                width: ListView.view.width
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    Layout.topMargin: 10
                    Layout.bottomMargin: 10
                    spacing: 12

                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        radius: companion.studyMode ? Ui.radius(8, theme.radius) : 16
                        color: panel.personTint(personItem.modelData.name)
                        Label {
                            anchors.centerIn: parent
                            width: parent.width - 4
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            text: personItem.symbolBadge
                                  ? String(personItem.modelData.name || "").trim()
                                  : panel.personInitials(personItem.modelData.name)
                            color: theme.ink1
                            font.family: personItem.serifSymbol ? "Literata" : ""
                            font.italic: personItem.serifSymbol
                            font.pixelSize: personItem.serifSymbol ? 15
                                          : (personItem.symbolBadge && String(personItem.modelData.name || "").trim().length >= 3 ? 9 : 11)
                            font.weight: personItem.serifSymbol ? Font.Medium : Font.DemiBold
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        Label {
                            Layout.fillWidth: true
                            text: personItem.modelData.name + (personItem.modelData.also ? " · " + personItem.modelData.also : "")
                            color: theme.ink1
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            readonly property string defRaw: personItem.modelData.who || ""
                            readonly property bool defMath: companion.hasMath(defRaw)
                            textFormat: defMath ? Text.RichText : Text.PlainText
                            text: defMath ? companion.formatDisplayHtml(defRaw, theme.fill) : defRaw
                            color: theme.ink3
                            font.pixelSize: 13
                            wrapMode: Text.Wrap
                            lineHeight: 1.45
                        }
                        JumpChip {
                            Layout.topMargin: 3
                            visible: personItem.modelData.chapterIndex !== undefined && personItem.modelData.chapterIndex >= 0
                            text: (companion.studyMode ? qsTr("First defined: %1") : qsTr("First seen: %1"))
                                  .arg(personItem.modelData.chapter || panel.placeLabel(personItem.modelData.chapterIndex))
                            tipText: companion.studyMode ? qsTr("Jump to where this first appears") : qsTr("Jump to first appearance")
                            onClicked: panel.jumpRequested(personItem.modelData.chapterIndex, personItem.modelData.offset || 0)
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: !(personItem.modelData.chapterIndex !== undefined && personItem.modelData.chapterIndex >= 0)
                                     && !!personItem.modelData.chapter
                            text: (companion.studyMode ? qsTr("First defined: %1") : qsTr("First seen: %1"))
                                  .arg(personItem.modelData.chapter || "")
                            color: theme.ink4
                            font.pixelSize: 11
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    Layout.preferredHeight: 1
                    visible: personItem.index < companion.people.length - 1
                    color: Ui.tint(theme.ink1, 0.05)
                }
            }
        }

        // --- Shared bottom composer (always reachable across tabs) ----------
        Rectangle {
            visible: !panel.choosing
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: theme.hairline
        }
        ColumnLayout {
            visible: !panel.choosing
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.topMargin: 10
            Layout.bottomMargin: 14
            spacing: 7

            // Sleek, minimal 3-option scope pill in the Ask tab.
            Segmented {
                visible: panel.tab === "ask"
                Layout.fillWidth: true
                Layout.preferredHeight: 26
                compact: true
                model: [
                    { value: "upto", label: qsTr("Up to here") },
                    { value: "book", label: qsTr("Whole book") },
                    { value: "shelf", label: qsTr("This shelf") }
                ]
                current: companion.scope
                onActivated: (value) => companion.scope = value
            }

            // Quiet pending visual snip attachment bar when waiting for a custom question.
            Rectangle {
                visible: companion.hasPendingSnip
                Layout.fillWidth: true
                implicitHeight: pendingSnipRow.implicitHeight + 12
                radius: Ui.radius(9, theme.radius)
                color: Ui.tint(theme.ink1, 0.04)
                border.width: 1
                border.color: theme.hairline
                RowLayout {
                    id: pendingSnipRow
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 6
                    anchors.topMargin: 6
                    anchors.bottomMargin: 6
                    spacing: 8
                    Rectangle {
                        Layout.preferredWidth: 44
                        Layout.preferredHeight: 32
                        radius: Ui.radius(5, theme.radius)
                        color: theme.drawer
                        border.width: 1
                        border.color: theme.hairline
                        clip: true
                        Image {
                            anchors.fill: parent
                            anchors.margins: 2
                            source: companion.pendingSnipUrl
                            fillMode: Image.PreserveAspectFit
                            asynchronous: true
                            sourceSize.width: 96
                            sourceSize.height: 72
                            mipmap: true
                            smooth: true
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        text: companion.pendingSnipPage >= 0
                              ? qsTr("Snipped region · %1").arg(panel.placeLabel(companion.pendingSnipPage))
                              : qsTr("Snipped region attached")
                        color: theme.ink2
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }
                    IconButton {
                        implicitWidth: 24
                        implicitHeight: 24
                        glyph: Icons.close
                        tone: theme.ink3
                        tip: qsTr("Remove snipped image")
                        onClicked: companion.clearPendingSnip()
                    }
                }
            }

            // Quiet pre-send cost transparency line while composing.
            ProvenanceRow {
                visible: (question.text.trim() !== "" || companion.hasPendingSnip) && !!panel.askEstimateInfo.where && (ai.hasKey || ai.hasLocal)
                local: !!panel.askEstimateInfo.local
                text: !panel.askEstimateInfo.where ? ""
                    : panel.askEstimateInfo.local
                      ? (companion.scope === "shelf"
                         ? qsTr("%1 · passages across this shelf").arg(panel.askEstimateInfo.where)
                         : companion.scope === "book"
                         ? qsTr("%1 · passages across the whole book").arg(panel.askEstimateInfo.where)
                         : qsTr("%1 · passages found in the book so far").arg(panel.askEstimateInfo.where))
                      : (companion.scope === "shelf"
                         ? qsTr("%1 · matching passages across this shelf · %2").arg(panel.askEstimateInfo.where).arg(panel.formatCents(panel.askEstimateInfo.cost))
                         : qsTr("%1 · about %Ln words · %2", "", panel.askEstimateInfo.words)
                               .arg(panel.askEstimateInfo.where).arg(panel.formatCents(panel.askEstimateInfo.cost)))
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(140, Math.max(40, question.implicitHeight + 20))
                radius: Ui.radius(12, theme.radius)
                color: Ui.tint(theme.ink1, 0.06)
                border.width: 1
                border.color: question.activeFocus ? theme.accent : theme.hairline
                Behavior on border.color { enabled: !theme.reducedMotion; ColorAnimation { duration: 100 } }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 6
                    spacing: 8

                    Flickable {
                        id: questionScroll
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.topMargin: 10
                        Layout.bottomMargin: 10
                        clip: true
                        contentWidth: width
                        contentHeight: question.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds

                        TextEdit {
                            id: question
                            width: parent.width
                            wrapMode: Text.Wrap
                            color: theme.ink1
                            selectionColor: theme.accentSoft
                            font.pixelSize: 13
                            selectByMouse: true
                            Accessible.role: Accessible.EditableText
                            Accessible.name: questionPlaceholder.text
                            onCursorRectangleChanged: {
                                if (cursorRectangle.y < questionScroll.contentY)
                                    questionScroll.contentY = cursorRectangle.y
                                else if (cursorRectangle.y + cursorRectangle.height > questionScroll.contentY + questionScroll.height)
                                    questionScroll.contentY = cursorRectangle.y + cursorRectangle.height - questionScroll.height
                            }
                            Keys.onReturnPressed: (event) => {
                                if (event.modifiers & Qt.ShiftModifier)
                                    event.accepted = false
                                else
                                    panel.send()
                            }
                            Keys.onEnterPressed: (event) => {
                                if (event.modifiers & Qt.ShiftModifier)
                                    event.accepted = false
                                else
                                    panel.send()
                            }
                            Keys.onEscapePressed: {
                                if (panel.clearConfirmOpen)
                                    panel.clearConfirmOpen = false
                                else
                                    panel.close()
                            }

                            Text {
                                id: questionPlaceholder
                                anchors.fill: parent
                                visible: question.text === ""
                                textFormat: Text.PlainText
                                text: (!ai.hasKey && !ai.hasLocal)
                                      ? qsTr("Set up a model to ask about the book…")
                                      : companion.hasPendingSnip
                                      ? qsTr("Ask about this snipped region…")
                                      : companion.scope === "shelf"
                                      ? (companion.shelfLabel !== "" ? qsTr("Ask across %1…").arg(companion.shelfLabel) : qsTr("Ask across this shelf…"))
                                      : companion.scope === "book"
                                      ? (companion.studyMode ? qsTr("Ask about this document…") : qsTr("Ask about the whole book…"))
                                      : qsTr("Ask about the book so far…")
                                color: theme.ink4
                                font: question.font
                                elide: Text.ElideRight
                            }
                        }
                    }

                    // Send button smoothly becomes Stop mid-stream so cancel is always one click away.
                    AbstractButton {
                        id: sendBtn
                        readonly property bool stopping: companion.busy && companion.feature === "ask"
                        Layout.alignment: Qt.AlignBottom
                        Layout.bottomMargin: 6
                        implicitWidth: 28
                        implicitHeight: 28
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        enabled: stopping || ((question.text.trim() !== "" || companion.hasPendingSnip) && !companion.busy)
                        opacity: enabled ? 1 : 0.35
                        Accessible.name: stopping ? qsTr("Stop") : qsTr("Ask")
                        onClicked: stopping ? companion.stop() : panel.send()
                        HoverHandler { enabled: sendBtn.enabled; cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: stopping ? qsTr("Stop generating") : qsTr("Ask (Enter)")
                        background: Rectangle {
                            radius: Ui.radius(7, theme.radius)
                            color: sendBtn.stopping
                                 ? (sendBtn.pressed ? Ui.tint(theme.ink1, 0.24)
                                    : sendBtn.hovered ? Ui.tint(theme.ink1, 0.18) : Ui.tint(theme.ink1, 0.13))
                                 : (sendBtn.enabled && sendBtn.pressed ? Qt.darker(theme.accent, 1.12)
                                    : sendBtn.enabled && sendBtn.hovered ? Qt.lighter(theme.accent, 1.06)
                                    : theme.accent)
                            Behavior on color { enabled: !theme.reducedMotion; ColorAnimation { duration: 100 } }
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -3
                                visible: sendBtn.visualFocus
                                radius: Ui.radius(10, theme.radius)
                                color: "transparent"
                                border.width: 2
                                border.color: theme.accent
                            }
                        }
                        contentItem: Item {
                            Glyph {
                                visible: !sendBtn.stopping
                                anchors.centerIn: parent
                                width: 14
                                height: 14
                                stroke: 2.4
                                path: Icons.arrowUp
                                color: theme.accentOn
                            }
                            Rectangle {
                                visible: sendBtn.stopping
                                anchors.centerIn: parent
                                width: 10
                                height: 10
                                radius: 2
                                color: theme.ink1
                            }
                        }
                    }
                }
            }
        }
    }

    // Inline full-size picture preview modal (renders over the whole window).
    Popup {
        id: picturePreview
        property var item: null
        function show(modelData) {
            item = modelData
            open()
        }
        onClosed: item = null
        parent: Overlay.overlay
        x: 0
        y: 0
        width: Overlay.overlay ? Overlay.overlay.width : 800
        height: Overlay.overlay ? Overlay.overlay.height : 600
        modal: true
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        // Always-dark modal palette so buttons stay razor-sharp in light AND dark themes.
        palette.windowText: "#f5f5f7"
        palette.buttonText: "#f5f5f7"
        palette.mid: "#b8bcc8"
        palette.highlight: theme.accent
        palette.highlightedText: theme.accentOn

        enter: Transition {
            enabled: !theme.reducedMotion
            NumberAnimation { property: "opacity"; from: 0.0; to: 1.0; duration: 160; easing.type: Easing.OutCubic }
        }
        exit: Transition {
            enabled: !theme.reducedMotion
            NumberAnimation { property: "opacity"; from: 1.0; to: 0.0; duration: 120; easing.type: Easing.InCubic }
        }

        Connections {
            target: illustrator
            function onPicturesChanged() {
                if (picturePreview.opened && picturePreview.item) {
                    const id = picturePreview.item.id
                    if (!illustrator.pictures.some(p => p.id === id))
                        picturePreview.close()
                }
            }
        }

        background: Rectangle {
            color: Ui.tint("#000000", 0.86)
            TapHandler { onTapped: picturePreview.close() }
        }

        contentItem: Item {
            anchors.fill: parent
            TapHandler { onTapped: picturePreview.close() }

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 32
                spacing: 14

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: picturePreview.item && picturePreview.item.styleLabel
                              ? qsTr("%1 · %2").arg(panel.placeLabel(picturePreview.item.chapter)).arg(picturePreview.item.styleLabel)
                              : picturePreview.item ? panel.placeLabel(picturePreview.item.chapter) : ""
                        color: "white"
                        opacity: 0.85
                        font.pixelSize: 13
                    }
                    IconButton {
                        glyph: Icons.close
                        tone: "white"
                        tip: qsTr("Close (Esc)")
                        onClicked: picturePreview.close()
                    }
                }
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Image {
                        id: fullPreviewImage
                        anchors.fill: parent
                        source: picturePreview.opened && picturePreview.item ? picturePreview.item.url : ""
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        sourceSize.width: Math.max(256, Math.min(2048, Math.round(width * (Screen.devicePixelRatio || 1))))
                        sourceSize.height: Math.max(256, Math.min(2048, Math.round(height * (Screen.devicePixelRatio || 1))))
                        mipmap: true
                        smooth: true
                    }
                }
                Label {
                    Layout.fillWidth: true
                    Layout.maximumWidth: 720
                    Layout.alignment: Qt.AlignHCenter
                    visible: !!picturePreview.item && !!picturePreview.item.caption
                    text: picturePreview.item ? picturePreview.item.caption : ""
                    color: "white"
                    font.pixelSize: 15
                    font.italic: true
                    font.family: "Literata"
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 10
                    TextButton {
                        text: picturePreview.item ? qsTr("Go to %1").arg(panel.placeLabel(picturePreview.item.chapter)) : ""
                        visible: !!picturePreview.item
                        onClicked: {
                            const target = picturePreview.item
                            picturePreview.close()
                            if (target)
                                panel.jumpRequested(target.chapter, target.offset || 0)
                        }
                    }
                    TextButton {
                        visible: !!picturePreview.item
                        enabled: illustrator.available && illustrator.budgetOk && !illustrator.busy
                        text: qsTr("Another take")
                        onClicked: {
                            const target = picturePreview.item
                            picturePreview.close()
                            if (target)
                                illustrator.regenerate(target.id)
                        }
                    }
                    TextButton {
                        visible: !!picturePreview.item
                        enabled: fullPreviewImage.status !== Image.Error
                        text: qsTr("Save a copy")
                        onClicked: {
                            if (picturePreview.item)
                                illustrator.chooseSaveCopy(picturePreview.item.id)
                        }
                    }
                }
            }
        }
    }
}
