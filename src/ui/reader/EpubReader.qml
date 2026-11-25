import QtQuick
import QtQuick.Controls
import QtWebChannel
import QtWebEngine
import "../audio"
import "../components"

import "../components/Icons.js" as Icons
import "../components/Themes.js" as Themes
import "../components/Ui.js" as Ui

// Reading a book: the page edge to edge in the reading theme, a quiet
// running head and folio, and everything else one key away. The page itself
// is the web surface (reader.html), driven through `clothbound`.
FocusScope {
    id: view

    property var settings
    // Effective appearance: either this book's custom override (stored in
    // book_settings) or the global reading defaults.
    QtObject {
        id: activeSettings
        function pick(key, fallback) {
            return reader.bookAppearanceOverride && reader.bookAppearance[key] !== undefined
                ? reader.bookAppearance[key] : fallback
        }
        readonly property string theme: String(pick("theme", view.settings.theme))
        readonly property string font: String(pick("font", view.settings.font))
        readonly property real fontScale: Number(pick("fontScale", view.settings.fontScale))
        readonly property int fontWeight: Math.round(Number(pick("fontWeight", reader.fontWeight)))
        readonly property real lineHeight: Number(pick("lineHeight", view.settings.lineHeight))
        readonly property real paragraphSpacing: Number(pick("paragraphSpacing", reader.paragraphSpacing))
        readonly property string margins: String(pick("margins", view.settings.margins))
        readonly property bool justify: !!pick("justify", view.settings.justify)
        readonly property bool hyphenate: !!pick("hyphenate", view.settings.hyphenate)
        readonly property bool dropCap: !!pick("dropCap", view.settings.dropCap)
        readonly property string layout: String(pick("layout", view.settings.layout))
        readonly property bool animate: !!pick("animate", view.settings.animate)
        readonly property bool publisher: !!pick("publisher", view.settings.publisher)
        readonly property bool pdfCrop: !!pick("pdfCrop", view.settings.pdfCrop)
        readonly property bool pdfOriginalColors: !!pick("pdfOriginalColors", view.settings.pdfOriginalColors)
        readonly property string pdfFit: String(pick("pdfFit", view.settings.pdfFit || "page"))
    }

    readonly property var look: Themes.palette(activeSettings.theme, theme)
    // For driving the panels from outside (CommandPalette and tests).
    readonly property alias contents: panels.contents
    readonly property alias contentsPanel: panels.contents
    readonly property alias appearance: panels.appearance
    readonly property alias appearancePanel: panels.appearance
    readonly property alias goTo: panels.goTo
    readonly property alias goToBox: panels.goTo
    readonly property alias voicePanel: panels.voicePanel
    readonly property alias searchBar: panels.searchBar
    readonly property alias pageMenu: menus.pageMenu
    readonly property alias citeMenu: menus.citeMenu
    readonly property alias readerMore: menus.readerMore
    readonly property alias listenOffer: readerChrome.listenOffer
    // The words selected on the page, as the page last said ({} for none).
    property var selection: ({})
    property var lastCompanionTarget: null
    readonly property alias marksPanel: panels.marksPanel
    readonly property alias companionPanel: panels.companionPanel
    readonly property alias noteEditor: panels.noteEditor
    readonly property var pdfSurface: pdfLoader.item
    readonly property alias listenBar: listenBar
    // Companion docks beside the page on wide screens without a blocking scrim (04_companion).
    // PDFs track the drawer's slideProgress smoothly on the GPU (deferring crisp re-raster
    // until settled); EPUBs glide via zero-reflow GPU translation and commit one clean
    // WebEngine reflow on settle so neither surface snaps robotically.
    readonly property bool companionDocked: companionPanel.settledOpen && view.width >= 860
    readonly property bool companionSliding: companionPanel.sliding && view.width >= 860
    readonly property bool companionResizing: Boolean(companionPanel["resizing"])
    readonly property real companionTargetWidth: view.width >= 860
                                                 ? Math.max(0, Math.min(Math.max(0, view.width - 320), companionPanel.width))
                                                 : 0
    readonly property real companionLiveDockWidth: companionTargetWidth * companionPanel.slideProgress
    readonly property real companionDockWidth: companionDocked ? companionTargetWidth : 0
    readonly property bool modalPanelOpen: contents.open || appearance.open || goTo.open || voicePanel.open || searchBar.editing
                                           || marksPanel.open || noteEditor.open || (companionPanel.open && view.width < 860)
    readonly property bool panelOpen: contents.open || appearance.open || goTo.open || voicePanel.open || searchBar.editing
                                     || marksPanel.open || noteEditor.open || companionPanel.open
    property bool chromeShown: false
    property bool surfaceReady: false
    property bool listening: false
    property bool pendingStartListening: false
    property bool jumpPillVisible: false
    property string jumpPillLabel: ""
    readonly property bool firstBookTipsActive: onboarding.tips.indexOf("keys") < 0 && reader.open && !view.epubLoading
                                                && !view.panelOpen && !narration.active
    readonly property var firstBookTipsModel: view.pdf
        ? [{ key: "← →", what: qsTr("Turn") },
           { key: "P", what: qsTr("Listen") },
           { key: "C", what: qsTr("Ask") },
           { key: "X", what: qsTr("Snip") },
           { key: "Ctrl K", what: qsTr("Find") }]
        : [{ key: "← →", what: qsTr("Turn") },
           { key: "P", what: qsTr("Listen") },
           { key: "C", what: qsTr("Ask") },
           { key: "Ctrl K", what: qsTr("Find") },
           { key: "?", what: qsTr("All keys") }]
    readonly property real footerInset: Math.max(24, Math.min(144, Math.round(readingStage.width * 0.1125)))

    function propBool(obj, key) {
        return !!(obj && obj[key])
    }
    function isDescendantOf(item, ancestor) {
        let cur = item
        while (cur) {
            if (cur === ancestor)
                return true
            cur = cur.parent
        }
        return false
    }
    readonly property Item focusedItem: Window.activeFocusItem
    readonly property bool chromeFocused: focusedItem !== null && (
        isDescendantOf(focusedItem, readerChrome.chromeItem) || isDescendantOf(focusedItem, readerChrome.bottomFooterItem)
        || isDescendantOf(focusedItem, readerChrome.jumpPillItem) || isDescendantOf(focusedItem, readerChrome.followVoicePillItem)
        || isDescendantOf(focusedItem, readerChrome.keysStripItem)
    )
    readonly property bool readingViewportFocused: {
        if (panelOpen || modalOpen || win.typing || lightbox.open || listenOffer.open
            || (pdf && pdfSurface && (pdfSurface.snipMode || pdfSurface.peekOpen)))
            return false
        if (chromeFocused)
            return false
        if (focusedItem === null || focusedItem === view || focusedItem === readingStage)
            return true
        if (pdf)
            return pdfSurface !== null && isDescendantOf(focusedItem, pdfSurface)
        return web !== null && isDescendantOf(focusedItem, web)
    }
    onChromeShownChanged: {
        if (!chromeShown && chromeFocused)
            focusPage()
    }
    onChromeFocusedChanged: {
        if (!chromeFocused && chromeShown)
            chromeTimer.restart()
    }

    onCompanionDockedChanged: styleTimer.restart()

    signal closeRequested()

    // The reading theme as the palette of every panel, menu and control in
    // the reader (the roles are in src/style/Clothbound/README.md).
    palette.window: look.bg
    palette.windowText: look.fg
    palette.base: Ui.tint(look.fg, 0.06)
    palette.alternateBase: Ui.tint(look.fg, 0.10)
    palette.text: look.fg
    palette.placeholderText: Ui.mix(look.fg, look.bg, 0.45)
    palette.button: Ui.tint(look.fg, 0.06)
    palette.buttonText: look.fg
    palette.mid: look.muted
    palette.midlight: Ui.tint(look.fg, 0.10)
    palette.dark: Ui.tint(look.fg, 0.14)
    palette.light: look.popover
    palette.shadow: look.dark ? "#73000000" : "#26000000"
    palette.highlight: look.link
    palette.highlightedText: Ui.readableOn(look.link)
    palette.link: look.link
    palette.linkVisited: Ui.tint(look.link, 0.16)
    palette.toolTipBase: look.popover
    palette.toolTipText: look.fg

    readonly property bool pdf: reader.format === "pdf"
    readonly property bool epubLoading: !pdf && reader.open && (!surfaceReady || reader.relocationSeq === 0)
    // WebEngine is only instantiated once an EPUB is opened, so PDF-only
    // sessions don't pay Chromium's memory cost.
    property bool webNeeded: false
    property bool epubSurfaceOpen: false
    property bool epubClosing: false
    readonly property var web: webLoader.item

    function snapshotAppearance() {
        return {
            theme: activeSettings.theme,
            font: activeSettings.font,
            fontScale: activeSettings.fontScale,
            fontWeight: activeSettings.fontWeight,
            lineHeight: activeSettings.lineHeight,
            paragraphSpacing: activeSettings.paragraphSpacing,
            margins: activeSettings.margins,
            justify: activeSettings.justify,
            hyphenate: activeSettings.hyphenate,
            dropCap: activeSettings.dropCap,
            layout: activeSettings.layout,
            animate: activeSettings.animate,
            publisher: activeSettings.publisher,
            pdfCrop: activeSettings.pdfCrop,
            pdfOriginalColors: activeSettings.pdfOriginalColors,
            pdfFit: activeSettings.pdfFit,
        }
    }
    function setAppearance(key, value) {
        if (reader.bookAppearanceOverride) {
            const next = snapshotAppearance()
            next[key] = value
            reader.saveBookAppearance(next)
            return
        }
        if (key === "fontWeight")
            reader.fontWeight = value
        else if (key === "paragraphSpacing")
            reader.paragraphSpacing = value
        else
            view.settings[key] = value
    }
    function setBookOverride(enabled) {
        if (enabled)
            reader.saveBookAppearance(snapshotAppearance())
        else
            reader.clearBookAppearance()
    }

    Component.onCompleted: {
        if (reader.open) {
            if (pdf)
                openBook()
            else
                webNeeded = true
        }
    }

    // Typed surface dispatch: directly invokes PdfSurface methods for PDFs
    // (zero dynamic eval / string substitution) and runs JavaScript on the
    // WebEngine surface for EPUBs.
    function runWebJs(code, callback) {
        if (!web)
            return
        if (callback)
            web.runJavaScript(code, callback)
        else
            web.runJavaScript(code)
    }
    function surfaceOpen(manifest, position, styleObj) {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.open(manifest, position, styleObj)
            return
        }
        runWebJs("clothbound.open(" + JSON.stringify(manifest) + "," + JSON.stringify(position) + ","
                 + JSON.stringify(styleObj) + ")")
    }
    function surfaceSetStyle(styleObj) {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.setStyle(styleObj)
            return
        }
        runWebJs("clothbound.setStyle(" + JSON.stringify(styleObj) + ")")
    }
    function surfaceNext() {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.next()
            return
        }
        runWebJs("clothbound.next()")
    }
    function surfacePrev() {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.prev()
            return
        }
        runWebJs("clothbound.prev()")
    }
    function surfaceNextChapter() {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.nextChapter()
            return
        }
        runWebJs("clothbound.nextChapter()")
    }
    function surfacePrevChapter() {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.prevChapter()
            return
        }
        runWebJs("clothbound.prevChapter()")
    }
    function surfaceBack() {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.back()
            return
        }
        runWebJs("clothbound.back()")
    }
    function surfaceForward() {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.forward()
            return
        }
        runWebJs("clothbound.forward()")
    }
    function surfaceGoTo(target) {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.goTo(target)
            return
        }
        runWebJs("clothbound.goTo(" + JSON.stringify(target) + ")")
    }
    function surfaceClearSelection() {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.clearSelection()
            return
        }
        runWebJs("clothbound.clearSelection()")
    }
    function surfaceEscape(callback) {
        if (pdf) {
            const closed = pdfSurface ? !!pdfSurface.dismiss() : false
            if (callback)
                callback(closed)
            return
        }
        runWebJs("clothbound.escape()", callback)
    }
    function surfaceFound(hits, currentHit) {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.found(hits, currentHit)
            return
        }
        runWebJs("clothbound.found(" + JSON.stringify(hits) + "," + JSON.stringify(currentHit) + ")")
    }
    function surfaceGoToAndFound(target, hits, currentHit) {
        if (pdf) {
            if (pdfSurface) {
                pdfSurface.goTo(target)
                pdfSurface.found(hits, currentHit)
            }
            return
        }
        runWebJs("clothbound.goTo(" + JSON.stringify(target) + ")"
                 + ".then(() => clothbound.found(" + JSON.stringify(hits) + ","
                 + JSON.stringify(currentHit) + "))")
    }
    function surfaceMarks(highlights) {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.setMarks()
            return
        }
        runWebJs("clothbound.marks(" + JSON.stringify(highlights) + ")")
    }
    function surfaceSpeak(start, end, follow) {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.speak(start, end, !!follow)
            return
        }
        runWebJs("clothbound.speak(" + Number(start) + "," + Number(end) + "," + (!!follow) + ")")
    }
    function surfaceGoToAndSpeak(target, start, end, follow) {
        if (pdf) {
            if (pdfSurface) {
                pdfSurface.goTo(target)
                pdfSurface.speak(start, end, !!follow)
            }
            return
        }
        runWebJs("clothbound.goTo(" + JSON.stringify(target) + ")"
                 + ".then(() => clothbound.speak(" + Number(start) + "," + Number(end) + "," + (!!follow) + "))")
    }
    function surfaceHighlight(name, spans) {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.highlight(name, spans)
            return
        }
        runWebJs("clothbound.highlight(" + JSON.stringify(name) + "," + JSON.stringify(spans) + ")")
    }
    function surfaceRequestSpeakable(chapter) {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.requestSpeakable(chapter)
            return
        }
        runWebJs("clothbound.requestSpeakable(" + Number(chapter) + ")")
    }
    function surfaceListeningStart(callback) {
        if (pdf) {
            const place = pdfSurface ? pdfSurface.listeningStart() : null
            if (callback)
                callback(place)
            return
        }
        runWebJs("JSON.stringify(clothbound.listeningStart())", function(result) {
            if (callback)
                callback(JSON.parse(result || "null"))
        })
    }
    function surfaceSampleText(callback) {
        if (pdf) {
            const sample = pdfSurface ? pdfSurface.sampleText() : ""
            if (callback)
                callback(sample)
            return
        }
        runWebJs("clothbound.sampleText()", callback)
    }
    // Compatibility entry point for external callers (e.g. Main.qml's citation
    // highlight jump): dispatches safely to typed PDF methods without eval.
    function js(code, callback) {
        if (!pdf) {
            runWebJs(code, callback)
            return
        }
        if (!pdfSurface)
            return
        const m = /^clothbound\.([A-Za-z]+)\(([\s\S]*)\)$/.exec(String(code || "").trim())
        if (!m)
            return
        let args = []
        const rawArgs = m[2].trim()
        if (rawArgs !== "") {
            try {
                args = JSON.parse("[" + rawArgs + "]")
            } catch (e) {
                return
            }
        }
        let result
        switch (m[1]) {
        case "open": result = pdfSurface.open(args[0], args[1], args[2]); break
        case "setStyle": result = pdfSurface.setStyle(args[0]); break
        case "next": result = pdfSurface.next(); break
        case "prev": result = pdfSurface.prev(); break
        case "nextChapter": result = pdfSurface.nextChapter(); break
        case "prevChapter": result = pdfSurface.prevChapter(); break
        case "back": result = pdfSurface.back(); break
        case "forward": result = pdfSurface.forward(); break
        case "goTo": result = pdfSurface.goTo(args[0]); break
        case "clearSelection": result = pdfSurface.clearSelection(); break
        case "escape": result = pdfSurface.dismiss(); break
        case "found": result = pdfSurface.found(args[0], args[1]); break
        case "marks": result = pdfSurface.setMarks(); break
        case "speak": result = pdfSurface.speak(args[0], args[1], args[2]); break
        case "highlight": result = pdfSurface.highlight(args[0], args[1]); break
        case "requestSpeakable": result = pdfSurface.requestSpeakable(args[0]); break
        case "listeningStart": result = pdfSurface.listeningStart(); break
        case "sampleText": result = pdfSurface.sampleText(); break
        default: return
        }
        if (callback)
            callback(result)
    }
    function style() {
        const margins = { narrow: [0.055, 36], normal: [0.1125, 30.5], wide: [0.155, 26] }[activeSettings.margins] || [0.1125, 30.5]
        const stageH = readingStage.height > 0 ? readingStage.height : view.height
        const topMargin = narration.active
            ? Math.max(68, Math.min(92, Math.round(stageH * 0.115)))
            : Math.max(60, Math.min(92, Math.round(stageH * 0.115)))
        const baseBottomMargin = Math.max(76, Math.min(108, Math.round(stageH * 0.13)))
        // Reserve +44 px bottom clearance while listening so the bottom line
        // of every page sits comfortably above the resting 44 px floating pill.
        const bottomMargin = narration.active
            ? Math.max(106, baseBottomMargin + 44)
            : baseBottomMargin
        return {
            theme: look,
            font: activeSettings.font,
            fontScale: activeSettings.fontScale,
            fontWeight: activeSettings.fontWeight,
            lineHeight: activeSettings.lineHeight,
            paragraphSpacing: activeSettings.paragraphSpacing,
            margin: margins[0],
            measure: margins[1],
            justify: activeSettings.justify,
            hyphenate: activeSettings.hyphenate,
            dropCap: activeSettings.dropCap,
            layout: activeSettings.layout,
            singleColumn: view.companionDocked,
            animate: activeSettings.animate,
            reducedMotion: theme.reducedMotion,
            publisher: activeSettings.publisher,
            pdfCrop: activeSettings.pdfCrop,
            pdfOriginalColors: activeSettings.pdfOriginalColors,
            pdfFit: activeSettings.pdfFit,
            marginTop: topMargin,
            // Room for the bottom footer or the floating slim listening pill.
            marginBottom: bottomMargin,
            radius: theme.radius,
            listening: narration.active,
            strings: { goToNote: qsTr("Go to note"), listenFromHere: qsTr("Listen from here") },
        }
    }
    function closeWebSurface() {
        if (!epubSurfaceOpen && !(surfaceReady && web && !pdf))
            return
        epubSurfaceOpen = false
        if (surfaceReady && web) {
            epubClosing = true
            web.updateLifecycleState()
            web.runJavaScript("clothbound.close()", function() {
                view.epubClosing = false
                if (view.web)
                    view.web.updateLifecycleState()
            })
        }
    }
    function openBook() {
        if (!reader.open)
            return
        clearSurfaceNudge()
        surfaceWatcher.resetSurfaceNudgeCooldown()
        pendingStartListening = false
        listenOffer.close()
        menus.closeAll()
        selectionBar.visible = false
        view.selection = ({})
        view.lastCompanionTarget = null
        decoratedChapter = -1
        jumpPillVisible = false
        lightbox.close()
        if (pdfSurface) {
            pdfSurface.cancelSnip()
            pdfSurface.closePeek()
            pdfSurface.clearSelection()
        }
        if (pdf) {
            closeWebSurface()
        } else {
            epubClosing = false
            webNeeded = true
            if (web)
                web.updateLifecycleState()
            if (!surfaceReady)
                return
            epubSurfaceOpen = true
        }
        surfaceOpen(reader.manifest, reader.startPosition, style())
    }
    function closeBook() {
        clearSurfaceNudge()
        surfaceWatcher.resetSurfaceNudgeCooldown()
        refreshTimer.stop()
        refreshLater.stop()
        pendingStartListening = false
        listenOffer.close()
        menus.closeAll()
        closePanels()
        if (searchBar.open)
            searchBar.close()
        lightbox.close()
        if (pdfSurface) {
            pdfSurface.cancelSnip()
            pdfSurface.closePeek()
            pdfSurface.clearSelection()
        }
        jumpPillVisible = false
        selectionBar.visible = false
        view.selection = ({})
        view.lastCompanionTarget = null
        decoratedChapter = -1
        closeWebSurface()
    }
    // Dismiss transient reader overlays when stepping back to the Library while
    // audio continues playing, keeping the book and WebEngine surface alive.
    function leaveForLibrary() {
        listenOffer.close()
        menus.closeAll()
        closePanels()
        if (searchBar.open)
            searchBar.close()
        lightbox.close()
        if (pdfSurface) {
            pdfSurface.cancelSnip()
            pdfSurface.closePeek()
            pdfSurface.clearSelection()
        }
        chromeShown = false
        selectionBar.visible = false
        view.selection = ({})
    }
    function toggleSnipMode() {
        if (!pdf || !reader.open || !pdfSurface)
            return
        const next = !pdfSurface.snipMode
        if (next) {
            selectionBar.visible = false
            chromeShown = false
        }
        pdfSurface.snipMode = next
        if (!next)
            focusPage()
    }
    function copyCitation(style) {
        const text = reader.formatCitation(style)
        if (text === "")
            return
        reader.copyText(text)
        toast.show(style === "bibtex" ? qsTr("Copied BibTeX") : qsTr("Copied APA"))
    }
    // Turning pages by hand while listening leaves the voice where it is;
    // "Back to the voice" brings the page back to it.
    function leaveVoice() {
        if (narration.active)
            narration.following = false
    }
    function next() { leaveVoice(); surfaceNext() }
    function prev() { leaveVoice(); surfacePrev() }
    function pageStep(dir) {
        leaveVoice()
        if (pdf) {
            if (pdfSurface)
                pdfSurface.pageStep(dir)
        } else {
            dir > 0 ? surfaceNext() : surfacePrev()
        }
    }
    function scrollVertical(dir) {
        if (pdf && pdfSurface)
            pdfSurface.scrollVertical(dir)
    }
    function nextChapter() { leaveVoice(); surfaceNextChapter() }
    function prevChapter() { leaveVoice(); surfacePrevChapter() }
    function back() { leaveVoice(); jumpPillVisible = false; surfaceBack() }
    function forward() { leaveVoice(); jumpPillVisible = false; surfaceForward() }
    function goTo(target) { leaveVoice(); surfaceGoTo(target) }

    // Cross-branch seam (ContinueShelf / VoicePanel): start reading aloud from
    // the current page, deferring cleanly if the EPUB surface is still loading.
    function startListening() {
        if (narration.active) {
            if (!narration.playing)
                narration.resume()
            return
        }
        if (!pdf && (!surfaceReady || reader.relocationSeq === 0)) {
            pendingStartListening = true
            return
        }
        pendingStartListening = false
        surfaceListeningStart(function(place) {
            if (place)
                narration.start(place.chapter, place.offset)
            else if (reader.open)
                narration.start(reader.chapter, reader.pageStart)
        })
    }

    // Listening starts from the top of the page in view, or stops if active.
    function listen() {
        if (narration.active) {
            narration.stop()
            return
        }
        startListening()
    }
    // Keys and the mouse go back to the page when what was open closes.
    function focusPage() {
        if (pdf) {
            if (pdfSurface)
                pdfSurface.forceActiveFocus()
            return
        }
        if (!modalPanelOpen && web)
            web.forceActiveFocus()
    }
    function closePanels() {
        contents.close()
        appearance.close()
        goTo.close()
        voicePanel.close()
        marksPanel.close()
        companionPanel.close()
        noteEditor.close()
        if (searchBar.editing)
            searchBar.close()
        if (pdf) {
            if (pdfSurface)
                pdfSurface.forceActiveFocus()
        } else if (web) {
            web.forceActiveFocus()
        }
    }
    function dismiss() {
        if (lightbox.open) {
            lightbox.close()
            focusPage()
            return
        }
        // Close ListenOffer on Esc without cancelling an active voice download.
        if (listenOffer.open) {
            listenOffer.close()
            focusPage()
            return
        }
        if (pdf && pdfSurface && pdfSurface.snipMode) {
            pdfSurface.cancelSnip()
            focusPage()
            return
        }
        if (pdf && pdfSurface && pdfSurface.peekOpen) {
            pdfSurface.closePeek()
            focusPage()
            return
        }
        if (panelOpen) {
            closePanels()
            return
        }
        if (selectionBar.visible) {
            selectionBar.visible = false
            surfaceClearSelection()
            return
        }
        if (searchBar.open) {
            searchBar.close()
            return
        }
        if (chromeFocused) {
            view.chromeShown = false
            focusPage()
            return
        }
        surfaceEscape(function(closed) {
            if (!closed)
                view.closeRequested()
        })
    }
    function cycleTheme() {
        const order = Themes.order
        const nextTheme = order[(order.indexOf(activeSettings.theme) + 1) % order.length]
        setAppearance("theme", nextTheme)
        toast.show(qsTr("%1 theme").arg(Themes.name(nextTheme)))
    }
    function setPdfFit(mode, announce) {
        if (!pdf)
            return
        const normalized = (mode === "width" || mode === "actual") ? mode : "page"
        setAppearance("pdfFit", normalized)
        if (pdfSurface)
            pdfSurface.setFitMode(normalized)
        if (normalized !== "page" && !view.panelOpen)
            view.chromeShown = false
        if (announce) {
            const label = normalized === "width" ? qsTr("Fit to width")
                        : normalized === "actual" ? qsTr("Actual size")
                        : qsTr("Fit to page")
            toast.show(label)
        }
    }
    function resize(step) {
        if (lightbox.open) {
            lightbox.setZoom(step === 0 ? 1.0 : lightbox.zoom * (step > 0 ? 1.25 : 1 / 1.25))
            return
        }
        // A PDF's text is its pages: 0 fits the whole page, +/− zoom smoothly
        // from the current fit scale.
        if (pdf) {
            if (step === 0) {
                setPdfFit("page", !view.panelOpen)
            } else if (pdfSurface) {
                pdfSurface.nudgeZoom(step > 0 ? 1.15 : 1 / 1.15)
                if (pdfSurface.zoomedIn && !view.panelOpen)
                    view.chromeShown = false
            }
            return
        }
        setAppearance("fontScale", step === 0 ? 1.15 : Math.max(0.7, Math.min(2.0, Math.round((activeSettings.fontScale + step) * 100) / 100)))
    }
    function showContents() {
        appearance.close()
        voicePanel.close()
        marksPanel.close()
        companionPanel.close()
        contents.show()
    }
    function showAppearance() {
        contents.close()
        voicePanel.close()
        marksPanel.close()
        companionPanel.close()
        appearance.show()
    }
    // Search hits on the page: all of them in the chapter shown, and the one
    // gone to.
    property int foundChapter: -1
    function markFound() {
        if ((!surfaceReady && !pdf) || !reader.open)
            return
        foundChapter = reader.chapter
        const hit = bookSearch.current >= 0 ? bookSearch.results[bookSearch.current] : null
        const current = hit && (hit.chapter === reader.chapter || pdf)
                        ? { chapter: hit.chapter, start: hit.start, end: hit.end } : null
        surfaceFound(bookSearch.inChapter(reader.chapter), current)
    }
    function showFound(hit) {
        leaveVoice()
        surfaceGoToAndFound({ chapter: hit.chapter, offset: hit.start, instant: true },
                            bookSearch.inChapter(hit.chapter),
                            { chapter: hit.chapter, start: hit.start, end: hit.end })
        foundChapter = hit.chapter
    }
    Connections {
        target: bookSearch
        function onChanged() { view.markFound() }
    }
    // Highlights and search hits are drawn on each chapter as it's shown.
    property int decoratedChapter: -1
    function decorate() {
        if ((pdf || (surfaceReady && reader.relocationSeq > 0)) && reader.open)
            surfaceMarks(marks.highlightsIn(reader.chapter))
    }
    Connections {
        target: reader
        function onLocationChanged() {
            selectionBar.visible = false
            // Skip Reader::openBook()'s synchronous pre-load signal on EPUBs so
            // opening-chapter highlights paint once reader.js actually loads it.
            if (!view.pdf && reader.relocationSeq === 0)
                return
            if (view.pendingStartListening) {
                view.pendingStartListening = false
                Qt.callLater(view.startListening)
            }
            if (reader.chapter !== view.decoratedChapter) {
                view.decoratedChapter = reader.chapter
                view.decorate()
                if (bookSearch.count > 0 || view.foundChapter >= 0)
                    view.markFound()
                // Back in the chapter being read aloud: its sentence at once.
                if (narration.active && narration.chapter === reader.chapter && narration.sentenceStart >= 0)
                    view.surfaceSpeak(narration.sentenceStart, narration.sentenceEnd, false)
            }
        }
        function onBookChanged() { view.decoratedChapter = -1 }
        function onListenRequested(chapter, offset) {
            view.leaveVoice()
            narration.start(chapter, offset)
        }
        function onSelected(selection) {
            view.selection = selection && selection.text ? selection : ({})
            if (selection && selection.text)
                win.tip("select", qsTr("Right-click words for Define, Explain, Translate, or a picture of the moment."))
            if (selection && selection.text) {
                selectionBar.forMark = false
                selectionBar.target = selection
                selectionBar.place(view.mapRect(selection), readingStage)
                selectionBar.visible = true
            } else if (!selectionBar.forMark) {
                selectionBar.visible = false
            }
        }
        function onMarkChosen(id, rect) {
            selectionBar.forMark = true
            selectionBar.target = marks.mark(id)
            selectionBar.place(view.mapRect(rect), readingStage)
            selectionBar.visible = true
        }
    }
    Connections {
        target: marks
        function onChanged() { view.decorate() }
    }
    function mapRect(rect) {
        const src = pdf ? pdfSurface : web
        if (!src)
            return rect
        const at = src.mapToItem(readingStage, rect.x, rect.y)
        return { x: at.x, y: at.y, width: rect.width, height: rect.height }
    }

    // B: a bookmark on the page in view, or off it again.
    readonly property int pageBookmark: marks.count >= 0 && reader.open
        ? marks.bookmarkBetween(reader.chapter, reader.pageStart, Math.max(reader.pageEnd, reader.pageStart + 1)) : 0
    function toggleBookmark() {
        if (pageBookmark) {
            marks.remove(pageBookmark)
            toast.show(qsTr("Bookmark removed"), qsTr("Undo"), () => marks.undo())
        } else {
            marks.addBookmark(reader.chapter, reader.pageStart, reader.snippet, reader.titleOfChapter(reader.chapter))
            toast.show(qsTr("Bookmarked"))
        }
    }

    function showMarks() {
        contents.close()
        appearance.close()
        voicePanel.close()
        companionPanel.close()
        marksPanel.show()
    }
    // The companion, on its own or for words on the page.
    function showCompanion(tab, focusInput) {
        contents.close()
        appearance.close()
        voicePanel.close()
        marksPanel.close()
        companionPanel.show(tab, !!focusInput)
    }
    // Supports both `askCompanion(question)` (from Main.qml / CommandPalette)
    // and `askCompanion(feature, target)` (from SelectionBar / pageMenu).
    function askCompanion(arg1, arg2) {
        if (arg2 !== undefined && arg2 !== null) {
            const feature = arg1
            const target = arg2
            if (!target || !target.text)
                return
            lastCompanionTarget = { chapter: target.chapter, start: target.start, end: target.end, text: target.text }
            showCompanion("selection")
            if (feature === "define")
                companion.define(target.text, target.chapter, target.start)
            else if (feature === "explain")
                companion.explain(target.text, target.chapter, target.end)
            else
                companion.translate(target.text, target.chapter, target.end)
            return
        }
        showCompanion("ask", true)
        const question = String(arg1 || "").trim()
        if (question !== "")
            companion.ask(question)
    }
    function showVoices() {
        contents.close()
        appearance.close()
        marksPanel.close()
        companionPanel.close()
        voicePanel.show()
    }
    function showChrome() {
        chromeShown = true
        chromeTimer.restart()
    }
    function triggerPageTurnSound(direction) {
        if ((direction > 0 && reader.atEnd) || (direction < 0 && reader.atStart))
            return
        if (view.settings && view.settings.pageTurnSound && !narration.playing
            && typeof reader.playPageTurn === "function")
            reader.playPageTurn(direction)
    }

    // Typography and theme changes reach the page together, once.
    Timer {
        id: styleTimer
        interval: 50
        onTriggered: if ((view.surfaceReady || view.pdf) && reader.open) view.surfaceSetStyle(view.style())
    }
    onLookChanged: styleTimer.restart()
    Connections {
        target: theme
        function onChanged() { styleTimer.restart() }
    }
    Connections {
        target: reader
        function onAppearanceChanged() { styleTimer.restart() }
        function onPageTurnRequested(direction) { view.triggerPageTurnSound(direction) }
        function onJumpRecorded(label) {
            view.jumpPillLabel = label
            view.jumpPillVisible = true
            jumpPillTimer.restart()
        }
        function onImageChosen(source, caption) {
            lightbox.openImage(source, caption)
        }
    }
    Connections {
        target: activeSettings
        function onFontChanged() { styleTimer.restart() }
        function onFontScaleChanged() { styleTimer.restart() }
        function onFontWeightChanged() { styleTimer.restart() }
        function onLineHeightChanged() { styleTimer.restart() }
        function onParagraphSpacingChanged() { styleTimer.restart() }
        function onMarginsChanged() { styleTimer.restart() }
        function onJustifyChanged() { styleTimer.restart() }
        function onHyphenateChanged() { styleTimer.restart() }
        function onDropCapChanged() { styleTimer.restart() }
        function onLayoutChanged() { styleTimer.restart() }
        function onAnimateChanged() { styleTimer.restart() }
        function onPublisherChanged() { styleTimer.restart() }
        function onPdfCropChanged() { styleTimer.restart() }
        function onPdfOriginalColorsChanged() { styleTimer.restart() }
        function onPdfFitChanged() { styleTimer.restart() }
    }

    Connections {
        target: download
        function onBlocksWanted(chapter) { view.surfaceRequestSpeakable(chapter) }
        function onReady() { toast.show(qsTr("Ready to listen offline")) }
        function onChanged() { if (view.web) view.web.updateLifecycleState() }
    }
    Connections {
        target: narration
        function onBlocksWanted(chapter) { view.surfaceRequestSpeakable(chapter) }
        function onSpeaking(chapter, start, end) {
            // A PDF shows two pages at once and finds its own way to the page.
            if (chapter === reader.chapter || view.pdf)
                view.surfaceSpeak(start, end, narration.following)
            else if (narration.following)
                view.surfaceGoToAndSpeak({ chapter: chapter, offset: start, instant: true, quiet: true, automated: true },
                                         start, end, true)
        }
        function onStateChanged() {
            if (narration.active) {
                view.chromeShown = false
                win.tip("listen", qsTr(", and . skip a paragraph, < and > change the speed. Close the window and the reading goes on."))
            }
            if (narration.active !== view.listening) {
                view.listening = narration.active
                styleTimer.restart()
            }
            if (!narration.active)
                view.surfaceHighlight("cb-speaking", [])
            if (view.web)
                view.web.updateLifecycleState()
        }
        function onNeedsKey() {
            toast.show(qsTr("Listening uses your Gemini key. Add it to start."))
            win.showAiSettings()
        }
        function onVoiceMissing() {
            // No voice at all yet: offer the one for this book, right here.
            const pack = voicePacks.supported && narration.hasPendingStart && !narration.voice.startsWith("builtin:")
                         ? voicePacks.offer(reader.language) : ({})
            if (pack.voice) {
                listenOffer.show()
                return
            }
            toast.show(qsTr("That voice isn't downloaded yet. Choose or download one."))
            view.showVoices()
        }
    }

    // Back from another workspace, monitor sleep, or tray: force Qt Quick and
    // Chromium's Viz compositor to produce and bind a fresh frame, even when
    // Clothbound returns tiled and unfocused. A single coalesced 1 px bottom-margin
    // nudge (inside the solid reading margin) forces visualPropertiesChanged()
    // to allocate a new LocalSurfaceId and backing GPU texture, while reader.js
    // explicitly suppresses relayout during the transient nudge so text lines
    // never reflow or shift.
    property bool surfaceNudge: false

    function clearSurfaceNudge() {
        nudgeRestoreTimer.stop()
        if (surfaceNudge) {
            surfaceNudge = false
            if (!pdf && surfaceReady && web)
                view.runWebJs("clothbound.endSurfaceNudge && clothbound.endSurfaceNudge()")
        }
    }

    function runSurfacePaint() {
        if (!reader.open || !win.visible)
            return
        if (pdf) {
            if (pdfSurface)
                surfaceWatcher.forceItemUpdate(pdfSurface)
            return
        }
        if (!surfaceReady || !web)
            return
        surfaceWatcher.forceItemUpdate(web)
        view.runWebJs("clothbound.refresh && clothbound.refresh()")
    }

    function refreshSoon(reason) {
        if (!reader.open || !win.visible)
            return
        if (pdf) {
            if (pdfSurface)
                surfaceWatcher.forceItemUpdate(pdfSurface)
            return
        }
        if (!surfaceReady || !web)
            return
        surfaceWatcher.forceItemUpdate(web)
        if (!surfaceNudge && !epubLoading && surfaceWatcher.takeSurfaceNudge(reason)) {
            view.runWebJs("clothbound.beginSurfaceNudge && clothbound.beginSurfaceNudge()")
            surfaceNudge = true
            nudgeRestoreTimer.restart()
        }
        refreshTimer.restart()
        refreshLater.restart()
    }

    Connections {
        target: surfaceWatcher
        function onRepaintRequested(reason) { view.refreshSoon(reason) }
        function onExposedChanged() {
            if (!surfaceWatcher.exposed) {
                view.clearSurfaceNudge()
                refreshTimer.stop()
                refreshLater.stop()
            }
        }
    }
    Connections {
        target: win
        function onVisibleChanged() {
            if (!win.visible) {
                view.clearSurfaceNudge()
                surfaceWatcher.resetSurfaceNudgeCooldown()
                refreshTimer.stop()
                refreshLater.stop()
            }
            if (view.web)
                view.web.updateLifecycleState()
        }
    }
    onPdfChanged: if (pdf) clearSurfaceNudge()
    Timer {
        id: nudgeRestoreTimer
        interval: 32
        onTriggered: {
            view.surfaceNudge = false
            if (!view.pdf && view.surfaceReady && reader.open && view.web) {
                view.runWebJs("clothbound.endSurfaceNudge && clothbound.endSurfaceNudge()")
                surfaceWatcher.forceItemUpdate(view.web)
            }
        }
    }
    Timer { id: refreshTimer; interval: 16; onTriggered: view.runSurfacePaint() }
    Timer { id: refreshLater; interval: 320; onTriggered: view.runSurfacePaint() }

    Connections {
        target: reader
        function onSurfaceReady() {
            view.surfaceReady = true
            if (reader.open && !view.pdf)
                view.openBook()
        }
        function onBookChanged() {
            if (!reader.open) {
                view.closeBook()
                return
            }
            if (reader.format === "pdf")
                win.tip("pdf", qsTr("PDF pages follow your reading theme. W fits width, 0 fits page, 1 is actual size, and + / − zoom."))
            else
                view.webNeeded = true
            view.closePanels()
            // J3 first book: reveal the top bar gently alongside the tips bar.
            view.chromeShown = onboarding.tips.indexOf("keys") < 0
            if (view.chromeShown)
                chromeTimer.restart()
            view.openBook()
        }
        function onTap(zone) {
            if (zone === "turn")
                view.leaveVoice()
            else if (zone === "center")
                view.chromeShown ? (view.chromeShown = false) : view.showChrome()
        }
        function onExternalLink(url) {
            toast.show(qsTr("Opening %1 in your browser").arg(url.replace(/^https?:\/\//, "").split("/")[0]))
            Qt.openUrlExternally(url)
        }
        function onKey(key, modifiers) { view.pageKey(key, modifiers) }
    }

    // Keys typed while the page had the focus come back here.
    function pageKey(key, modifiers) {
        const shift = (modifiers & 1) !== 0, ctrl = (modifiers & 2) !== 0, alt = (modifiers & 4) !== 0, meta = (modifiers & 8) !== 0
        if (ctrl && !alt && !meta) {
            if (key === "q" || key === "Q") { win.quitApp(); return }
            if (key === "w" || key === "W") { win.close(); return }
        }
        if (win.helpVisible || settingsView.open || welcome.open || win.typing)
            return
        const plain = !ctrl && !alt && !meta
        if (alt && !ctrl && !meta) {
            if (key === "ArrowLeft") { back(); return }
            if (key === "ArrowRight") { forward(); return }
            return
        }
        if (shift && !ctrl && !alt && !meta && narration.active && typeof narration.skipSeconds === "function") {
            if (key === "ArrowLeft") { narration.skipSeconds(-15); return }
            if (key === "ArrowRight") { narration.skipSeconds(15); return }
        }
        if (ctrl && !alt && !meta) {
            if (key === "f" || key === "F") searchBar.show()
            else if ((key === "k" || key === "K") && typeof commandPalette !== "undefined") commandPalette.open ? commandPalette.close() : commandPalette.show()
            else if (key === "+" || key === "=") resize(0.05)
            else if (key === "-") resize(-0.05)
            else if (key === "0") resize(0)
            else if (key === "1" && pdf) setPdfFit("actual", true)
            else if (key === ",") settingsView.open ? settingsView.close() : settingsView.show()
            else if ((key === "z" || key === "Z") && shift && marks.canRedo) marks.redo()
            else if ((key === "z" || key === "Z") && !shift && marks.canUndo) {
                const what = marks.undoText
                marks.undo()
                toast.show(qsTr("Undid: %1").arg(what))
            }
            return
        }
        if (key === "Escape") { dismiss(); return }
        if (!plain)
            return
        if (key === "ArrowRight") { reader.rtl ? prev() : next() }
        else if (key === "ArrowLeft") { reader.rtl ? next() : prev() }
        else if (key === "ArrowDown" && pdf) scrollVertical(1)
        else if (key === "ArrowUp" && pdf) scrollVertical(-1)
        else if (key === "l") next()
        else if (key === "h") prev()
        else if (key === "PageDown" || key === "j" || (key === " " && !shift)) pageStep(1)
        else if (key === "PageUp" || key === "k" || (key === " " && shift)) pageStep(-1)
        else if (key === "]") nextChapter()
        else if (key === "[") prevChapter()
        else if (key === "t" || key === "T") { contents.open ? contents.close() : showContents() }
        else if (key === "a" || key === "A") { appearance.open ? appearance.close() : showAppearance() }
        else if (key === "g" || key === "G") goTo.show()
        else if (key === "b" || key === "B") toggleBookmark()
        else if (key === "m" || key === "M") { marksPanel.open ? marksPanel.close() : showMarks() }
        else if (key === "c" || key === "C") { companionPanel.open ? companionPanel.close() : showCompanion("ask") }
        else if ((key === "x" || key === "X") && pdf) toggleSnipMode()
        else if (key === "v" || key === "V") { voicePanel.open ? voicePanel.close() : showVoices() }
        else if (key === "/") searchBar.show()
        else if (key === "n" && bookSearch.count > 0) searchBar.next()
        else if (key === "N" && bookSearch.count > 0) searchBar.previous()
        else if (key === "Tab") { if (view.readingViewportFocused) view.chromeShown ? (view.chromeShown = false) : view.showChrome() }
        else if (key === "p" && !shift) narration.active ? narration.toggle() : listen()
        else if ((key === "P" || (key === "p" && shift)) && narration.active) narration.stop()
        else if (key === "," && narration.active) narration.previousParagraph()
        else if (key === "." && narration.active) narration.nextParagraph()
        else if (key === "<" && narration.active) narration.rate = narration.rate - 0.1
        else if (key === ">" && narration.active) narration.rate = narration.rate + 0.1
        else if (key === "d" || key === "D") cycleTheme()
        else if (key === "+" || key === "=") resize(0.05)
        else if (key === "-") resize(-0.05)
        else if (key === "0") resize(0)
        else if ((key === "w" || key === "W") && pdf) setPdfFit("width", true)
        else if (key === "1" && pdf) setPdfFit("actual", true)
        else if (key === "f" || key === "F" || key === "F11") win.toggleFullScreen()
        else if (key === "?") win.helpVisible = !win.helpVisible
        else if (key === "q" || key === "Q") win.quitApp()
    }

    Rectangle {
        anchors.fill: parent
        color: view.look.bg
        Behavior on color {
            enabled: !theme.reducedMotion
            ColorAnimation { duration: 200; easing.type: Easing.OutCubic }
        }
    }

    // The reading stage: fills the window normally, and shrinks cleanly by the
    // docked Companion drawer width on wide windows without per-frame resize
    // spam so users can read and select words beside Companion (04_companion).
    Item {
        id: readingStage
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.rightMargin: view.pdf ? Math.round(view.companionLiveDockWidth) : view.companionDockWidth

        Loader {
            id: pdfLoader
            anchors.fill: parent
            active: view.pdf
            visible: view.pdf
            focus: view.pdf
            sourceComponent: PdfSurface {
                id: pdfSurfaceItem
                anchors.fill: parent
                look: view.look
                liveResizing: view.companionSliding || view.companionResizing
                onContextMenuRequested: (x, y) => {
                    const place = pdfSurfaceItem.placeAt(x, y)
                    if (!place)
                        return
                    pageMenu.place = place
                    pageMenu.popup(pdfSurfaceItem, x, y)
                }
                onSnipActionTriggered: (action, base64Png, pageIndex, prompt) => {
                    view.showCompanion("ask")
                    if (typeof companion.askAboutSnipBase64 === "function") {
                        companion.askAboutSnipBase64(base64Png, action, prompt || "", pageIndex)
                    } else {
                        toast.show(qsTr("Snip captured (p. %1)").arg(pageIndex + 1))
                    }
                }
            }
        }

        Loader {
            id: webLoader
            readonly property real epubDockGlide: (!view.pdf && view.companionSliding && !theme.reducedMotion)
                                                  ? (view.companionDocked
                                                     ? Math.round(view.companionTargetWidth * (1 - companionPanel.slideProgress) / 2)
                                                     : -Math.round(view.companionTargetWidth * companionPanel.slideProgress / 2))
                                                  : 0
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: epubDockGlide
            anchors.rightMargin: -epubDockGlide
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.bottomMargin: view.surfaceNudge && parent.height > 2 ? 1 : 0
            active: view.webNeeded
            visible: !view.pdf
            opacity: view.pdf ? 0 : 1
            enabled: !view.pdf
            sourceComponent: webComponent
        }

        // Subtle cold-start loading indicator while WebEngine boots and renders
        // the opening EPUB chapter, delayed slightly so warm loads never flicker.
        Item {
            id: epubSpinner
            anchors.centerIn: parent
            width: 40
            height: 40
            property bool shown: false
            visible: opacity > 0.01
            opacity: shown && view.epubLoading ? 0.6 : 0
            Behavior on opacity { NumberAnimation { duration: theme.reducedMotion ? 0 : 180 } }
            Timer {
                interval: 180
                running: view.epubLoading
                onTriggered: epubSpinner.shown = true
            }
            Connections {
                target: view
                function onEpubLoadingChanged() {
                    if (!view.epubLoading)
                        epubSpinner.shown = false
                }
            }
            BusyIndicator {
                anchors.centerIn: parent
                running: epubSpinner.visible
                implicitWidth: 28
                implicitHeight: 28
            }
        }

        // Reveal reading chrome consistently: moving into the top 56 px edge
        // ALWAYS reveals the top bar (including while listening or in zoomed /
        // Fit-to-Width PDFs), while mid-page pointer drift stays quiet during
        // listening or PDF panning so text is never obscured.
        property real lastPointerX: -1
        property real lastPointerY: -1
        HoverHandler {
            id: pointerWatch
            onHoveredChanged: {
                if (!hovered) {
                    readingStage.lastPointerX = -1
                    readingStage.lastPointerY = -1
                    return
                }
                const py = point.position.y
                if (py <= 56 && !view.modalPanelOpen && !lightbox.open
                    && !(view.pdf && pdfSurface && (pdfSurface.snipMode || pdfSurface.peekOpen)))
                    view.showChrome()
            }
            onPointChanged: {
                const px = point.position.x, py = point.position.y
                const canReveal = !view.modalPanelOpen && !lightbox.open
                                  && !(view.pdf && pdfSurface && (pdfSurface.snipMode || pdfSurface.peekOpen))
                if (canReveal) {
                    const nearTopEdge = py <= 56
                    const nearBottomEdge = py >= readingStage.height - 84
                    const zoomedPdf = view.pdf && pdfSurface && pdfSurface.zoomedIn
                    if (nearTopEdge) {
                        if (readingStage.lastPointerX < 0 || Math.hypot(px - readingStage.lastPointerX, py - readingStage.lastPointerY) >= 2)
                            view.showChrome()
                    } else if (readingStage.lastPointerX >= 0 && !narration.active) {
                        const dist = Math.hypot(px - readingStage.lastPointerX, py - readingStage.lastPointerY)
                        if (zoomedPdf) {
                            if (nearBottomEdge && dist >= 4)
                                view.showChrome()
                        } else if (dist >= 6) {
                            view.showChrome()
                        }
                    }
                }
                readingStage.lastPointerX = px
                readingStage.lastPointerY = py
            }
        }

        Timer {
            id: jumpPillTimer
            interval: 12000
            onTriggered: view.jumpPillVisible = false
        }

        // Top/bottom chrome, running head, folios, first-book tips, jump-back pill,
        // GPU-accelerated bookmark dog-ear, and first-listen offer popover.
        ReaderChrome {
            id: readerChrome
            z: 2
            anchors.fill: parent
            stageItem: readingStage
            look: view.look
            pdf: view.pdf
            chromeShown: view.chromeShown
            modalPanelOpen: view.modalPanelOpen
            scrollLayout: activeSettings.layout === "scroll"
            pdfZoomedIn: !!(view.pdf && pdfSurface && pdfSurface.zoomedIn)
            contentsOpen: contents.open
            contentsWidth: contents.width
            citeMenuOpened: citeMenu.opened
            firstBookTipsActive: view.firstBookTipsActive
            firstBookTipsModel: view.firstBookTipsModel
            jumpPillVisible: view.jumpPillVisible
            jumpPillLabel: view.jumpPillLabel
            pageBookmark: view.pageBookmark
            footerInset: view.footerInset
            listenBarHeight: listenBar.height
            rightToolsWidth: tools.width
            rightToolsMargin: tools.anchors.rightMargin
            listenButtonCenterX: tools.x + listenButton.x + listenButton.width / 2

            onCloseRequested: view.closeRequested()
            onToggleContents: contents.open ? contents.close() : view.showContents()
            onCiteMenuRequested: (anchor, x, y) => citeMenu.popup(anchor, x, y)
            onGoToTarget: (target) => view.goTo(target)
            onShowGoTo: goTo.show()
            onPrevPageRequested: view.prev()
            onNextPageRequested: view.next()
            onJumpBack: view.back()
            onDismissJumpPill: view.jumpPillVisible = false
            onDismissFirstBookTips: {
                onboarding.tips = onboarding.tips.concat(["keys"])
                chromeTimer.restart()
            }
            onToggleBookmark: view.toggleBookmark()
            onShowVoices: view.showVoices()
            onFocusPageRequested: view.focusPage()

            Row {
                id: tools
                anchors.right: parent.right
                anchors.rightMargin: 52
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                IconButton {
                    implicitWidth: 32
                    implicitHeight: 30
                    tabFocusable: view.chromeShown
                    look: view.look
                    glyph: Icons.search
                    active: searchBar.open
                    tip: qsTr("Search in book (/)")
                    onClicked: searchBar.open ? searchBar.close() : searchBar.show()
                }
                IconButton {
                    implicitWidth: 32
                    implicitHeight: 30
                    tabFocusable: view.chromeShown
                    look: view.look
                    glyph: Icons.bookmark
                    active: marksPanel.open
                    tip: qsTr("Highlights and notes (M)")
                    onClicked: marksPanel.open ? marksPanel.close() : view.showMarks()
                }
                IconButton {
                    implicitWidth: 32
                    implicitHeight: 30
                    tabFocusable: view.chromeShown
                    look: view.look
                    tip: qsTr("Appearance (A)")
                    active: appearance.open
                    contentItem: Label {
                        text: "Aa"
                        font.family: "Literata"
                        font.pixelSize: 15
                        font.weight: Font.Medium
                        color: appearance.open ? view.look.link : (view.look.inkSoft || view.look.fg)
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: appearance.open ? appearance.close() : view.showAppearance()
                }
                Item {
                    id: listenButton
                    width: 32
                    height: 30
                    // Subtle accent halo inviting First Listen while J3 tips are active (12_j3_first_book:468).
                    Rectangle {
                        anchors.fill: parent
                        visible: view.firstBookTipsActive && !narration.active
                        radius: Ui.radius(7, theme.radius)
                        color: Ui.tint(view.look.link, 0.12)
                        border.width: 2
                        border.color: Ui.tint(view.look.link, 0.35)
                    }
                    IconButton {
                        anchors.fill: parent
                        tabFocusable: view.chromeShown
                        look: view.look
                        glyph: Icons.headphones
                        tone: view.firstBookTipsActive ? view.look.link : (view.look.inkSoft || view.look.fg)
                        active: narration.active
                        tip: narration.active ? qsTr("Stop listening (⇧P)") : qsTr("Listen (P)")
                        onClicked: view.listen()
                    }
                }
                IconButton {
                    visible: reader.isPdf
                    implicitWidth: 32
                    implicitHeight: 30
                    tabFocusable: view.chromeShown
                    look: view.look
                    glyph: "M4 8.5 V5.5 Q4 4 5.5 4 H8.5 M15.5 4 H18.5 Q20 4 20 5.5 V8.5 M20 15.5 V18.5 Q20 20 18.5 20 H15.5 M8.5 20 H5.5 Q4 20 4 18.5 V15.5 M12 9 V15 M9 12 H15"
                    active: !!pdfSurface && pdfSurface.snipMode
                    tip: qsTr("Snip region (X)")
                    onClicked: view.toggleSnipMode()
                }
                IconButton {
                    implicitWidth: 32
                    implicitHeight: 30
                    tabFocusable: view.chromeShown
                    look: view.look
                    glyph: Icons.sparkle
                    active: companionPanel.open
                    tip: qsTr("Companion (C)")
                    onClicked: companionPanel.open ? companionPanel.close() : view.showCompanion("ask")
                }
            }
        }

        SelectionBar {
            id: selectionBar
            visible: false
            z: 6
            look: view.look
            onHighlight: (color) => {
                if (forMark) {
                    marks.setColor(target.id, color)
                } else {
                    marks.addHighlight(target.chapter, target.start, target.end, target.text, color,
                                       reader.titleOfChapter(target.chapter))
                    view.surfaceClearSelection()
                }
                visible = false
            }
            onAskCompanion: (feature) => {
                view.askCompanion(feature, target)
                visible = false
            }
            onDefineHere: {
                if (!target || !(target.text || target.quote))
                    return
                const text = target.text || target.quote
                view.lastCompanionTarget = { chapter: target.chapter, start: target.start, end: target.end, text: text }
                companion.define(text, target.chapter, target.start)
            }
            onMoreInCompanion: {
                view.showCompanion("selection")
                visible = false
            }
            onNote: {
                const id = forMark ? target.id
                    : marks.addHighlight(target.chapter, target.start, target.end, target.text, "yellow",
                                         reader.titleOfChapter(target.chapter))
                if (!forMark)
                    view.surfaceClearSelection()
                visible = false
                noteEditor.show(id)
            }
            onCopy: {
                reader.copyText(target.text)
                if (!forMark)
                    view.surfaceClearSelection()
                visible = false
                toast.show(qsTr("Copied"))
            }
            onListen: {
                view.surfaceClearSelection()
                visible = false
                narration.start(target.chapter, target.start)
            }
            onRemove: {
                marks.remove(target.id)
                visible = false
                toast.show(qsTr("Highlight removed"), qsTr("Undo"), () => marks.undo())
            }
        }
    }

    Component {
        id: webComponent
        WebEngineView {
            id: webItem
            profile: typeof webProfileProvider !== "undefined" ? webProfileProvider.ensure() : readerProfile
            visible: win.visible && win.visibility !== Window.Minimized
            backgroundColor: view.look.bg
            function updateLifecycleState() {
                // Keep WebEngine active whenever an EPUB is open (or draining close()),
                // without waiting for readerReveal > 0.01. QtWebEngine refuses
                // Active -> Frozen while visible in the scene graph (e.g. close fade),
                // so freeze only once hidden and idle.
                const actuallyVisible = visible && webLoader.visible && view.visible
                                        && (!view.parent || view.parent.visible)
                const wantActive = (reader.open && !view.pdf) || view.epubClosing
                                   || actuallyVisible || narration.active || download.active
                const target = wantActive
                    ? WebEngineView.LifecycleState.Active : WebEngineView.LifecycleState.Frozen
                if (lifecycleState !== target)
                    lifecycleState = target
            }
            onVisibleChanged: updateLifecycleState()
            Component.onCompleted: updateLifecycleState()
            Connections {
                target: webLoader
                function onVisibleChanged() { webItem.updateLifecycleState() }
            }
            Connections {
                target: view
                function onVisibleChanged() { webItem.updateLifecycleState() }
            }
            Connections {
                target: view.parent
                enabled: !!view.parent
                function onVisibleChanged() { webItem.updateLifecycleState() }
            }
            Connections {
                target: reader
                function onBookChanged() { webItem.updateLifecycleState() }
            }
            // Chromium only takes the mouse (selecting words, a tap on the
            // margin) when it has focus; the window's shortcuts still come first.
            activeFocusOnPress: true
            webChannel: channel
            url: "clothbound://local/app/reader.html"
            settings.javascriptCanOpenWindows: false
            settings.localContentCanAccessRemoteUrls: false
            settings.localContentCanAccessFileUrls: false
            settings.pluginsEnabled: false
            settings.pdfViewerEnabled: false
            settings.focusOnNavigationEnabled: false
            settings.showScrollBars: false
            settings.errorPageEnabled: false
            settings.spatialNavigationEnabled: false
            settings.linksIncludedInFocusChain: false
            // Clothbound's own menu for the page: listening from the place clicked,
            // and the selection's highlight, note and copy.
            onContextMenuRequested: (request) => {
                request.accepted = true
                const at = Qt.point(request.position.x, request.position.y)
                view.runWebJs("JSON.stringify(clothbound.placeAt(" + at.x + "," + at.y + "))", function(result) {
                    const place = JSON.parse(result || "null")
                    if (!place)
                        return
                    pageMenu.place = place
                    pageMenu.popup(webItem, at.x, at.y)
                })
            }
            onNewWindowRequested: (request) => {}
            onNavigationRequested: (request) => {
                const u = request.url.toString()
                if (u.startsWith("clothbound://"))
                    return
                if (!request.isMainFrame && (u === "about:blank" || u === "about:srcdoc"))
                    return
                request.reject()
            }
            onJavaScriptConsoleMessage: (level, message, line, source) => {
                if (level === WebEngineView.ErrorMessageLevel || debugMode)
                    console.warn(source.split("/").pop() + ":" + line + ": " + message)
            }
            onLoadingChanged: (request) => {
                if (debugMode || request.status === WebEngineView.LoadFailedStatus)
                    console.warn("reader surface:", request.status, request.url, request.errorString)
            }
            onRenderProcessTerminated: (status, code) => {
                view.clearSurfaceNudge()
                view.surfaceReady = false
                view.decoratedChapter = -1
                reload()
            }
        }
    }

    WebChannel {
        id: channel
        Component.onCompleted: registerObjects({ bridge: reader })
    }

    // While a modal panel or dialog is open the page underneath is left alone:
    // the wheel does nothing there, and a click outside closes what's open
    // (a note being written is never thrown away by a stray click; docked
    // Companion leaves the reading stage interactive).
    Item {
        id: scrim
        z: 8
        anchors.fill: parent
        readonly property bool dimmed: noteEditor.open || goTo.open
        property real dimOpacity: dimmed ? 1 : 0
        Behavior on dimOpacity {
            NumberAnimation {
                duration: theme.reducedMotion ? 90 : (scrim.dimmed ? 200 : 150)
                easing.type: scrim.dimmed ? Easing.OutCubic : Easing.OutQuad
            }
        }
        visible: view.modalPanelOpen || dimOpacity > 0.01
        Rectangle {
            anchors.fill: parent
            opacity: scrim.dimOpacity
            color: view.look.dark
                 ? Qt.rgba(6 / 255, 6 / 255, 8 / 255, 0.50)
                 : Qt.rgba(24 / 255, 20 / 255, 14 / 255, 0.42)
        }
        MouseArea {
            anchors.fill: parent
            enabled: view.modalPanelOpen
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
            onPressed: (mouse) => {
                mouse.accepted = true
                if (!noteEditor.open)
                    view.closePanels()
            }
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    // --- Chrome timer (2000 ms, paused while menus/popovers are open) --------
    Timer {
        id: chromeTimer
        interval: 2000
        onTriggered: {
            if (!readerChrome.chromeHovered && !readerChrome.scrubberHovered && !readerChrome.scrubberDragging
                && !view.chromeFocused && !view.propBool(listenBar, "menuOpen") && !readerChrome.listenOfferOpen
                && !menus.anyOpened && !selectionBar.visible && !readerChrome.keysStripWanted)
                view.chromeShown = false
        }
    }

    Loader {
        id: listenBar
        active: narration.active && view.enabled
        readonly property var loaded: item
        readonly property bool menuOpen: loaded ? !!loaded.menuOpen : false
        // Preserve height on Loader for Main.qml's Toast bottomClearance binding.
        height: loaded ? loaded.height : 0
        sourceComponent: ListenBar {
            parent: view
            z: 9
            showBookInfo: false
            // Centered in the room the side panels leave (VoicePanel is a centered modal sheet).
            readonly property real leftInset: contents.open ? contents.width : 0
            readonly property real rightInset: marksPanel.open ? marksPanel.width
                                             : companionPanel.open ? companionPanel.width
                                             : appearance.open ? appearance.width : 0
            availableWidth: Math.max(320, view.width - leftInset - rightInset)
            animateSize: !view.companionResizing

            // Animate horizontal center for panel open/close (paused during live
            // Companion resize) and slide progress for narration start/stop, so
            // the pill's own width/height morph expands symmetrically upward with
            // its bottom edge locked 14 px above the viewport floor.
            property real centerX: leftInset + (view.width - leftInset - rightInset) / 2
            Behavior on centerX {
                enabled: !theme.reducedMotion && !view.companionResizing
                NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
            }
            property real showProgress: narration.active ? 1 : 0
            Behavior on showProgress {
                enabled: !theme.reducedMotion
                NumberAnimation { duration: 240; easing.type: Easing.OutQuart }
            }
            x: Math.round(centerX - width / 2)
            y: Math.round((view.height - height - 14) + (1 - showProgress) * (height + 30))
            visible: showProgress > 0.005
            look: view.look
            onFollowRequested: narration.following = true
            onVoiceRequested: voicePanel.open ? voicePanel.close() : view.showVoices()
        }
    }

    ReaderMenus {
        id: menus
        look: view.look
        pdf: view.pdf
        selection: view.selection
        pageBookmark: view.pageBookmark
        onListenFromPlace: (chapter, offset) => {
            view.leaveVoice()
            narration.start(chapter, offset)
        }
        onHighlightSelection: {
            marks.addHighlight(view.selection.chapter, view.selection.start, view.selection.end,
                               view.selection.text, "yellow", reader.titleOfChapter(view.selection.chapter))
            view.surfaceClearSelection()
            selectionBar.visible = false
        }
        onNoteSelection: {
            const id = marks.addHighlight(view.selection.chapter, view.selection.start, view.selection.end,
                                          view.selection.text, "yellow", reader.titleOfChapter(view.selection.chapter))
            view.surfaceClearSelection()
            selectionBar.visible = false
            noteEditor.show(id)
        }
        onCopySelection: {
            reader.copyText(view.selection.text)
            view.surfaceClearSelection()
            selectionBar.visible = false
            toast.show(qsTr("Copied"))
        }
        onAskCompanion: (feature, target) => view.askCompanion(feature, target)
        onIllustrateRequested: (fromSelection, place) => {
            view.showCompanion("pictures")
            if (fromSelection)
                illustrator.illustrate(view.selection.text, view.selection.chapter, view.selection.end)
            else
                illustrator.illustrate("", place.chapter, place.offset + 600)
        }
        onToggleSnip: view.toggleSnipMode()
        onCopyCitation: (style) => view.copyCitation(style)
        onToggleBookmark: view.toggleBookmark()
        onShowContents: view.showContents()
        onShowMarks: view.showMarks()
        onShowSearch: searchBar.show()
        onShowGoTo: goTo.show()
        onToggleFullScreen: win.toggleFullScreen()
        onToastMessage: (msg) => toast.show(msg)
    }

    ReaderPanels {
        id: panels
        z: 10
        anchors.fill: parent
        look: view.look
        activeSettings: activeSettings
        pdf: view.pdf
        pdfSurface: view.pdfSurface
        readingStage: readingStage
        debugModeEnabled: debugMode
        onFocusPageRequested: view.focusPage()
        onGoToRequested: (target) => view.goTo(target)
        onSampleTextRequested: (callback) => view.surfaceSampleText(callback)
        onStartListeningRequested: view.startListening()
        onAppearanceSettingChanged: (key, value) => view.setAppearance(key, value)
        onPdfFitChosen: (mode) => view.setPdfFit(mode, false)
        onBookOverrideToggled: (enabled) => view.setBookOverride(enabled)
        onSearchHitJumped: (hit) => view.showFound(hit)
        onSaveAsNoteRequested: (quote, note) => {
            if (!reader.open)
                return
            const trimmedQuote = (quote || "").trim()
            const trimmedNote = (note || "").trim()
            if (trimmedQuote === "" && trimmedNote === "")
                return
            const matchTarget = view.selection && view.selection.text && view.selection.text.trim() === trimmedQuote
                ? view.selection
                : (view.lastCompanionTarget && view.lastCompanionTarget.text && view.lastCompanionTarget.text.trim() === trimmedQuote
                   ? view.lastCompanionTarget : null)
            let id = 0
            if (matchTarget && matchTarget.end > matchTarget.start) {
                id = marks.addHighlight(matchTarget.chapter, matchTarget.start, matchTarget.end,
                                        trimmedQuote, "blue", reader.titleOfChapter(matchTarget.chapter))
            } else {
                const label = reader.titleOfChapter(reader.chapter)
                const excerpt = trimmedQuote !== "" ? trimmedQuote : (reader.snippet !== "" ? reader.snippet : label)
                id = marks.addBookmark(reader.chapter, reader.pageStart, excerpt, label)
            }
            if (id > 0) {
                if (trimmedNote !== "")
                    marks.setNote(id, trimmedNote)
                toast.show(qsTr("Saved as note"))
            }
        }
    }

    // Inline EPUB image/SVG lightbox with async image loading, zoom, pan, and caption.
    Loader {
        id: lightbox
        anchors.fill: parent
        z: 50
        active: false
        readonly property var loaded: item
        readonly property bool open: loaded ? !!loaded.open : false
        readonly property real zoom: loaded ? loaded.zoom : 1.0
        function openImage(src, cap) {
            if (!src || src === "")
                return
            active = true
            if (loaded)
                loaded.openImage(src, cap)
        }
        function close() {
            if (loaded)
                loaded.close()
        }
        function setZoom(next) {
            if (loaded)
                loaded.setZoom(next)
        }
        sourceComponent: ReaderLightbox {
            anchors.fill: parent
            onClosed: view.focusPage()
        }
    }

    // --- Keys ----------------------------------------------------------------
    readonly property bool modalOpen: win.helpVisible || settingsView.open || welcome.open || (typeof commandPalette !== "undefined" && commandPalette.open)
    readonly property bool keys: reader.open && enabled && visible && !modalPanelOpen && !modalOpen && !win.typing && !lightbox.open && !(pdf && pdfSurface && pdfSurface.snipMode)
    // Listening keys work with a panel open too, unless a field is taking typing.
    readonly property bool typing: win.typing
    readonly property bool listenKeys: reader.open && enabled && visible && !modalOpen && !win.typing && !lightbox.open
    Shortcut {
        sequence: "C"
        context: Qt.ApplicationShortcut
        enabled: view.keys || (reader.open && view.enabled && view.visible && companionPanel.open && !view.modalOpen && !win.typing && !lightbox.open)
        onActivated: companionPanel.open ? companionPanel.close() : view.showCompanion("ask")
    }
    Shortcut { sequence: "X"; context: Qt.ApplicationShortcut; enabled: reader.open && view.enabled && view.visible && view.pdf && !view.modalPanelOpen && !view.modalOpen && !win.typing && !lightbox.open; onActivated: view.toggleSnipMode() }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: reader.rtl ? view.prev() : view.next() }
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: reader.rtl ? view.next() : view.prev() }
    Shortcut { sequence: "Down"; context: Qt.ApplicationShortcut; enabled: view.keys && view.pdf; onActivated: view.scrollVertical(1) }
    Shortcut { sequence: "Up"; context: Qt.ApplicationShortcut; enabled: view.keys && view.pdf; onActivated: view.scrollVertical(-1) }
    Shortcut { sequence: "L"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.next() }
    Shortcut { sequence: "H"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.prev() }
    Shortcut { sequences: ["PgDown", "J", "Space"]; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.pageStep(1) }
    Shortcut { sequences: ["PgUp", "K", "Shift+Space"]; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.pageStep(-1) }
    Shortcut { sequence: "]"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.nextChapter() }
    Shortcut { sequence: "["; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.prevChapter() }
    Shortcut { sequence: "Alt+Left"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.back() }
    Shortcut { sequence: "Alt+Right"; context: Qt.ApplicationShortcut; enabled: view.keys && reader.canGoForward; onActivated: view.forward() }
    Shortcut { sequence: "Shift+Left"; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: if (typeof narration.skipSeconds === "function") narration.skipSeconds(-15) }
    Shortcut { sequence: "Shift+Right"; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: if (typeof narration.skipSeconds === "function") narration.skipSeconds(15) }
    Shortcut {
        sequence: "T"
        context: Qt.ApplicationShortcut
        enabled: view.keys || (reader.open && view.enabled && view.visible && contents.open && !view.modalOpen && !win.typing && !lightbox.open)
        onActivated: contents.open ? contents.close() : view.showContents()
    }
    Shortcut {
        sequence: "A"
        context: Qt.ApplicationShortcut
        enabled: view.keys || (reader.open && view.enabled && view.visible && appearance.open && !view.modalOpen && !win.typing && !lightbox.open)
        onActivated: appearance.open ? appearance.close() : view.showAppearance()
    }
    Shortcut { sequence: "G"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: goTo.show() }
    Shortcut { sequence: "B"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.toggleBookmark() }
    Shortcut {
        sequence: "M"
        context: Qt.ApplicationShortcut
        enabled: view.keys || (reader.open && view.enabled && view.visible && marksPanel.open && !view.modalOpen && !win.typing && !lightbox.open)
        onActivated: marksPanel.open ? marksPanel.close() : view.showMarks()
    }
    Shortcut { sequences: [StandardKey.Redo]; context: Qt.ApplicationShortcut; enabled: view.keys && marks.canRedo; onActivated: marks.redo() }
    Shortcut { sequences: [StandardKey.Copy]; context: Qt.ApplicationShortcut; enabled: reader.open && view.enabled && view.visible && view.pdf && !!pdfSurface && !!pdfSurface.selection && !view.modalOpen && !win.typing; onActivated: { reader.copyText(pdfSurface.selection.text); toast.show(qsTr("Copied")) } }
    Shortcut { sequences: ["/", StandardKey.Find]; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: searchBar.show() }
    Shortcut { sequence: "N"; context: Qt.ApplicationShortcut; enabled: view.keys && bookSearch.count > 0; onActivated: searchBar.next() }
    Shortcut { sequence: "Shift+N"; context: Qt.ApplicationShortcut; enabled: view.keys && bookSearch.count > 0; onActivated: searchBar.previous() }
    Shortcut { sequence: "D"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.cycleTheme() }
    Shortcut { sequences: ["+", "=", "Ctrl++", "Ctrl+="]; context: Qt.ApplicationShortcut; enabled: reader.open && view.enabled && view.visible && !view.modalOpen && !win.typing; onActivated: view.resize(0.05) }
    Shortcut { sequences: ["-", "Ctrl+-"]; context: Qt.ApplicationShortcut; enabled: reader.open && view.enabled && view.visible && !view.modalOpen && !win.typing; onActivated: view.resize(-0.05) }
    Shortcut { sequences: ["0", "Ctrl+0"]; context: Qt.ApplicationShortcut; enabled: reader.open && view.enabled && view.visible && !view.modalOpen && !win.typing; onActivated: view.resize(0) }
    Shortcut { sequence: "W"; context: Qt.ApplicationShortcut; enabled: view.keys && view.pdf; onActivated: view.setPdfFit("width", true) }
    Shortcut { sequences: ["1", "Ctrl+1"]; context: Qt.ApplicationShortcut; enabled: view.keys && view.pdf; onActivated: view.setPdfFit("actual", true) }
    Shortcut { sequence: "Tab"; context: Qt.ApplicationShortcut; enabled: view.keys && view.readingViewportFocused; onActivated: view.chromeShown ? (view.chromeShown = false) : view.showChrome() }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: reader.open && view.enabled && view.visible && !view.modalOpen; onActivated: view.dismiss() }
    // Listening.
    // P listens from the page in view, then pauses and goes on; ⇧P stops.
    Shortcut { sequence: "P"; context: Qt.ApplicationShortcut; enabled: view.listenKeys; onActivated: narration.active ? narration.toggle() : view.listen() }
    Shortcut { sequence: "Shift+P"; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: narration.stop() }
    Shortcut {
        sequence: "V"
        context: Qt.ApplicationShortcut
        enabled: view.keys || (reader.open && view.enabled && view.visible && voicePanel.open && !view.modalOpen && !win.typing && !lightbox.open)
        onActivated: voicePanel.open ? voicePanel.close() : view.showVoices()
    }
    Shortcut { sequence: ","; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: narration.previousParagraph() }
    Shortcut { sequence: "."; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: narration.nextParagraph() }
    Shortcut { sequences: ["<", "Shift+,"]; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: narration.rate = narration.rate - 0.1 }
    Shortcut { sequences: [">", "Shift+."]; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: narration.rate = narration.rate + 0.1 }
    Shortcut { sequence: "Media Play"; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: narration.resume() }
    Shortcut { sequence: "Media Pause"; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: narration.pause() }
    Shortcut { sequence: "Toggle Media Play/Pause"; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: narration.toggle() }
}
