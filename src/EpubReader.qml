import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
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
        // 0: pages in chapter, 1: time left in chapter, 2: time left in book, 3: %
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
    readonly property bool panelOpen: contents.open || appearance.open || goTo.open || voicePanel.open || searchBar.editing
                                     || marksPanel.open || noteEditor.open || companionPanel.open
    property bool chromeShown: false
    property bool surfaceReady: false
    property bool listening: false
    property bool jumpPillVisible: false
    property string jumpPillLabel: ""

    signal closeRequested()

    Material.theme: look.dark ? Material.Dark : Material.Light
    Material.accent: look.link

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
        const margins = { narrow: [0.035, 40], normal: [0.06, 34], wide: [0.1, 30] }[activeSettings.margins] || [0.06, 34]
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
            layout: activeSettings.layout,
            animate: activeSettings.animate,
            publisher: activeSettings.publisher,
            pdfCrop: activeSettings.pdfCrop,
            pdfOriginalColors: activeSettings.pdfOriginalColors,
            marginTop: 54,
            // Room for the listen bar under the text while listening.
            marginBottom: narration.active ? 84 : 54,
            radius: theme.radius,
            listening: narration.active,
            strings: { goToNote: qsTr("Go to note"), listenFromHere: qsTr("Listen from here") },
        }
    }
    function openBook() {
        if (!reader.open)
            return
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
    // "Follow along" brings the page back to it.
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

    // Listening starts from the top of the page in view.
    function listen() {
        if (narration.active) {
            narration.stop()
            return
        }
        js("JSON.stringify(dogear.listeningStart())", function(result) {
            const place = JSON.parse(result || "null")
            if (place)
                narration.start(place.chapter, place.offset)
        })
    }
    // Keys and the mouse go back to the page when what was open closes.
    function focusPage() {
        if (pdf) {
            pdfSurface.forceActiveFocus()
            return
        }
        if (!panelOpen && web)
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
                selectionBar.place(view.mapRect(selection), view)
                selectionBar.visible = true
            } else if (!selectionBar.forMark) {
                selectionBar.visible = false
            }
        }
        function onMarkChosen(id, rect) {
            selectionBar.forMark = true
            selectionBar.target = marks.mark(id)
            selectionBar.place(view.mapRect(rect), view)
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
        const at = src.mapToItem(view, rect.x, rect.y)
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
    function askCompanion(feature, target) {
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
            if (narration.active)
                win.tip("listen", qsTr(", and . skip a paragraph, < and > change the speed. Close the window and the reading goes on."))
            if (narration.active !== view.listening) {
                view.listening = narration.active
                styleTimer.restart()
            }
            if (!narration.active)
                view.js("dogear.highlight('dg-speaking', [])")
        }
        function onNeedsKey() {
            toast.show(qsTr("Listening uses your Gemini key. Add it to start."))
            win.showAiSettings()
        }
        function onVoiceMissing() {
            toast.show(qsTr("That voice isn't downloaded yet. Choose or download one."))
            view.showVoices()
        }
    }

    // Back from another workspace or app: a fresh frame of the page, at once
    // and again a moment later (as the compositor settles).
    Connections {
        target: win
        function onActiveChanged() { if (win.active) view.refreshSoon() }
        function onVisibilityChanged() {
            if (win.visible && win.visibility !== Window.Minimized && win.visibility !== Window.Hidden)
                view.refreshSoon()
        }
    }
    Connections {
        target: Qt.application
        function onStateChanged() { if (Qt.application.state === Qt.ApplicationActive) view.refreshSoon() }
    }
    function refreshSoon() {
        if (pdf || !surfaceReady || !reader.open)
            return
        refreshTimer.restart()
        refreshLater.restart()
    }
    Timer { id: refreshTimer; interval: 16; onTriggered: if (!view.pdf && view.surfaceReady && reader.open) view.js("dogear.refresh && dogear.refresh()") }
    Timer { id: refreshLater; interval: 350; onTriggered: if (!view.pdf && view.surfaceReady && reader.open) view.js("dogear.refresh && dogear.refresh()") }

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
                view.chromeShown = false
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
        if (win.helpVisible || aiSettings.open || welcome.open || win.typing)
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
            else if (key === "+" || key === "=") resize(0.05)
            else if (key === "-") resize(-0.05)
            else if (key === "0") resize(0)
            else if (key === ",") aiSettings.open ? aiSettings.close() : aiSettings.show()
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
        anchors.fill: parent
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

    Component {
        id: webComponent
        WebEngineView {
            id: webItem
            profile: readerProfile
            backgroundColor: view.look.bg
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

    // In scroll mode the text passes under the running head and folio;
    // it fades into the page there rather than colliding with them.
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

    // --- Running head and folio -------------------------------------------
    Label {
        anchors.top: parent.top
        anchors.topMargin: 18
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width * 0.6, 640)
        horizontalAlignment: Text.AlignHCenter
        opacity: view.chromeShown ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: 160 } }
        text: reader.chapterTitle
        color: view.look.muted
        font.pixelSize: 12
        font.letterSpacing: 0.4
        elide: Text.ElideRight
    }

    function folioPrimaryText() {
        const mode = ((folioSettings.folioMode % 4) + 4) % 4
        const loc = Qt.locale()
        const percent = qsTr("%1%").arg(Math.round(reader.progress * 100).toLocaleString(loc, 'f', 0))
        if (mode === 0) {
            if (view.pdf) {
                const cur = (reader.chapter + 1).toLocaleString(loc, 'f', 0)
                const tot = Math.max(1, reader.chapterCount).toLocaleString(loc, 'f', 0)
                return qsTr("Page %1 of %2").arg(cur).arg(tot)
            }
            const p = (reader.page + 1).toLocaleString(loc, 'f', 0)
            const ps = Math.max(1, reader.pages).toLocaleString(loc, 'f', 0)
            return qsTr("Page %1 of %2").arg(p).arg(ps)
        }
        if (mode === 1) {
            if (reader.atEnd)
                return qsTr("Finished")
            if (!view.pdf && reader.minutesLeftInChapter > 0)
                return qsTr("%1 left in chapter").arg(Format.duration(reader.minutesLeftInChapter))
            if (reader.minutesLeftInBook > 0)
                return qsTr("%1 left in book").arg(Format.duration(reader.minutesLeftInBook))
            return percent
        }
        if (mode === 2) {
            if (reader.atEnd)
                return qsTr("Finished")
            if (reader.minutesLeftInBook > 0)
                return qsTr("%1 left in book").arg(Format.duration(reader.minutesLeftInBook))
            return percent
        }
        return percent
    }

    Item {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 28
        anchors.rightMargin: 28
        height: 44
        opacity: view.chromeShown || narration.active ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: 160 } }

        Label {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: reader.pageLabel !== "" ? qsTr("p. %1").arg(reader.pageLabel) : ""
            color: view.look.muted
            font.pixelSize: 12
            font.features: ({ "tnum": 1 })
        }
        AbstractButton {
            id: folioButton
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            enabled: !view.chromeShown && !narration.active
            hoverEnabled: true
            focusPolicy: Qt.NoFocus
            implicitWidth: folioLabel.implicitWidth + 16
            implicitHeight: 28
            onClicked: folioSettings.folioMode = (folioSettings.folioMode + 1) % 4
            background: Rectangle {
                radius: Math.min(6, theme.radius + 2)
                color: folioButton.hovered ? Ui.tint(view.look.fg, 0.06) : "transparent"
            }
            contentItem: Label {
                id: folioLabel
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                text: {
                    void(folioSettings.folioMode)
                    void(reader.page); void(reader.pages); void(reader.chapter); void(reader.chapterCount)
                    void(reader.progress); void(reader.minutesLeftInChapter); void(reader.minutesLeftInBook)
                    void(reader.atEnd); void(translator.language)
                    return view.folioPrimaryText()
                }
                color: view.look.muted
                font.pixelSize: 12
                font.features: ({ "tnum": 1 })
            }
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            ToolTip.visible: hovered
            ToolTip.delay: 700
            ToolTip.text: qsTr("Click to switch reading indicator")
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
        z: 4
        anchors.left: parent.left
        anchors.leftMargin: 24 + (contents.open ? contents.width : 0)
        Behavior on anchors.leftMargin { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        readonly property bool shown: view.jumpPillVisible && reader.canGoBack && view.jumpPillLabel !== ""
        y: shown ? parent.height - height - (view.chromeShown ? 84 : narration.active ? 76 : 48) : parent.height + 16
        Behavior on y { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        opacity: shown ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 150 } }
        visible: opacity > 0.01
        width: jumpPillRow.implicitWidth + 16
        height: 36
        radius: Math.min(18, Math.max(theme.radius + 4, 8))
        color: view.look.popover
        border.color: view.look.border

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
                    radius: Math.min(15, theme.radius + 4)
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
                glyph: Icons.close
                look: view.look
                tone: view.look.muted
                tip: qsTr("Dismiss")
                onClicked: view.jumpPillVisible = false
            }
        }
    }

    // While a panel or a dialog is open the page underneath is left alone:
    // the wheel does nothing there, and a click outside closes what's open
    // (a note being written is never thrown away by a stray click).
    MouseArea {
        id: scrim
        anchors.fill: parent
        visible: view.panelOpen
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onPressed: (mouse) => {
            mouse.accepted = true
            if (!noteEditor.open)
                view.closePanels()
        }
        onWheel: (wheel) => wheel.accepted = true
    }

    // --- Chrome ------------------------------------------------------------
    Timer {
        id: chromeTimer
        interval: 3200
        onTriggered: if (!chromeHover.hovered && !scrubberHover.hovered && !progressScrubber.dragging) view.chromeShown = false
    }
    // The chrome comes when the pointer moves to the top or bottom edge, not
    // merely because it rests there when a book opens.
    property real lastPointerY: -1
    HoverHandler {
        id: topEdge
        onPointChanged: {
            const y = point.position.y
            if (view.lastPointerY >= 0 && y !== view.lastPointerY && (y < 64 || y > view.height - 64) && !view.panelOpen)
                view.showChrome()
            view.lastPointerY = y
        }
    }

    Rectangle {
        id: chrome
        anchors.left: parent.left
        anchors.right: parent.right
        height: 60
        y: view.chromeShown ? 0 : -height - 4
        Behavior on y { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        color: view.look.popover
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: view.look.border }
        HoverHandler { id: chromeHover }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            IconButton {
                glyph: Icons.back
                look: view.look
                tip: qsTr("Library (Esc)")
                onClicked: view.closeRequested()
            }
            IconButton {
                glyph: Icons.contents
                look: view.look
                tip: qsTr("Contents (T)")
                active: contents.open
                onClicked: contents.open ? contents.close() : view.showContents()
            }
        }

        Column {
            anchors.centerIn: parent
            width: Math.max(0, Math.min(parent.width - 520, 640))
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: reader.title
                color: view.look.fg
                font.pixelSize: 14
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: reader.author
                color: view.look.muted
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            IconButton {
                glyph: Icons.bookmark
                look: view.look
                active: view.pageBookmark > 0
                tip: view.pageBookmark ? qsTr("Remove the bookmark (B)") : qsTr("Bookmark this page (B)")
                onClicked: view.toggleBookmark()
            }
            IconButton {
                glyph: Icons.pen
                look: view.look
                active: marksPanel.open
                tip: qsTr("Highlights, notes, and bookmarks (M)")
                onClicked: marksPanel.open ? marksPanel.close() : view.showMarks()
            }
            IconButton {
                glyph: Icons.sparkle
                look: view.look
                active: companionPanel.open
                tip: qsTr("Companion: ask about the book (C)")
                onClicked: companionPanel.open ? companionPanel.close() : view.showCompanion("ask")
            }
            IconButton {
                glyph: Icons.headphones
                look: view.look
                active: narration.active
                tip: narration.active ? qsTr("Stop listening (⇧P)") : qsTr("Listen from this page (P)")
                onClicked: view.listen()
            }
            IconButton {
                look: view.look
                tip: qsTr("Appearance (A)")
                active: appearance.open
                contentItem: Label {
                    text: "Aa"
                    font.family: "Literata"
                    font.pixelSize: 16
                    color: appearance.open ? view.look.link : view.look.fg
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: appearance.open ? appearance.close() : view.showAppearance()
            }
            IconButton {
                glyph: Icons.fullscreen
                look: view.look
                tip: qsTr("Full screen (F)")
                onClicked: win.toggleFullScreen()
            }
        }
    }

    Rectangle {
        id: bottomChrome
        anchors.left: parent.left
        anchors.right: parent.right
        height: 72
        y: view.chromeShown ? parent.height - height : parent.height + 4
        Behavior on y { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        color: view.look.popover
        Rectangle { width: parent.width; height: 1; color: view.look.border }
        HoverHandler { id: scrubberHover }

        Label {
            anchors.left: parent.left
            anchors.leftMargin: 28
            anchors.top: parent.top
            anchors.topMargin: 10
            text: reader.chapterTitle
            color: view.look.fg
            font.pixelSize: 12
            elide: Text.ElideRight
            width: parent.width * 0.6
        }
        Label {
            anchors.right: parent.right
            anchors.rightMargin: 28
            anchors.top: parent.top
            anchors.topMargin: 10
            text: reader.minutesLeftInBook > 0 && translator.language !== ""
                  ? qsTr("%1 left in book").arg(Format.duration(reader.minutesLeftInBook)) : ""
            color: view.look.muted
            font.pixelSize: 12
            font.features: ({ "tnum": 1 })
        }
        ProgressScrubber {
            id: progressScrubber
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 28
            anchors.rightMargin: 28
            anchors.bottomMargin: 2
            height: 44
            look: view.look
            onGo: (target) => view.goTo(target)
        }
    }

    ListenBar {
        id: listenBar
        // Centered in the room the side panels leave.
        readonly property real leftInset: contents.open ? contents.width : 0
        readonly property real rightInset: voicePanel.open ? voicePanel.width : marksPanel.open ? marksPanel.width
                                         : companionPanel.open ? companionPanel.width
                                         : appearance.open ? appearance.width : 0
        x: leftInset + (parent.width - leftInset - rightInset - width) / 2
        Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        y: narration.active ? parent.height - height - (view.chromeShown ? 84 : 14) : parent.height + 10
        Behavior on y { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
        visible: y < parent.height
        look: view.look
        onFollowRequested: narration.following = true
        onVoiceRequested: voicePanel.open ? voicePanel.close() : view.showVoices()
    }

    ContentsPanel {
        id: contents
        onClosed: view.focusPage()
        height: parent.height
        look: view.look
        onChosen: (chapter, fragment) => view.goTo({ chapter: chapter, fragment: fragment, instant: true })
    }

    VoicePanel {
        id: voicePanel
        onClosed: view.focusPage()
        height: parent.height
        look: view.look
        onSampleWanted: view.js("dogear.sampleText()", function(text) {
            voicePanel.sample = text || ""
            if (debugMode)
                console.log("voice sample:", voicePanel.sample)
        })
    }

    AppearancePanel {
        id: appearance
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

    // The bookmark's ribbon, hanging from the top of a marked page.
    Item {
        id: ribbon
        anchors.right: parent.right
        anchors.rightMargin: 46
        width: 18
        height: 30
        y: view.pageBookmark ? 0 : -height - 2
        Behavior on y { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
        Canvas {
            anchors.fill: parent
            property color fill: view.look.link
            onFillChanged: requestPaint()
            onPaint: {
                const g = getContext("2d")
                g.reset()
                g.fillStyle = fill
                g.beginPath()
                g.moveTo(0, 0)
                g.lineTo(width, 0)
                g.lineTo(width, height)
                g.lineTo(width / 2, height - 7)
                g.lineTo(0, height)
                g.closePath()
                g.fill()
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
        onCompanion: (feature) => {
            view.askCompanion(feature, target)
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

    Menu {
        id: pageMenu
        property var place: ({})
        readonly property bool hasSelection: !!view.selection.text
        Material.theme: view.look.dark ? Material.Dark : Material.Light
        Material.accent: view.look.link
        Material.foreground: view.look.fg
        topPadding: 6
        bottomPadding: 6
        background: Rectangle {
            implicitWidth: 230
            radius: Math.min(10, theme.radius)
            color: view.look.popover
            border.color: view.look.border
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
        z: 7
        look: view.look
    }

    CompanionPanel {
        id: companionPanel
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
        onClosed: view.focusPage()
        height: parent.height
        look: view.look
        onChosen: (chapter, offset) => view.goTo({ chapter: chapter, offset: offset, instant: true })
    }

    SearchBar {
        id: searchBar
        onClosed: view.focusPage()
        anchors.horizontalCenter: parent.horizontalCenter
        y: 64
        z: 5
        look: view.look
        onJumped: (hit) => view.showFound(hit)
    }

    GoToBox {
        id: goTo
        onClosed: view.focusPage()
        anchors.horizontalCenter: parent.horizontalCenter
        y: 96
        look: view.look
        onGo: (target) => view.goTo(target)
    }

    // --- Keys ----------------------------------------------------------------
    readonly property bool modalOpen: win.helpVisible || aiSettings.open || welcome.open
    readonly property bool keys: visible && !panelOpen && !modalOpen && !win.typing && !lightbox.open
    // Listening keys work with a panel open too, unless a field is taking typing.
    readonly property bool typing: win.typing
    readonly property bool listenKeys: visible && !modalOpen && !win.typing && !lightbox.open
    Shortcut { sequence: "C"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.showCompanion("ask") }
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
