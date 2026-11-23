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
    readonly property var look: Themes.palette(settings.theme, theme)
    // For driving the panel from outside (and from tests).
    readonly property alias voicePanel: voicePanel
    readonly property alias searchBar: searchBar
    readonly property alias pageMenu: pageMenu
    // The words selected on the page, as the page last said ({} for none).
    property var selection: ({})
    readonly property alias marksPanel: marksPanel
    readonly property alias companionPanel: companionPanel
    readonly property alias noteEditor: noteEditor
    readonly property bool panelOpen: contents.open || appearance.open || goTo.open || voicePanel.open || searchBar.editing
                                     || marksPanel.open || noteEditor.open || companionPanel.open
    property bool chromeShown: false
    property bool surfaceReady: false
    property bool listening: false

    signal closeRequested()

    Material.theme: look.dark ? Material.Dark : Material.Light
    Material.accent: look.link

    readonly property bool pdf: reader.format === "pdf"
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
        if (callback)
            web.runJavaScript(code, callback)
        else
            web.runJavaScript(code)
    }
    function style() {
        const margins = { narrow: [0.035, 40], normal: [0.06, 34], wide: [0.1, 30] }[settings.margins] || [0.06, 34]
        return {
            theme: look,
            font: settings.font,
            fontScale: settings.fontScale,
            lineHeight: settings.lineHeight,
            margin: margins[0],
            measure: margins[1],
            justify: settings.justify,
            hyphenate: settings.hyphenate,
            layout: settings.layout,
            animate: settings.animate,
            publisher: settings.publisher,
            marginTop: 54,
            // Room for the listen bar under the text while listening.
            marginBottom: narration.active ? 84 : 54,
            radius: theme.radius,
            listening: narration.active,
            strings: { goToNote: qsTr("Go to note"), listenFromHere: qsTr("Listen from here") },
        }
    }
    function openBook() {
        if (!reader.open || (!surfaceReady && !pdf))
            return
        js("dogear.open(" + JSON.stringify(reader.manifest) + "," + JSON.stringify(reader.startPosition) + ","
           + JSON.stringify(style()) + ")")
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
    function back() { leaveVoice(); js("dogear.back()") }
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
        if (!panelOpen)
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
        else
            web.forceActiveFocus()
    }
    function dismiss() {
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
        settings.theme = order[(order.indexOf(settings.theme) + 1) % order.length]
        toast.show(qsTr("%1 theme").arg(Themes.name(settings.theme)))
    }
    function resize(step) {
        // A PDF's text is its pages: it zooms.
        if (pdf) {
            pdfSurface.zoom = step === 0 ? 1 : Math.max(1, Math.min(4, pdfSurface.zoom * (step > 0 ? 1.15 : 1 / 1.15)))
            return
        }
        settings.fontScale = step === 0 ? 1.15 : Math.max(0.7, Math.min(2.0, Math.round((settings.fontScale + step) * 100) / 100))
    }
    function showContents() { contents.show() }
    function showAppearance() { appearance.show() }
    // Search hits on the page: all of them in the chapter shown, and the one
    // gone to.
    property int foundChapter: -1
    function markFound() {
        if (!surfaceReady || !reader.open)
            return
        foundChapter = reader.chapter
        const hit = bookSearch.current >= 0 ? bookSearch.results[bookSearch.current] : null
        const current = hit && hit.chapter === reader.chapter ? { start: hit.start, end: hit.end } : null
        js("dogear.found(" + JSON.stringify(bookSearch.inChapter(reader.chapter)) + "," + JSON.stringify(current) + ")")
    }
    function showFound(hit) {
        leaveVoice()
        js("dogear.goTo(" + JSON.stringify({ chapter: hit.chapter, offset: hit.start, instant: true }) + ")"
           + ".then(() => dogear.found(" + JSON.stringify(bookSearch.inChapter(hit.chapter)) + ","
           + JSON.stringify({ start: hit.start, end: hit.end }) + "))")
        foundChapter = hit.chapter
    }
    Connections {
        target: bookSearch
        function onChanged() { view.markFound() }
    }
    // Highlights and search hits are drawn on each chapter as it's shown.
    property int decoratedChapter: -1
    function decorate() {
        if (surfaceReady && reader.open)
            js("dogear.marks(" + JSON.stringify(marks.highlightsIn(reader.chapter)) + ")")
    }
    Connections {
        target: reader
        function onLocationChanged() {
            selectionBar.visible = false
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
        const at = web.mapToItem(view, rect.x, rect.y)
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
        onTriggered: if (view.surfaceReady && reader.open) view.js("dogear.setStyle(" + JSON.stringify(view.style()) + ")")
    }
    onLookChanged: styleTimer.restart()
    Connections {
        target: view.settings
        function onFontChanged() { styleTimer.restart() }
        function onFontScaleChanged() { styleTimer.restart() }
        function onLineHeightChanged() { styleTimer.restart() }
        function onMarginsChanged() { styleTimer.restart() }
        function onJustifyChanged() { styleTimer.restart() }
        function onHyphenateChanged() { styleTimer.restart() }
        function onLayoutChanged() { styleTimer.restart() }
        function onAnimateChanged() { styleTimer.restart() }
        function onPublisherChanged() { styleTimer.restart() }
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
                view.js("dogear.goTo({chapter:" + chapter + ",offset:" + start + ",instant:true,quiet:true})"
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
    }

    // Back from another workspace or app: a fresh frame of the page, at once
    // and again a moment later (as the compositor settles).
    Connections {
        target: win
        function onActiveChanged() { if (win.active) view.refreshSoon() }
        function onVisibilityChanged() { view.refreshSoon() }
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
    Timer { id: refreshTimer; interval: 16; onTriggered: view.js("dogear.refresh && dogear.refresh()") }
    Timer { id: refreshLater; interval: 350; onTriggered: view.js("dogear.refresh && dogear.refresh()") }

    Connections {
        target: reader
        function onSurfaceReady() {
            view.surfaceReady = true
            view.openBook()
        }
        function onBookChanged() {
            if (reader.open && reader.format === "pdf")
                win.tip("pdf", qsTr("PDF pages follow your reading theme. + and − zoom, and the layout (A) shows one page, two, or scrolling."))
            if (reader.open) {
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
        const shift = modifiers & 1, ctrl = modifiers & 2, alt = modifiers & 4
        if (key === "ArrowRight" || key === "PageDown" || key === "l" || key === "j" || (key === " " && !shift)) next()
        else if (key === "ArrowLeft" || key === "PageUp" || key === "h" || key === "k" || (key === " " && shift)) prev()
        else if (key === "Escape") dismiss()
        else if (key === "]") nextChapter()
        else if (key === "[") prevChapter()
        else if (key === "t" || key === "T") contents.show()
        else if (key === "a" || key === "A") appearance.show()
        else if (key === "g" || key === "G") goTo.show()
        else if (key === "d" || key === "D") cycleTheme()
        else if (key === "+" || key === "=") resize(0.05)
        else if (key === "-") resize(-0.05)
        else if (key === "0") resize(0)
        else if (key === "f" || key === "F" || key === "F11") win.toggleFullScreen()
        else if (key === "?") win.helpVisible = !win.helpVisible
        else if ((key === "q" || key === "Q") && !ctrl) Qt.quit()
        else if (alt && key === "ArrowLeft") back()
    }

    Rectangle {
        anchors.fill: parent
        color: view.look.bg
    }

    PdfSurface {
        id: pdfSurface
        anchors.fill: web
        visible: view.pdf
        look: view.look
        focus: view.pdf
    }

    WebEngineView {
        id: web
        anchors.fill: parent
        opacity: view.pdf ? 0 : 1
        enabled: !view.pdf
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
                pageMenu.popup(web, at.x, at.y)
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
            reload()
        }
    }

    WebChannel {
        id: channel
        Component.onCompleted: registerObjects({ bridge: reader })
    }

    // In scroll mode the text passes under the running head and folio;
    // it fades into the page there rather than colliding with them.
    Rectangle {
        visible: view.settings.layout === "scroll"
        anchors.top: parent.top
        width: parent.width
        height: 52
        gradient: Gradient {
            GradientStop { position: 0.45; color: view.look.bg }
            GradientStop { position: 1; color: Ui.tint(view.look.bg, 0) }
        }
    }
    Rectangle {
        visible: view.settings.layout === "scroll"
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
            text: reader.minutesLeftInChapter > 0 && translator.language !== ""
                  ? qsTr("%1 left in chapter").arg(Format.duration(reader.minutesLeftInChapter)) : ""
            color: view.look.muted
            font.pixelSize: 12
        }
        Label {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: {
                const percent = qsTr("%1%").arg(Math.round(reader.progress * 100).toLocaleString(Qt.locale(translator.language), 'f', 0))
                return reader.pageLabel !== "" ? qsTr("p. %1").arg(reader.pageLabel) + "  ·  " + percent : percent
            }
            color: view.look.muted
            font.pixelSize: 12
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
        onTriggered: if (!chromeHover.hovered && !scrubberHover.hovered) view.chromeShown = false
    }
    // The chrome comes when the pointer moves to the top edge, not merely
    // because it rests there when a book opens.
    property real lastPointerY: -1
    HoverHandler {
        id: topEdge
        onPointChanged: {
            const y = point.position.y
            if (view.lastPointerY >= 0 && y !== view.lastPointerY && y < 64 && !view.panelOpen)
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
                tone: view.look.fg
                tip: qsTr("Library (Esc)")
                onClicked: view.closeRequested()
            }
            IconButton {
                glyph: Icons.contents
                tone: view.look.fg
                tip: qsTr("Contents (T)")
                active: contents.open
                onClicked: contents.open ? contents.close() : contents.show()
            }
        }

        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - 420, 640)
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
                tone: view.look.fg
                active: view.pageBookmark > 0
                tip: view.pageBookmark ? qsTr("Remove the bookmark (B)") : qsTr("Bookmark this page (B)")
                onClicked: view.toggleBookmark()
            }
            IconButton {
                glyph: Icons.pen
                tone: view.look.fg
                active: marksPanel.open
                tip: qsTr("Highlights, notes, and bookmarks (M)")
                onClicked: marksPanel.open ? marksPanel.close() : view.showMarks()
            }
            IconButton {
                glyph: Icons.sparkle
                tone: view.look.fg
                active: companionPanel.open
                tip: qsTr("Companion: ask about the book (C)")
                onClicked: companionPanel.open ? companionPanel.close() : view.showCompanion("ask")
            }
            IconButton {
                glyph: Icons.headphones
                tone: view.look.fg
                active: narration.active
                tip: narration.active ? qsTr("Stop listening (⇧P)") : qsTr("Listen from this page (P)")
                onClicked: view.listen()
            }
            IconButton {
                tone: view.look.fg
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
                onClicked: appearance.open ? appearance.close() : appearance.show()
            }
            IconButton {
                glyph: Icons.fullscreen
                tone: view.look.fg
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
        }
        ProgressScrubber {
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
        settings: view.settings
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
            onTriggered: selectionBar.highlight("yellow")
        }
        MenuItem {
            visible: pageMenu.hasSelection
            height: visible ? implicitHeight : 0
            text: qsTr("Add a note")
            onTriggered: selectionBar.note()
        }
        MenuItem {
            visible: pageMenu.hasSelection
            height: visible ? implicitHeight : 0
            text: qsTr("Copy")
            onTriggered: selectionBar.copy()
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

    MarksPanel {
        id: marksPanel
        onClosed: view.focusPage()
        height: parent.height
        look: view.look
        onChosen: (chapter, offset) => view.goTo({ chapter: chapter, offset: offset, instant: true })
    }

    SearchBar {
        id: searchBar
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
    readonly property bool keys: visible && !panelOpen && !win.helpVisible
    // Listening keys work with a panel open too, unless a field is taking typing.
    readonly property bool typing: win.activeFocusItem !== null && win.activeFocusItem.cursorPosition !== undefined
    readonly property bool listenKeys: visible && !win.helpVisible && !typing
    Shortcut { sequence: "C"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.showCompanion("ask") }
    Shortcut { sequences: ["Right", "PgDown", "L", "J", "Space"]; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.next() }
    Shortcut { sequences: ["Left", "PgUp", "H", "K", "Shift+Space"]; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.prev() }
    Shortcut { sequence: "]"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.nextChapter() }
    Shortcut { sequence: "["; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.prevChapter() }
    Shortcut { sequence: "Alt+Left"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.back() }
    Shortcut { sequence: "T"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: contents.show() }
    Shortcut { sequence: "A"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: appearance.show() }
    Shortcut { sequence: "G"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: goTo.show() }
    Shortcut { sequence: "B"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.toggleBookmark() }
    Shortcut { sequence: "M"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.showMarks() }
    Shortcut { sequences: [StandardKey.Redo]; context: Qt.ApplicationShortcut; enabled: view.keys && marks.canRedo; onActivated: marks.redo() }
    Shortcut { sequences: ["/", StandardKey.Find]; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: searchBar.show() }
    Shortcut { sequence: "N"; context: Qt.ApplicationShortcut; enabled: view.keys && bookSearch.count > 0; onActivated: searchBar.next() }
    Shortcut { sequence: "Shift+N"; context: Qt.ApplicationShortcut; enabled: view.keys && bookSearch.count > 0; onActivated: searchBar.previous() }
    Shortcut { sequence: "D"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.cycleTheme() }
    Shortcut { sequences: ["+", "=", "Ctrl++", "Ctrl+="]; context: Qt.ApplicationShortcut; enabled: view.visible; onActivated: view.resize(0.05) }
    Shortcut { sequences: ["-", "Ctrl+-"]; context: Qt.ApplicationShortcut; enabled: view.visible; onActivated: view.resize(-0.05) }
    Shortcut { sequences: ["0", "Ctrl+0"]; context: Qt.ApplicationShortcut; enabled: view.visible; onActivated: view.resize(0) }
    Shortcut { sequence: "Tab"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.chromeShown ? (view.chromeShown = false) : view.showChrome() }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: view.visible && !win.helpVisible; onActivated: view.dismiss() }
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
