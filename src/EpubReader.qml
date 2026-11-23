import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtWebChannel
import QtWebEngine

import "Format.js" as Format
import "Icons.js" as Icons
import "Themes.js" as Themes
import "Ui.js" as Ui

// Reading a book: the page edge to edge in the reading theme, a quiet
// running head and folio, and everything else one key away. The page itself
// is the web surface (reader.html), driven through `dogear`.
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
    }
    Settings {
        id: folioSettings
        location: settingsUrl
        category: "typography"
        // 0: time left in chapter, 1: time left in book, 2: % · time left
        property int folioMode: 0
    }

    readonly property var look: Themes.palette(activeSettings.theme, theme)
    // For driving the panels from outside (CommandPalette and tests).
    readonly property alias contentsPanel: contents
    readonly property alias appearancePanel: appearance
    readonly property alias goToBox: goTo
    readonly property alias voicePanel: voicePanel
    readonly property alias searchBar: searchBar
    readonly property alias pageMenu: pageMenu
    // The words selected on the page, as the page last said ({} for none).
    property var selection: ({})
    property var lastCompanionTarget: null
    readonly property alias marksPanel: marksPanel
    readonly property alias companionPanel: companionPanel
    readonly property alias noteEditor: noteEditor
    // Companion docks beside the page on wide screens without a blocking scrim (04_companion).
    readonly property bool companionDocked: companionPanel.open && view.width >= 860
    readonly property real companionDockWidth: companionDocked ? Math.min(384, companionPanel.width) : 0
    readonly property bool modalPanelOpen: contents.open || appearance.open || goTo.open || voicePanel.open || searchBar.editing
                                           || marksPanel.open || noteEditor.open || (companionPanel.open && !companionDocked)
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
    readonly property real footerInset: Math.max(24, Math.min(144, Math.round(readingStage.width * 0.1125)))

    function propBool(obj, key) {
        return !!(obj && obj[key])
    }

    onCompanionDockedChanged: styleTimer.restart()

    signal closeRequested()

    // The reading theme as the palette of every panel, menu and control in
    // the reader (the roles are in src/style/Dogear/README.md).
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

    // The surface's calls (dogear.next(), …): the web page's for an EPUB,
    // the PDF surface's (which answers the same calls) for a PDF.
    function js(code, callback) {
        if (pdf) {
            // (QML keeps "escape" and "marks" for its own: the surface calls them
            // dismiss and setMarks.)
            const call = code.replace("dogear.escape(", "dogear.dismiss(").replace("dogear.marks(", "dogear.setMarks(")
            const result = (new Function("dogear", "return (" + call + ")"))(pdfSurface)
            if (callback)
                callback(result)
            return
        }
        if (!web)
            return
        if (callback)
            web.runJavaScript(code, callback)
        else
            web.runJavaScript(code)
    }
    function style() {
        const margins = { narrow: [0.055, 36], normal: [0.1125, 30.5], wide: [0.155, 26] }[activeSettings.margins] || [0.1125, 30.5]
        const stageH = readingStage.height > 0 ? readingStage.height : view.height
        const topMargin = narration.active
            ? Math.max(68, Math.min(92, Math.round(stageH * 0.115)))
            : Math.max(54, Math.min(92, Math.round(stageH * 0.115)))
        const bottomMargin = narration.active
            ? Math.max(136, Math.min(148, Math.round(stageH * 0.185)))
            : Math.max(54, Math.min(104, Math.round(stageH * 0.13)))
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
            marginTop: topMargin,
            // Room for the bottom footer or the floating two-row player.
            marginBottom: bottomMargin,
            radius: theme.radius,
            listening: narration.active,
            strings: { goToNote: qsTr("Go to note"), listenFromHere: qsTr("Listen from here") },
        }
    }
    function openBook() {
        if (!reader.open)
            return
        clearSurfaceNudge()
        surfaceWatcher.resetSurfaceNudgeCooldown()
        if (!pdf)
            webNeeded = true
        if (!surfaceReady && !pdf)
            return
        decoratedChapter = -1
        jumpPillVisible = false
        lightbox.close()
        js("dogear.open(" + JSON.stringify(reader.manifest) + "," + JSON.stringify(reader.startPosition) + ","
           + JSON.stringify(style()) + ")")
    }
    function closeBook() {
        clearSurfaceNudge()
        surfaceWatcher.resetSurfaceNudgeCooldown()
        refreshTimer.stop()
        refreshLater.stop()
        pendingStartListening = false
        listenOffer.open = false
        closePanels()
        lightbox.close()
        jumpPillVisible = false
        selectionBar.visible = false
        view.selection = ({})
        view.lastCompanionTarget = null
        decoratedChapter = -1
        if (pdf)
            pdfSurface.clearSelection()
        else if (surfaceReady && web)
            js("dogear.close()")
    }
    // Turning pages by hand while listening leaves the voice where it is;
    // "Back to the voice" brings the page back to it.
    function leaveVoice() {
        if (narration.active)
            narration.following = false
    }
    function next() { leaveVoice(); js("dogear.next()") }
    function prev() { leaveVoice(); js("dogear.prev()") }
    function nextChapter() { leaveVoice(); js("dogear.nextChapter()") }
    function prevChapter() { leaveVoice(); js("dogear.prevChapter()") }
    function back() { leaveVoice(); jumpPillVisible = false; js("dogear.back()") }
    function forward() { leaveVoice(); jumpPillVisible = false; js("dogear.forward()") }
    function goTo(target) { leaveVoice(); js("dogear.goTo(" + JSON.stringify(target) + ")") }

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
        js("JSON.stringify(dogear.listeningStart())", function(result) {
            const place = JSON.parse(result || "null")
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
        if (pdf)
            pdfSurface.forceActiveFocus()
        else if (web)
            web.forceActiveFocus()
    }
    function dismiss() {
        if (lightbox.open) {
            lightbox.close()
            focusPage()
            return
        }
        // Close ListenOffer on Esc without cancelling an active voice download.
        if (listenOffer.open) {
            listenOffer.open = false
            focusPage()
            return
        }
        if (panelOpen) {
            closePanels()
            return
        }
        if (selectionBar.visible) {
            selectionBar.visible = false
            js("dogear.clearSelection()")
            return
        }
        if (searchBar.open) {
            searchBar.close()
            return
        }
        js("dogear.escape()", function(closed) {
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
    function resize(step) {
        if (lightbox.open) {
            lightbox.setZoom(step === 0 ? 1.0 : lightbox.zoom * (step > 0 ? 1.25 : 1 / 1.25))
            return
        }
        // A PDF's text is its pages: it zooms.
        if (pdf) {
            pdfSurface.zoom = step === 0 ? 1 : Math.max(1, Math.min(4, pdfSurface.zoom * (step > 0 ? 1.15 : 1 / 1.15)))
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
        js("dogear.found(" + JSON.stringify(bookSearch.inChapter(reader.chapter)) + "," + JSON.stringify(current) + ")")
    }
    function showFound(hit) {
        leaveVoice()
        js("dogear.goTo(" + JSON.stringify({ chapter: hit.chapter, offset: hit.start, instant: true }) + ")"
           + ".then(() => dogear.found(" + JSON.stringify(bookSearch.inChapter(hit.chapter)) + ","
           + JSON.stringify({ chapter: hit.chapter, start: hit.start, end: hit.end }) + "))")
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
            js("dogear.marks(" + JSON.stringify(marks.highlightsIn(reader.chapter)) + ")")
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
                    view.js("dogear.speak(" + narration.sentenceStart + "," + narration.sentenceEnd + ",false)")
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
    function showCompanion(tab) {
        contents.close()
        appearance.close()
        voicePanel.close()
        marksPanel.close()
        companionPanel.show(tab)
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
        showCompanion("ask")
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

    // Typography and theme changes reach the page together, once.
    Timer {
        id: styleTimer
        interval: 50
        onTriggered: if ((view.surfaceReady || view.pdf) && reader.open) view.js("dogear.setStyle(" + JSON.stringify(view.style()) + ")")
    }
    onLookChanged: styleTimer.restart()
    Connections {
        target: reader
        function onAppearanceChanged() { styleTimer.restart() }
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
        function onLayoutChanged() { styleTimer.restart() }
        function onAnimateChanged() { styleTimer.restart() }
        function onPublisherChanged() { styleTimer.restart() }
        function onPdfCropChanged() { styleTimer.restart() }
        function onPdfOriginalColorsChanged() { styleTimer.restart() }
    }

    Connections {
        target: download
        function onBlocksWanted(chapter) { view.js("dogear.requestSpeakable(" + chapter + ")") }
        function onReady() { toast.show(qsTr("Ready to listen offline")) }
        function onChanged() { if (view.web) view.web.updateLifecycleState() }
    }
    Connections {
        target: narration
        function onBlocksWanted(chapter) { view.js("dogear.requestSpeakable(" + chapter + ")") }
        function onSpeaking(chapter, start, end) {
            // A PDF shows two pages at once and finds its own way to the page.
            if (chapter === reader.chapter || view.pdf)
                view.js("dogear.speak(" + start + "," + end + "," + narration.following + ")")
            else if (narration.following)
                view.js("dogear.goTo({chapter:" + chapter + ",offset:" + start + ",instant:true,quiet:true,automated:true})"
                        + ".then(() => dogear.speak(" + start + "," + end + ",true))")
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
                view.js("dogear.highlight('dg-speaking', [])")
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
    // Dogear returns tiled and unfocused. A single coalesced 1 px bottom-margin
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
                view.js("dogear.endSurfaceNudge && dogear.endSurfaceNudge()")
        }
    }

    function runSurfacePaint() {
        if (!reader.open || !win.visible)
            return
        if (pdf) {
            surfaceWatcher.forceItemUpdate(pdfSurface)
            return
        }
        if (!surfaceReady || !web)
            return
        surfaceWatcher.forceItemUpdate(web)
        view.js("dogear.refresh && dogear.refresh()")
    }

    function refreshSoon(reason) {
        if (!reader.open || !win.visible)
            return
        if (pdf) {
            surfaceWatcher.forceItemUpdate(pdfSurface)
            return
        }
        if (!surfaceReady || !web)
            return
        surfaceWatcher.forceItemUpdate(web)
        if (!surfaceNudge && !epubLoading && surfaceWatcher.takeSurfaceNudge(reason)) {
            view.js("dogear.beginSurfaceNudge && dogear.beginSurfaceNudge()")
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
                view.js("dogear.endSurfaceNudge && dogear.endSurfaceNudge()")
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
            if (reader.open) {
                if (reader.format === "pdf")
                    win.tip("pdf", qsTr("PDF pages follow your reading theme. + and − zoom, and the layout (A) shows one page, two, or scrolling."))
                else
                    view.webNeeded = true
                view.closePanels()
                // J3 first book: reveal the top bar gently alongside the tips bar.
                view.chromeShown = onboarding.tips.indexOf("keys") < 0
                if (view.chromeShown)
                    chromeTimer.restart()
                view.openBook()
            }
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
        if (win.helpVisible || settingsView.open || welcome.open || win.typing)
            return
        const shift = (modifiers & 1) !== 0, ctrl = (modifiers & 2) !== 0, alt = (modifiers & 4) !== 0, meta = (modifiers & 8) !== 0
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
        else if (key === "PageDown" || key === "l" || key === "j" || (key === " " && !shift)) next()
        else if (key === "PageUp" || key === "h" || key === "k" || (key === " " && shift)) prev()
        else if (key === "]") nextChapter()
        else if (key === "[") prevChapter()
        else if (key === "t" || key === "T") showContents()
        else if (key === "a" || key === "A") showAppearance()
        else if (key === "g" || key === "G") goTo.show()
        else if (key === "b" || key === "B") toggleBookmark()
        else if (key === "m" || key === "M") showMarks()
        else if (key === "c" || key === "C") showCompanion("ask")
        else if (key === "v" || key === "V") showVoices()
        else if (key === "/") searchBar.show()
        else if (key === "n" && bookSearch.count > 0) searchBar.next()
        else if (key === "N" && bookSearch.count > 0) searchBar.previous()
        else if (key === "Tab") view.chromeShown ? (view.chromeShown = false) : view.showChrome()
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
        else if (key === "f" || key === "F" || key === "F11") win.toggleFullScreen()
        else if (key === "?") win.helpVisible = !win.helpVisible
        else if (key === "q" || key === "Q") Qt.quit()
    }

    Rectangle {
        anchors.fill: parent
        color: view.look.bg
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
        anchors.rightMargin: view.companionDockWidth

        PdfSurface {
            id: pdfSurface
            anchors.fill: parent
            visible: view.pdf
            look: view.look
            focus: view.pdf
            onContextMenuRequested: (x, y) => {
                const place = pdfSurface.placeAt(x, y)
                if (!place)
                    return
                pageMenu.place = place
                pageMenu.popup(pdfSurface, x, y)
            }
        }

        Loader {
            id: webLoader
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.bottomMargin: view.surfaceNudge && parent.height > 2 ? 1 : 0
            active: view.webNeeded
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
            Behavior on opacity { NumberAnimation { duration: 180 } }
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

        // In scroll mode the text passes under the top/bottom margins;
        // it fades into the page there rather than colliding with chrome.
        Rectangle {
            visible: activeSettings.layout === "scroll"
            anchors.top: parent.top
            width: parent.width
            height: 52
            gradient: Gradient {
                GradientStop { position: 0.45; color: view.look.bg }
                GradientStop { position: 1; color: Ui.tint(view.look.bg, 0) }
            }
        }
        Rectangle {
            visible: activeSettings.layout === "scroll"
            anchors.bottom: parent.bottom
            width: parent.width
            height: 52
            gradient: Gradient {
                GradientStop { position: 0; color: Ui.tint(view.look.bg, 0) }
                GradientStop { position: 0.55; color: view.look.bg }
            }
        }

        // Reveal reading chrome on any pointer movement (2 s idle fade, 09:628).
        property real lastPointerX: -1
        property real lastPointerY: -1
        HoverHandler {
            id: pointerWatch
            onPointChanged: {
                const px = point.position.x, py = point.position.y
                if (readingStage.lastPointerX >= 0 && !view.modalPanelOpen && !narration.active && !lightbox.open) {
                    const dist = Math.hypot(px - readingStage.lastPointerX, py - readingStage.lastPointerY)
                    if (dist >= 6)
                        view.showChrome()
                }
                readingStage.lastPointerX = px
                readingStage.lastPointerY = py
            }
        }

        // --- Top Reading Chrome (02_reading.html:525-547) --------------------
        Item {
            id: chrome
            z: 3
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 48
            opacity: view.chromeShown ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity {
                NumberAnimation { duration: theme.reducedMotion ? 80 : 180; easing.type: Easing.OutCubic }
            }
            HoverHandler { id: chromeHover }

            Row {
                id: leftTools
                anchors.left: parent.left
                anchors.leftMargin: 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                // Back to the library, as words: the way out is never a guess.
                AbstractButton {
                    id: backButton
                    anchors.verticalCenter: parent.verticalCenter
                    height: 30
                    width: backRow.implicitWidth + 16
                    hoverEnabled: true
                    focusPolicy: Qt.NoFocus
                    Accessible.name: qsTr("Library")
                    onClicked: view.closeRequested()
                    background: Rectangle {
                        radius: Ui.radius(7, theme.radius)
                        color: backButton.pressed ? Ui.tint(view.look.fg, 0.12)
                             : backButton.hovered ? Ui.tint(view.look.fg, 0.07) : "transparent"
                    }
                    contentItem: Item {
                        Row {
                            id: backRow
                            x: 6
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 4
                            Glyph {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 16
                                height: 16
                                stroke: 2
                                path: Icons.chevronLeft
                                color: view.look.inkSoft || view.look.fg
                            }
                            Label {
                                anchors.verticalCenter: parent.verticalCenter
                                text: qsTr("Library")
                                color: view.look.inkSoft || view.look.fg
                                font.pixelSize: 13
                                font.weight: Font.Medium
                            }
                        }
                    }
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Library (Esc)")
                    ToolTip.delay: 600
                }

                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 32
                    implicitHeight: 30
                    tabFocusable: false
                    look: view.look
                    glyph: Icons.contents
                    active: contents.open
                    tip: qsTr("Contents (T)")
                    onClicked: contents.open ? contents.close() : view.showContents()
                }
            }

            // The book, and where in it: "The Time Machine — Chapter I".
            Item {
                id: heading
                anchors.centerIn: parent
                readonly property real sideReserve: Math.max(leftTools.width + 24, tools.width + tools.anchors.rightMargin + 14)
                readonly property real room: Math.max(0, parent.width - 2 * sideReserve)
                readonly property bool showChapter: reader.chapterTitle !== "" && reader.chapterTitle !== reader.title
                TextMetrics { id: titleMetrics; text: reader.title; font: titleLabel.font }
                TextMetrics { id: chapterMetrics; text: " — " + reader.chapterTitle; font: chapterLabel.font }
                readonly property real chapterRoom: showChapter ? Math.min(chapterMetrics.advanceWidth + 2, room * 0.45) : 0
                width: titleLabel.width + chapterLabel.width
                height: parent.height
                Label {
                    id: titleLabel
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.min(titleMetrics.advanceWidth + 2, heading.room - heading.chapterRoom)
                    text: reader.title
                    color: view.look.inkSoft || view.look.fg
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    font.letterSpacing: 0.12
                    elide: Text.ElideRight
                }
                Label {
                    id: chapterLabel
                    anchors.left: titleLabel.right
                    anchors.verticalCenter: parent.verticalCenter
                    visible: heading.showChapter
                    width: visible ? Math.min(chapterMetrics.advanceWidth + 2, heading.room - titleLabel.width) : 0
                    text: " — " + reader.chapterTitle
                    color: view.look.inkSoft || view.look.fg
                    opacity: 0.55
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    font.letterSpacing: 0.12
                    elide: Text.ElideRight
                }
            }

            Row {
                id: tools
                anchors.right: parent.right
                anchors.rightMargin: view.pageBookmark > 0 ? 52 : 14
                anchors.verticalCenter: parent.verticalCenter
                spacing: 4

                IconButton {
                    implicitWidth: 32
                    implicitHeight: 30
                    tabFocusable: false
                    look: view.look
                    glyph: Icons.search
                    active: searchBar.open
                    tip: qsTr("Search in book (/)")
                    onClicked: searchBar.open ? searchBar.close() : searchBar.show()
                }
                IconButton {
                    implicitWidth: 32
                    implicitHeight: 30
                    tabFocusable: false
                    look: view.look
                    glyph: Icons.bookmark
                    active: marksPanel.open
                    tip: qsTr("Highlights and notes (M)")
                    onClicked: marksPanel.open ? marksPanel.close() : view.showMarks()
                }
                IconButton {
                    implicitWidth: 32
                    implicitHeight: 30
                    tabFocusable: false
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
                        tabFocusable: false
                        look: view.look
                        glyph: Icons.headphones
                        tone: view.firstBookTipsActive ? view.look.link : (view.look.inkSoft || view.look.fg)
                        active: narration.active
                        tip: narration.active ? qsTr("Stop listening (⇧P)") : qsTr("Listen (P)")
                        onClicked: view.listen()
                    }
                }
                IconButton {
                    implicitWidth: 32
                    implicitHeight: 30
                    tabFocusable: false
                    look: view.look
                    glyph: Icons.sparkle
                    active: companionPanel.open
                    tip: qsTr("Companion (C)")
                    onClicked: companionPanel.open ? companionPanel.close() : view.showCompanion("ask")
                }
            }

            Menu {
                id: readerMore
                width: 260
                MenuItem { text: qsTr("Contents") + "\tT"; onTriggered: view.showContents() }
                MenuItem { text: qsTr("Highlights, notes, and bookmarks") + "\tM"; onTriggered: view.showMarks() }
                MenuItem { text: (view.pageBookmark ? qsTr("Remove the bookmark") : qsTr("Bookmark this page")) + "\tB"; onTriggered: view.toggleBookmark() }
                MenuItem { text: qsTr("Search in the book") + "\t/"; onTriggered: searchBar.show() }
                MenuItem { text: qsTr("Go to a page") + "\tG"; onTriggered: goTo.show() }
                MenuSeparator {}
                MenuItem { text: qsTr("Full screen") + "\tF"; onTriggered: win.toggleFullScreen() }
            }
        }

        // Top-center voice status / return pill while listening (03_listening.html:588-591).
        AbstractButton {
            id: followVoicePill
            z: 3
            anchors.top: parent.top
            anchors.topMargin: 24
            anchors.horizontalCenter: parent.horizontalCenter
            readonly property bool shown: narration.active && !view.chromeShown
            opacity: shown ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: theme.reducedMotion ? 80 : 160 } }
            implicitHeight: 30
            implicitWidth: followPillRow.implicitWidth + 22
            hoverEnabled: true
            focusPolicy: Qt.NoFocus
            enabled: shown && !narration.following
            onClicked: if (!narration.following) narration.following = true
            background: Rectangle {
                radius: Ui.radius(15, theme.radius)
                color: !narration.following
                     ? (followVoicePill.hovered ? Ui.tint(view.look.link, 0.22) : Ui.tint(view.look.link, 0.14))
                     : Ui.tint(view.look.fg, 0.06)
                border.width: 1
                border.color: !narration.following ? Ui.tint(view.look.link, 0.35) : Ui.tint(view.look.fg, 0.10)
            }
            contentItem: Item {
                Row {
                    id: followPillRow
                    anchors.centerIn: parent
                    spacing: 6
                    Glyph {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 13
                        height: 13
                        stroke: 2
                        path: Icons.waveform
                        color: !narration.following ? view.look.link : (view.look.inkSoft || view.look.fg)
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: narration.following ? qsTr("Pages follow the voice") : qsTr("Back to the voice")
                        color: !narration.following ? view.look.link : (view.look.inkSoft || view.look.fg)
                        font.pixelSize: 12
                        font.weight: Font.Medium
                    }
                }
            }
            HoverHandler { cursorShape: !narration.following ? Qt.PointingHandCursor : Qt.ArrowCursor }
        }

        // --- Unified Inset Bottom Footer (02_reading.html:576-595) -----------
        Item {
            id: bottomFooter
            z: 3
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: view.footerInset
            anchors.rightMargin: view.footerInset
            anchors.bottomMargin: 24
            height: footerCol.implicitHeight
            readonly property bool shown: view.chromeShown && !narration.active && !keysStrip.wanted
            opacity: shown ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity {
                NumberAnimation { duration: theme.reducedMotion ? 80 : 180; easing.type: Easing.OutCubic }
            }
            HoverHandler { id: scrubberHover }

            Column {
                id: footerCol
                width: parent.width
                spacing: 8

                ProgressScrubber {
                    id: progressScrubber
                    width: parent.width
                    look: view.look
                    onGo: (target) => view.goTo(target)
                }

                Item {
                    width: parent.width
                    height: 24

                    // Left: time left (tappable to cycle chapter / book / % · time left; never bare %).
                    AbstractButton {
                        id: folioLeftButton
                        anchors.left: parent.left
                        anchors.verticalCenter: parent.verticalCenter
                        enabled: bottomFooter.shown
                        hoverEnabled: true
                        focusPolicy: Qt.NoFocus
                        implicitWidth: folioLeftLabel.implicitWidth + 12
                        implicitHeight: 24
                        onClicked: folioSettings.folioMode = (folioSettings.folioMode + 1) % 3
                        background: Rectangle {
                            radius: Ui.radius(6, theme.radius)
                            color: folioLeftButton.hovered ? Ui.tint(view.look.fg, 0.06) : "transparent"
                        }
                        contentItem: Label {
                            id: folioLeftLabel
                            horizontalAlignment: Text.AlignLeft
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: 4
                            rightPadding: 4
                            text: {
                                void(folioSettings.folioMode)
                                void(reader.progress); void(reader.minutesLeftInChapter); void(reader.minutesLeftInBook)
                                void(reader.atEnd); void(translator.language)
                                return view.folioTimeText()
                            }
                            color: view.look.muted
                            font.pixelSize: 12
                            font.features: ({ "tnum": 1 })
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered
                        ToolTip.delay: 700
                        ToolTip.text: qsTr("Click to switch time remaining")
                    }

                    // Right: spread page range ("Pages 3–4 of 148"), tappable for Go To (G).
                    AbstractButton {
                        id: folioRightButton
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        enabled: bottomFooter.shown
                        hoverEnabled: true
                        focusPolicy: Qt.NoFocus
                        implicitWidth: folioRightLabel.implicitWidth + 12
                        implicitHeight: 24
                        onClicked: goTo.show()
                        background: Rectangle {
                            radius: Ui.radius(6, theme.radius)
                            color: folioRightButton.hovered ? Ui.tint(view.look.fg, 0.06) : "transparent"
                        }
                        contentItem: Label {
                            id: folioRightLabel
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: 4
                            rightPadding: 4
                            text: {
                                void(reader.bookPageStart); void(reader.bookPageEnd); void(reader.bookPages)
                                void(reader.pageLabel); void(translator.language)
                                return view.folioPagesText()
                            }
                            color: view.look.muted
                            font.pixelSize: 12
                            font.features: ({ "tnum": 1 })
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered
                        ToolTip.delay: 700
                        ToolTip.text: qsTr("Go to a page (G)")
                    }
                }
            }
        }

        // J3 First-book tips: dark 760 px floating bar shown once, then behind ? (12_j3_first_book:489-500).
        Rectangle {
            id: keysStrip
            z: 5
            readonly property bool wanted: view.firstBookTipsActive
            anchors.horizontalCenter: parent.horizontalCenter
            y: wanted ? parent.height - height - 40 : parent.height + 16
            Behavior on y {
                enabled: !theme.reducedMotion
                NumberAnimation { duration: 280; easing.type: Easing.OutQuart }
            }
            opacity: wanted ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 180 } }
            visible: opacity > 0.01 && y < parent.height
            width: Math.min(parent.width - 48, 760)
            height: 62
            radius: Ui.radius(14, theme.radius)
            color: Qt.rgba(28 / 255, 27 / 255, 25 / 255, 0.95)
            border.width: view.look.dark ? 1 : 0
            border.color: Qt.rgba(1, 1, 1, 0.08)

            // Warm elevation shadow (0 18px 50px rgba(40,30,10,0.28)).
            Rectangle {
                z: -2
                anchors.fill: parent
                anchors.leftMargin: -4
                anchors.rightMargin: -4
                anchors.topMargin: 10
                anchors.bottomMargin: -16
                radius: parent.radius > 0 ? parent.radius + 4 : 0
                color: view.look.dark ? "#000000" : "#281E0A"
                opacity: 0.24
            }
            Rectangle {
                z: -1
                anchors.fill: parent
                anchors.topMargin: 4
                anchors.bottomMargin: -6
                radius: parent.radius
                color: "#000000"
                opacity: 0.20
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 16
                spacing: 20
                Repeater {
                    model: [{ key: qsTr("Space"), what: qsTr("next page") },
                            { key: "P", what: qsTr("read aloud") },
                            { key: "C", what: qsTr("ask about it") },
                            { key: "?", what: qsTr("every key") }]
                    delegate: Row {
                        required property var modelData
                        Layout.alignment: Qt.AlignVCenter
                        spacing: 8
                        KeyHint {
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.key
                            keycap: true
                            onDark: true
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: modelData.what
                            color: "#CFC9BE"
                            font.pixelSize: 13
                        }
                    }
                }
                Item { Layout.fillWidth: true }
                AbstractButton {
                    id: gotItBtn
                    Layout.alignment: Qt.AlignVCenter
                    implicitHeight: 30
                    implicitWidth: gotItLabel.implicitWidth + 28
                    hoverEnabled: true
                    focusPolicy: Qt.NoFocus
                    onClicked: {
                        onboarding.tips = onboarding.tips.concat(["keys"])
                        chromeTimer.restart()
                    }
                    background: Rectangle {
                        radius: Ui.radius(7, theme.radius)
                        color: gotItBtn.pressed ? Qt.rgba(1, 1, 1, 0.20)
                             : gotItBtn.hovered ? Qt.rgba(1, 1, 1, 0.16) : Qt.rgba(1, 1, 1, 0.10)
                    }
                    contentItem: Label {
                        id: gotItLabel
                        text: qsTr("Got it")
                        color: "#F2EFE9"
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }

        // Dismissible jump-back pill shown after TOC, link, scrubber, search,
        // or Companion jumps so returning where you came from is one click away.
        Timer {
            id: jumpPillTimer
            interval: 12000
            onTriggered: view.jumpPillVisible = false
        }
        Rectangle {
            id: jumpPill
            z: 5
            anchors.left: parent.left
            anchors.leftMargin: 24 + (contents.open ? contents.width : 0)
            Behavior on anchors.leftMargin {
                enabled: !theme.reducedMotion
                NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
            }
            readonly property bool shown: view.jumpPillVisible && reader.canGoBack && view.jumpPillLabel !== ""
            y: shown ? parent.height - height - (narration.active ? 132 : bottomFooter.shown ? 86 : 36) : parent.height + 16
            Behavior on y {
                enabled: !theme.reducedMotion
                NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
            }
            opacity: shown ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 150 } }
            visible: opacity > 0.01
            width: jumpPillRow.implicitWidth + 16
            height: 36
            radius: Ui.radius(18, theme.radius)
            color: view.look.popover
            border.width: 1
            border.color: view.look.line

            Rectangle {
                z: -1
                anchors.fill: parent
                anchors.topMargin: 3
                anchors.bottomMargin: -3
                radius: parent.radius
                color: "black"
                opacity: view.look.dark ? 0.35 : 0.12
            }

            Row {
                id: jumpPillRow
                anchors.centerIn: parent
                spacing: 4

                AbstractButton {
                    id: jumpPillAction
                    anchors.verticalCenter: parent.verticalCenter
                    implicitHeight: 30
                    implicitWidth: jumpActionRow.implicitWidth + 12
                    hoverEnabled: true
                    focusPolicy: Qt.NoFocus
                    onClicked: view.back()
                    background: Rectangle {
                        radius: Ui.radius(15, theme.radius)
                        color: jumpPillAction.hovered ? Ui.tint(view.look.link, 0.14) : "transparent"
                    }
                    contentItem: Row {
                        id: jumpActionRow
                        spacing: 6
                        leftPadding: 6
                        rightPadding: 6
                        Glyph {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 14
                            height: 14
                            path: Icons.back
                            color: view.look.link
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Return to %1").arg(view.jumpPillLabel)
                            color: view.look.fg
                            font.pixelSize: 13
                            elide: Text.ElideRight
                            width: Math.min(implicitWidth, 240)
                        }
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: qsTr("Return (Alt+←)")
                }

                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 26
                    implicitHeight: 26
                    tabFocusable: false
                    glyph: Icons.close
                    look: view.look
                    tone: view.look.muted
                    tip: qsTr("Dismiss")
                    onClicked: view.jumpPillVisible = false
                }
            }
        }

        // The bookmark is a 44×44 tactile paper dog-ear in the top-right corner (02_reading:550).
        Item {
            id: dogEar
            z: 4
            anchors.top: parent.top
            anchors.right: parent.right
            width: 44
            height: 44
            visible: !view.modalPanelOpen || view.pageBookmark > 0
            property real fold: view.pageBookmark > 0 ? 1 : earHover.hovered ? 0.55 : 0
            Behavior on fold {
                enabled: !theme.reducedMotion
                NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
            }
            HoverHandler { id: earHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: view.toggleBookmark() }
            ToolTip.visible: earHover.hovered
            ToolTip.text: view.pageBookmark ? qsTr("Remove bookmark (B)") : qsTr("Bookmark this page (B)")
            ToolTip.delay: 500
            Canvas {
                anchors.fill: parent
                visible: dogEar.fold > 0.01
                property real fold: dogEar.fold
                property color page: view.look.bg
                property color flap: view.pageBookmark > 0
                                   ? (view.look.fold || Ui.mix(view.look.bg, view.look.fg, 0.12))
                                   : Ui.mix(view.look.bg, view.look.fg, 0.08)
                property bool darkPage: view.look.dark
                onFoldChanged: requestPaint()
                onPageChanged: requestPaint()
                onFlapChanged: requestPaint()
                onDarkPageChanged: requestPaint()
                onPaint: {
                    const g = getContext("2d")
                    g.reset()
                    const s = width * fold
                    const x0 = width - s
                    // Folded corner matches 02_reading: linear-gradient(225deg, paper 50%, fold 50%).
                    g.fillStyle = page
                    g.beginPath(); g.moveTo(x0, 0); g.lineTo(width, 0); g.lineTo(width, s); g.closePath(); g.fill()
                    g.shadowColor = Qt.rgba(0, 0, 0, darkPage ? 0.24 : 0.08)
                    g.shadowBlur = 4
                    g.shadowOffsetX = -2
                    g.shadowOffsetY = 2
                    g.fillStyle = flap
                    g.beginPath(); g.moveTo(x0, 0); g.lineTo(x0, s); g.lineTo(width, s); g.closePath(); g.fill()
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
                    view.js("dogear.clearSelection()")
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
                    view.js("dogear.clearSelection()")
                visible = false
                noteEditor.show(id)
            }
            onCopy: {
                reader.copyText(target.text)
                if (!forMark)
                    view.js("dogear.clearSelection()")
                visible = false
                toast.show(qsTr("Copied"))
            }
            onListen: {
                view.js("dogear.clearSelection()")
                visible = false
                narration.start(target.chapter, target.start)
            }
            onRemove: {
                marks.remove(target.id)
                visible = false
                toast.show(qsTr("Highlight removed"), qsTr("Undo"), () => marks.undo())
            }
        }

        // First-listen popover anchored beneath the top-right Listen button (13_j4_first_listen).
        ListenOffer {
            id: listenOffer
            z: 7
            look: view.look
            anchors.top: parent.top
            anchors.topMargin: chrome.height + 6
            anchors.right: parent.right
            anchors.rightMargin: 22
            onMoreVoices: view.showVoices()
        }
        Binding {
            target: listenOffer
            property: "caretRightOffset"
            value: Math.max(24, Math.min(Math.max(24, listenOffer.width - 24),
                            (readingStage.width - 22) - (tools.x + listenButton.x + listenButton.width / 2)))
            when: listenOffer.hasOwnProperty("caretRightOffset")
        }
    }

    function folioTimeText() {
        if (reader.atEnd)
            return qsTr("Finished")
        const mode = ((folioSettings.folioMode % 3) + 3) % 3
        const loc = Qt.locale()
        const percent = qsTr("%1%").arg(Math.round(reader.progress * 100).toLocaleString(loc, 'f', 0))
        if (mode === 0) {
            if (!view.pdf && reader.minutesLeftInChapter > 0)
                return qsTr("%1 left in chapter").arg(Format.duration(reader.minutesLeftInChapter))
            if (reader.minutesLeftInBook > 0)
                return qsTr("%1 left").arg(Format.duration(reader.minutesLeftInBook))
            return percent
        }
        if (mode === 1) {
            if (reader.minutesLeftInBook > 0)
                return qsTr("%1 left in book").arg(Format.duration(reader.minutesLeftInBook))
            return percent
        }
        if (reader.minutesLeftInBook > 0)
            return qsTr("%1 · %2 left").arg(percent).arg(Format.duration(reader.minutesLeftInBook))
        return percent
    }

    function folioPagesText() {
        const loc = Qt.locale()
        const start = Math.max(1, reader.bookPageStart).toLocaleString(loc, 'f', 0)
        const end = Math.max(1, reader.bookPageEnd).toLocaleString(loc, 'f', 0)
        const total = Math.max(1, reader.bookPages).toLocaleString(loc, 'f', 0)
        const spreadText = reader.bookPageEnd > reader.bookPageStart
            ? qsTr("Pages %1–%2 of %3").arg(start).arg(end).arg(total)
            : qsTr("Page %1 of %2").arg(start).arg(total)
        if (!view.pdf && reader.pageLabel !== "")
            return qsTr("%1 · p. %2").arg(spreadText).arg(reader.pageLabel)
        return spreadText
    }

    Component {
        id: webComponent
        WebEngineView {
            id: webItem
            profile: readerProfile
            visible: win.visible && win.visibility !== Window.Minimized
            backgroundColor: view.look.bg
            function updateLifecycleState() {
                const target = (!visible && !narration.active && !download.active)
                    ? WebEngineView.LifecycleState.Frozen : WebEngineView.LifecycleState.Active
                if (lifecycleState !== target)
                    lifecycleState = target
            }
            onVisibleChanged: updateLifecycleState()
            Component.onCompleted: updateLifecycleState()
            // Chromium only takes the mouse (selecting words, a tap on the
            // margin) when it has focus; the window's shortcuts still come first.
            activeFocusOnPress: true
            webChannel: channel
            url: "dogear://local/app/reader.html"
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
            // Dogear's own menu for the page: listening from the place clicked,
            // and the selection's highlight, note and copy.
            onContextMenuRequested: (request) => {
                request.accepted = true
                const at = Qt.point(request.position.x, request.position.y)
                view.js("JSON.stringify(dogear.placeAt(" + at.x + "," + at.y + "))", function(result) {
                    const place = JSON.parse(result || "null")
                    if (!place)
                        return
                    pageMenu.place = place
                    pageMenu.popup(webItem, at.x, at.y)
                })
            }
            onNewWindowRequested: (request) => {}
            onNavigationRequested: (request) => {
                if (!request.url.toString().startsWith("dogear://"))
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
    Rectangle {
        id: scrim
        z: 8
        anchors.fill: parent
        visible: view.modalPanelOpen
        // VoicePanel provides its own modal scrim; dim the page for GoToBox and NoteEditor.
        color: (noteEditor.open || goTo.open)
             ? (view.look.dark ? Qt.rgba(6 / 255, 6 / 255, 8 / 255, 0.50) : Qt.rgba(24 / 255, 20 / 255, 14 / 255, 0.42))
             : "transparent"
        MouseArea {
            anchors.fill: parent
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
            if (!chromeHover.hovered && !scrubberHover.hovered && !progressScrubber.dragging
                && !view.propBool(listenBar, "menuOpen") && !listenOffer.open
                && !readerMore.opened && !pageMenu.opened && !selectionBar.visible && !keysStrip.wanted)
                view.chromeShown = false
        }
    }

    ListenBar {
        id: listenBar
        z: 9
        // Centered in the room the side panels leave (VoicePanel is a centered modal sheet).
        readonly property real leftInset: contents.open ? contents.width : 0
        readonly property real rightInset: marksPanel.open ? marksPanel.width
                                         : companionPanel.open ? companionPanel.width
                                         : appearance.open ? appearance.width : 0
        x: leftInset + (parent.width - leftInset - rightInset - width) / 2
        Behavior on x {
            enabled: !theme.reducedMotion
            NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
        }
        y: narration.active ? parent.height - height - 28 : parent.height + 16
        Behavior on y {
            enabled: !theme.reducedMotion
            NumberAnimation { duration: 240; easing.type: Easing.OutQuart }
        }
        visible: y < parent.height
        look: view.look
        onFollowRequested: narration.following = true
        onVoiceRequested: voicePanel.open ? voicePanel.close() : view.showVoices()
    }

    ContentsPanel {
        id: contents
        z: 10
        onClosed: view.focusPage()
        height: parent.height
        look: view.look
        onChosen: (chapter, fragment) => view.goTo({ chapter: chapter, fragment: fragment, instant: true })
    }

    VoicePanel {
        id: voicePanel
        z: 12
        onClosed: view.focusPage()
        height: parent.height
        look: view.look
        onSampleWanted: view.js("dogear.sampleText()", function(text) {
            voicePanel.sample = text || ""
            if (debugMode)
                console.log("voice sample:", voicePanel.sample)
        })
    }
    Connections {
        target: voicePanel
        ignoreUnknownSignals: true
        function onListenRequested() {
            voicePanel.close()
            view.startListening()
        }
    }

    AppearancePanel {
        id: appearance
        z: 10
        onClosed: view.focusPage()
        height: parent.height
        look: view.look
        settings: activeSettings
        pdf: view.pdf
        pdfSurface: pdfSurface
        bookOverride: reader.bookAppearanceOverride
        onSettingChanged: (key, value) => view.setAppearance(key, value)
        onBookOverrideToggled: (enabled) => view.setBookOverride(enabled)
    }

    Menu {
        id: pageMenu
        property var place: ({})
        readonly property bool hasSelection: !!view.selection.text
        topPadding: 6
        bottomPadding: 6
        background: Rectangle {
            implicitWidth: 230
            radius: Math.min(10, theme.radius)
            color: view.look.popover
            border.color: view.look.line
            Rectangle {
                z: -1
                anchors.fill: parent
                anchors.topMargin: 4
                anchors.bottomMargin: -4
                radius: parent.radius
                color: "black"
                opacity: view.look.dark ? 0.35 : 0.12
            }
        }

        MenuItem {
            text: narration.active ? qsTr("Listen from here instead") : qsTr("Listen from here")
            onTriggered: {
                view.leaveVoice()
                narration.start(pageMenu.place.chapter, pageMenu.place.offset)
            }
        }
        MenuSeparator { visible: pageMenu.hasSelection }
        MenuItem {
            visible: pageMenu.hasSelection
            height: visible ? implicitHeight : 0
            text: qsTr("Highlight")
            onTriggered: {
                marks.addHighlight(view.selection.chapter, view.selection.start, view.selection.end,
                                   view.selection.text, "yellow", reader.titleOfChapter(view.selection.chapter))
                view.js("dogear.clearSelection()")
                selectionBar.visible = false
            }
        }
        MenuItem {
            visible: pageMenu.hasSelection
            height: visible ? implicitHeight : 0
            text: qsTr("Add a note")
            onTriggered: {
                const id = marks.addHighlight(view.selection.chapter, view.selection.start, view.selection.end,
                                              view.selection.text, "yellow", reader.titleOfChapter(view.selection.chapter))
                view.js("dogear.clearSelection()")
                selectionBar.visible = false
                noteEditor.show(id)
            }
        }
        MenuItem {
            visible: pageMenu.hasSelection
            height: visible ? implicitHeight : 0
            text: qsTr("Copy")
            onTriggered: {
                reader.copyText(view.selection.text)
                view.js("dogear.clearSelection()")
                selectionBar.visible = false
                toast.show(qsTr("Copied"))
            }
        }
        MenuSeparator { visible: pageMenu.hasSelection }
        MenuItem {
            visible: pageMenu.hasSelection
            height: visible ? implicitHeight : 0
            text: qsTr("Define")
            onTriggered: view.askCompanion("define", view.selection)
        }
        MenuItem {
            visible: pageMenu.hasSelection
            height: visible ? implicitHeight : 0
            text: qsTr("Explain")
            onTriggered: view.askCompanion("explain", view.selection)
        }
        MenuItem {
            visible: pageMenu.hasSelection
            height: visible ? implicitHeight : 0
            text: qsTr("Translate")
            onTriggered: view.askCompanion("translate", view.selection)
        }
        MenuItem {
            visible: illustrator.available
            height: visible ? implicitHeight : 0
            text: pageMenu.hasSelection ? qsTr("Illustrate these words") : qsTr("Illustrate this moment")
            onTriggered: {
                view.showCompanion("pictures")
                if (pageMenu.hasSelection)
                    illustrator.illustrate(view.selection.text, view.selection.chapter, view.selection.end)
                else
                    illustrator.illustrate("", pageMenu.place.chapter, pageMenu.place.offset + 600)
            }
        }
        MenuSeparator {}
        MenuItem {
            text: view.pageBookmark ? qsTr("Remove the bookmark") : qsTr("Bookmark this page")
            onTriggered: view.toggleBookmark()
        }
    }

    NoteEditor {
        id: noteEditor
        onClosed: view.focusPage()
        anchors.centerIn: parent
        z: 14
        look: view.look
    }

    CompanionPanel {
        id: companionPanel
        z: 10
        onClosed: view.focusPage()
        height: parent.height
        look: view.look
    }
    Connections {
        target: companionPanel
        ignoreUnknownSignals: true
        function onJumpRequested(chapter, offset) {
            view.goTo({ chapter: chapter, offset: offset, instant: true })
        }
        function onSaveAsNote(quote, note) {
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

    // Inline EPUB image/SVG lightbox with zoom, wheel zoom, drag-to-pan, and caption.
    FocusScope {
        id: lightbox
        anchors.fill: parent
        z: 50
        property bool open: false
        property string source: ""
        property string caption: ""
        property real zoom: 1.0
        property real panX: 0
        property real panY: 0
        visible: opacity > 0.01
        opacity: open ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 160 } }

        function openImage(src, cap) {
            if (!src || src === "")
                return
            source = src
            caption = cap || ""
            zoom = 1.0
            panX = 0
            panY = 0
            open = true
            forceActiveFocus()
        }
        function close() {
            if (!open)
                return
            open = false
            zoom = 1.0
            panX = 0
            panY = 0
        }
        function setZoom(next) {
            zoom = Math.max(1.0, Math.min(5.0, Math.round(next * 100) / 100))
            if (zoom <= 1.01) {
                panX = 0
                panY = 0
            } else {
                clampPan()
            }
        }
        function clampPan() {
            const maxX = Math.max(0, (lightboxImage.paintedWidth * zoom - width + 96) / 2)
            const maxY = Math.max(0, (lightboxImage.paintedHeight * zoom - height + 140) / 2)
            panX = Math.max(-maxX, Math.min(maxX, panX))
            panY = Math.max(-maxY, Math.min(maxY, panY))
        }

        Keys.onEscapePressed: { close(); view.focusPage() }
        Keys.onPressed: (event) => {
            if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) { setZoom(zoom * 1.25); event.accepted = true }
            else if (event.key === Qt.Key_Minus) { setZoom(zoom / 1.25); event.accepted = true }
            else if (event.key === Qt.Key_0) { setZoom(1.0); event.accepted = true }
        }

        Rectangle {
            anchors.fill: parent
            color: Ui.tint("black", 0.86)
        }

        MouseArea {
            id: lightboxPan
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            cursorShape: lightbox.zoom > 1.01 ? (pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor) : Qt.ArrowCursor
            property bool dragged: false
            property real startX: 0
            property real startY: 0
            property real startPanX: 0
            property real startPanY: 0
            onPressed: (event) => {
                dragged = false
                startX = event.x
                startY = event.y
                startPanX = lightbox.panX
                startPanY = lightbox.panY
            }
            onPositionChanged: (event) => {
                if (!pressed || lightbox.zoom <= 1.01)
                    return
                const dx = event.x - startX, dy = event.y - startY
                if (!dragged && Math.abs(dx) + Math.abs(dy) < 4)
                    return
                dragged = true
                lightbox.panX = startPanX + dx
                lightbox.panY = startPanY + dy
                lightbox.clampPan()
            }
            onDoubleClicked: lightbox.setZoom(lightbox.zoom > 1.2 ? 1.0 : 2.0)
            onClicked: (event) => {
                if (dragged)
                    return
                const hitW = lightboxImage.paintedWidth * lightbox.zoom
                const hitH = lightboxImage.paintedHeight * lightbox.zoom
                const cx = lightbox.width / 2 + lightbox.panX
                const cy = lightbox.height / 2 + lightbox.panY
                const onImage = event.x >= cx - hitW / 2 && event.x <= cx + hitW / 2
                                && event.y >= cy - hitH / 2 && event.y <= cy + hitH / 2
                if (!onImage) {
                    lightbox.close()
                    view.focusPage()
                }
            }
            onWheel: (wheel) => {
                wheel.accepted = true
                const factor = wheel.angleDelta.y > 0 ? 1.15 : (1 / 1.15)
                lightbox.setZoom(lightbox.zoom * factor)
            }
        }

        Image {
            id: lightboxImage
            anchors.centerIn: parent
            anchors.horizontalCenterOffset: lightbox.panX
            anchors.verticalCenterOffset: lightbox.panY
            width: Math.max(120, lightbox.width - 96)
            height: Math.max(120, lightbox.height - (lightbox.caption !== "" ? 156 : 112))
            source: lightbox.open ? lightbox.source : ""
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            smooth: true
            mipmap: true
            scale: lightbox.zoom
        }

        // Top-right floating zoom & close controls.
        Rectangle {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: 20
            anchors.rightMargin: 20
            width: lightboxControls.implicitWidth + 14
            height: 40
            radius: 20
            color: Ui.tint("#1c1c1e", 0.9)
            border.color: Ui.tint("white", 0.16)

            Row {
                id: lightboxControls
                anchors.centerIn: parent
                spacing: 4
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    glyph: Icons.zoomOut
                    tone: "white"
                    enabled: lightbox.zoom > 1.01
                    tip: qsTr("Zoom out (−)")
                    onClicked: lightbox.setZoom(lightbox.zoom / 1.25)
                }
                AbstractButton {
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 48
                    implicitHeight: 30
                    hoverEnabled: true
                    focusPolicy: Qt.NoFocus
                    onClicked: lightbox.setZoom(1.0)
                    background: Rectangle {
                        radius: 6
                        color: parent.hovered ? Ui.tint("white", 0.12) : "transparent"
                    }
                    contentItem: Label {
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        text: Math.round(lightbox.zoom * 100).toLocaleString(Qt.locale(), 'f', 0) + "%"
                        color: "white"
                        font.pixelSize: 12
                        font.features: ({ "tnum": 1 })
                    }
                    ToolTip.visible: hovered
                    ToolTip.delay: 600
                    ToolTip.text: qsTr("Reset zoom (0)")
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    glyph: Icons.zoomIn
                    tone: "white"
                    enabled: lightbox.zoom < 4.99
                    tip: qsTr("Zoom in (+)")
                    onClicked: lightbox.setZoom(lightbox.zoom * 1.25)
                }
                Rectangle {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 1
                    height: 18
                    color: Ui.tint("white", 0.18)
                }
                IconButton {
                    anchors.verticalCenter: parent.verticalCenter
                    glyph: Icons.close
                    tone: "white"
                    tip: qsTr("Close (Esc)")
                    onClicked: { lightbox.close(); view.focusPage() }
                }
            }
        }

        // Caption pill shown when the image has alt text or a figcaption.
        Rectangle {
            visible: lightbox.caption !== ""
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 24
            anchors.horizontalCenter: parent.horizontalCenter
            width: Math.min(parent.width - 64, captionText.implicitWidth + 32)
            height: captionText.implicitHeight + 18
            radius: 10
            color: Ui.tint("#1c1c1e", 0.88)
            border.color: Ui.tint("white", 0.14)
            Label {
                id: captionText
                anchors.centerIn: parent
                width: Math.min(implicitWidth, lightbox.width - 96)
                text: lightbox.caption
                color: "#f2f0eb"
                font.pixelSize: 13
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                maximumLineCount: 4
                elide: Text.ElideRight
            }
        }
    }

    MarksPanel {
        id: marksPanel
        z: 10
        onClosed: view.focusPage()
        height: parent.height
        look: view.look
        onChosen: (chapter, offset) => view.goTo({ chapter: chapter, offset: offset, instant: true })
    }

    SearchBar {
        id: searchBar
        onClosed: view.focusPage()
        anchors.horizontalCenter: readingStage.horizontalCenter
        y: 64
        z: 14
        look: view.look
        onJumped: (hit) => view.showFound(hit)
    }

    GoToBox {
        id: goTo
        onClosed: view.focusPage()
        anchors.horizontalCenter: readingStage.horizontalCenter
        y: 96
        z: 14
        look: view.look
        onGo: (target) => view.goTo(target)
    }

    // --- Keys ----------------------------------------------------------------
    readonly property bool modalOpen: win.helpVisible || settingsView.open || welcome.open || (typeof commandPalette !== "undefined" && commandPalette.open)
    readonly property bool keys: visible && !modalPanelOpen && !modalOpen && !win.typing && !lightbox.open
    // Listening keys work with a panel open too, unless a field is taking typing.
    readonly property bool typing: win.typing
    readonly property bool listenKeys: visible && !modalOpen && !win.typing && !lightbox.open
    Shortcut { sequence: "C"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: companionPanel.open ? companionPanel.close() : view.showCompanion("ask") }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: reader.rtl ? view.prev() : view.next() }
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: reader.rtl ? view.next() : view.prev() }
    Shortcut { sequences: ["PgDown", "L", "J", "Space"]; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.next() }
    Shortcut { sequences: ["PgUp", "H", "K", "Shift+Space"]; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.prev() }
    Shortcut { sequence: "]"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.nextChapter() }
    Shortcut { sequence: "["; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.prevChapter() }
    Shortcut { sequence: "Alt+Left"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.back() }
    Shortcut { sequence: "Alt+Right"; context: Qt.ApplicationShortcut; enabled: view.keys && reader.canGoForward; onActivated: view.forward() }
    Shortcut { sequence: "Shift+Left"; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: if (typeof narration.skipSeconds === "function") narration.skipSeconds(-15) }
    Shortcut { sequence: "Shift+Right"; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: if (typeof narration.skipSeconds === "function") narration.skipSeconds(15) }
    Shortcut { sequence: "T"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.showContents() }
    Shortcut { sequence: "A"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.showAppearance() }
    Shortcut { sequence: "G"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: goTo.show() }
    Shortcut { sequence: "B"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.toggleBookmark() }
    Shortcut { sequence: "M"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.showMarks() }
    Shortcut { sequences: [StandardKey.Redo]; context: Qt.ApplicationShortcut; enabled: view.keys && marks.canRedo; onActivated: marks.redo() }
    Shortcut { sequences: [StandardKey.Copy]; context: Qt.ApplicationShortcut; enabled: view.visible && view.pdf && !!pdfSurface.selection && !view.modalOpen && !win.typing; onActivated: { reader.copyText(pdfSurface.selection.text); toast.show(qsTr("Copied")) } }
    Shortcut { sequences: ["/", StandardKey.Find]; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: searchBar.show() }
    Shortcut { sequence: "N"; context: Qt.ApplicationShortcut; enabled: view.keys && bookSearch.count > 0; onActivated: searchBar.next() }
    Shortcut { sequence: "Shift+N"; context: Qt.ApplicationShortcut; enabled: view.keys && bookSearch.count > 0; onActivated: searchBar.previous() }
    Shortcut { sequence: "D"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.cycleTheme() }
    Shortcut { sequences: ["+", "=", "Ctrl++", "Ctrl+="]; context: Qt.ApplicationShortcut; enabled: view.visible && !view.modalOpen && !win.typing; onActivated: view.resize(0.05) }
    Shortcut { sequences: ["-", "Ctrl+-"]; context: Qt.ApplicationShortcut; enabled: view.visible && !view.modalOpen && !win.typing; onActivated: view.resize(-0.05) }
    Shortcut { sequences: ["0", "Ctrl+0"]; context: Qt.ApplicationShortcut; enabled: view.visible && !view.modalOpen && !win.typing; onActivated: view.resize(0) }
    Shortcut { sequence: "Tab"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.chromeShown ? (view.chromeShown = false) : view.showChrome() }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: view.visible && !view.modalOpen; onActivated: view.dismiss() }
    // Listening.
    // P listens from the page in view, then pauses and goes on; ⇧P stops.
    Shortcut { sequence: "P"; context: Qt.ApplicationShortcut; enabled: view.listenKeys; onActivated: narration.active ? narration.toggle() : view.listen() }
    Shortcut { sequence: "Shift+P"; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: narration.stop() }
    Shortcut { sequence: "V"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.showVoices() }
    Shortcut { sequence: ","; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: narration.previousParagraph() }
    Shortcut { sequence: "."; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: narration.nextParagraph() }
    Shortcut { sequences: ["<", "Shift+,"]; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: narration.rate = narration.rate - 0.1 }
    Shortcut { sequences: [">", "Shift+."]; context: Qt.ApplicationShortcut; enabled: view.listenKeys && narration.active; onActivated: narration.rate = narration.rate + 0.1 }
    Shortcut { sequence: "Media Play"; context: Qt.ApplicationShortcut; enabled: narration.active; onActivated: narration.resume() }
    Shortcut { sequence: "Media Pause"; context: Qt.ApplicationShortcut; enabled: narration.active; onActivated: narration.pause() }
    Shortcut { sequence: "Toggle Media Play/Pause"; context: Qt.ApplicationShortcut; enabled: narration.active; onActivated: narration.toggle() }
}
