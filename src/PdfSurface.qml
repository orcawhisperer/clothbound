import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui
import "Themes.js" as Themes

// A PDF's reading surface: its pages rendered to the reading theme, one at
// a time, two side by side, or scrolling, with margins cut the same on
// every page. It answers the same calls as the EPUB surface (dogear.next(),
// goTo, marks, speak, placeAt…), so the reader around it is the same: a
// page is a chapter, and a place on it an offset in its text.
Item {
    id: surface

    property var look
    property var style: ({})
    readonly property int count: reader.chapterCount
    // The first page in view.
    property int current: 0
    property var history: []
    property var forwardHistory: []
    property bool justJumped: false
    readonly property string layout: style.layout || "auto"
    readonly property bool scrolling: layout === "scroll"
    readonly property bool spread: !scrolling && count > 1
                                   && (layout === "double" || (layout === "auto" && width > height * 1.3))
    readonly property int step: spread ? 2 : 1
    property real zoom: 1.0
    property bool cropping: style.pdfCrop !== undefined ? !!style.pdfCrop : true
    readonly property bool originalColors: !!style.pdfOriginalColors
    readonly property rect crop: cropping ? reader.pdfCrop : Qt.rect(0, 0, 1, 1)
    readonly property real topRoom: style.marginTop || 54
    readonly property real bottomRoom: style.marginBottom || 54

    // What's drawn over the pages.
    property var foundHits: []
    property var foundCurrent: null
    property int foundVersion: 0
    property var speaking: ({ chapter: -1, start: 0, end: 0 })
    property var selection: null   // {chapter, start, end, text}
    property int marksVersion: 0

    signal contextMenuRequested(real x, real y)

    readonly property string colors: originalColors ? ""
                                     : ("?bg=" + encodeURIComponent(look ? look.bg : "#ffffff")
                                        + "&fg=" + encodeURIComponent(look ? look.fg : "#000000"))

    onZoomChanged: if (zoom <= 1.01) pageList.panX = 0

    // --- The surface's calls --------------------------------------------------

    function open(manifest, position, newStyle) {
        style = newStyle
        history = []
        forwardHistory = []
        justJumped = false
        current = 0
        zoom = 1.0
        pageList.panX = 0
        foundHits = []
        foundCurrent = null
        foundVersion++
        selection = null
        speaking = { chapter: -1, start: 0, end: 0 }
        spreadRow.resetSlots()
        goTo(Object.assign({}, position || ({ chapter: 0 }), { quiet: true, instant: true }))
        return Promise.resolve(true)
    }
    function setStyle(newStyle) { style = newStyle; prefetchTimer.restart(); report() }
    function next() {
        if (scrolling) {
            pageList.contentY = Math.min(Math.max(0, pageList.contentHeight - pageList.height), pageList.contentY + pageList.height * 0.9)
            return
        }
        const from = firstOfSpread(current)
        const target = Math.min(count - 1, from + step)
        if (target !== from && typeof reader.notifyPageTurn === "function")
            reader.notifyPageTurn(1)
        show(target, false, target !== from ? 1 : 0, true)
        reader.tapped("turn")
    }
    function prev() {
        if (scrolling) {
            pageList.contentY = Math.max(0, pageList.contentY - pageList.height * 0.9)
            return
        }
        const from = firstOfSpread(current)
        const target = Math.max(0, from - step)
        if (target !== from && typeof reader.notifyPageTurn === "function")
            reader.notifyPageTurn(-1)
        show(target, false, target !== from ? -1 : 0, true)
        reader.tapped("turn")
    }
    function nextChapter() {
        for (const entry of reader.contents)
            if (entry.chapter > current + step - 1) {
                if (typeof reader.notifyPageTurn === "function")
                    reader.notifyPageTurn(1)
                goTo({ chapter: entry.chapter, dir: 1 })
                return
            }
    }
    function prevChapter() {
        let target = 0
        for (const entry of reader.contents)
            if (entry.chapter < current)
                target = entry.chapter
        if (target !== current && typeof reader.notifyPageTurn === "function")
            reader.notifyPageTurn(-1)
        goTo({ chapter: target, dir: target !== current ? -1 : 0 })
    }
    function recordHistoryEntry(page) {
        const entry = { chapter: page, pageLabel: reader.pdfPageLabel(page) }
        const top = history.length ? history[history.length - 1] : null
        if (!top || top.chapter !== entry.chapter)
            history = history.concat([entry]).slice(-50)
        forwardHistory = []
        justJumped = true
    }
    function back() {
        if (!history.length)
            return false
        const entry = history[history.length - 1]
        history = history.slice(0, -1)
        forwardHistory = forwardHistory.concat([{ chapter: current, pageLabel: reader.pdfPageLabel(current) }]).slice(-50)
        justJumped = false
        show(entry.chapter, false, 0, true)
        return true
    }
    function forward() {
        if (!forwardHistory.length)
            return false
        const entry = forwardHistory[forwardHistory.length - 1]
        forwardHistory = forwardHistory.slice(0, -1)
        history = history.concat([{ chapter: current, pageLabel: reader.pdfPageLabel(current) }]).slice(-50)
        justJumped = false
        show(entry.chapter, false, 0, true)
        return true
    }
    function canGoBack() { return history.length > 0 }
    function canGoForward() { return forwardHistory.length > 0 }
    function goTo(target) {
        if (!target)
            return Promise.resolve()
        const page = Math.max(0, Math.min(count - 1, target.chapter || 0))
        if (!target.quiet && page !== current)
            recordHistoryEntry(current)
        show(page, !!target.automated, target.dir || 0, !target.instant)
        return Promise.resolve()
    }
    function listeningStart() { return { chapter: current, offset: 0 } }
    function clearSelection() { selection = null }
    function dismiss() {
        if (selection) {
            selection = null
            reader.selectionChanged({})
            return true
        }
        return false
    }
    function found(hits, currentHit) {
        foundHits = hits || []
        foundCurrent = currentHit
        foundVersion++
    }
    function setMarks() { marksVersion++ }
    function speak(start, end, follow) {
        speaking = { chapter: narration.chapter, start: start, end: end }
        if (follow && !visiblePages().includes(narration.chapter))
            show(narration.chapter, true)
    }
    function highlight(name, spans) {
        if (name === "dg-speaking" && (!spans || !spans.length))
            speaking = { chapter: -1, start: 0, end: 0 }
    }
    function requestSpeakable(page) { reader.pdfSpeakable(page) }
    function sampleText() { return reader.pdfPageText(current).replace(/\s+/g, " ").slice(0, 700) }
    function restoreDecorations() {}
    // A point on the surface (in its own terms): the page there, and the
    // sentence the point is in.
    function placeAt(x, y) {
        const hit = pageAt(x, y)
        if (!hit)
            return null
        const words = reader.pdfSelect(hit.page, Math.max(0, hit.x - 0.002), hit.y, hit.x + 0.002, hit.y)
        let offset = words.start !== undefined ? words.start : 0
        const text = reader.pdfPageText(hit.page)
        while (offset > 0 && !(offset >= 2 && /[.!?\u2026]\s/.test(text.slice(offset - 2, offset))) && text[offset - 1] !== "\n")
            --offset
        return { chapter: hit.page, offset: offset, selected: selection ? selection.text : "" }
    }

    // --- Pages ----------------------------------------------------------------

    function firstOfSpread(page) {
        // Spreads pair pages as a printed book does: the cover alone, then
        // left and right.
        return spread ? (page === 0 ? 0 : page - ((page - 1) % 2)) : page
    }
    function visiblePages() {
        if (scrolling) {
            const out = []
            for (let i = 0; i < pageList.count; ++i) {
                const item = pageList.itemAtIndex(i)
                if (item && item.y + item.height > pageList.contentY && item.y < pageList.contentY + pageList.height)
                    out.push(i)
            }
            return out.length ? out : [current]
        }
        const first = firstOfSpread(current)
        return spread && first > 0 && first + 1 < count ? [first, first + 1] : [first]
    }
    function show(page, automated, dir, animate) {
        current = Math.max(0, Math.min(count - 1, page))
        if (scrolling)
            pageList.positionViewAtIndex(current, ListView.Beginning)
        else
            spreadRow.present(visiblePages(), dir || 0, animate !== false)
        selection = null
        report(!!automated)
    }
    function report(automated) {
        if (!reader.open)
            return
        const pages = visiblePages()
        const page = pages[0]
        const length = reader.charactersIn(page)
        const label = reader.pdfPageLabel(page)
        const backEntry = history.length ? history[history.length - 1] : null
        const jumped = justJumped
        justJumped = false
        reader.relocated({ chapter: page, offset: 0, end: length, page: 0, pages: 1,
                           columns: pages.length,
                           visiblePageEnd: pages[pages.length - 1],
                           pageLabel: label !== "" ? label : String(page + 1),
                           atEnd: pages[pages.length - 1] >= count - 1,
                           canGoBack: history.length > 0,
                           canGoForward: forwardHistory.length > 0,
                           backChapter: backEntry ? backEntry.chapter : -1,
                           backPageLabel: backEntry ? (backEntry.pageLabel || "") : "",
                           jumped: jumped,
                           automated: !!automated,
                           snippet: reader.pdfPageText(page).replace(/\s+/g, " ").slice(0, 120) })
    }
    // The page under a point of the surface, and where on it (0 to 1).
    function pageAt(x, y) {
        const items = scrolling
            ? Array.from({ length: pageList.count }, (_, i) => pageList.itemAtIndex(i))
            : spreadRow.activePageItems()
        for (const item of items) {
            if (!item || item.page === undefined || item.page < 0 || !item.visible)
                continue
            const at = item.mapFromItem(surface, x, y)
            if (at.x >= 0 && at.y >= 0 && at.x <= item.width && at.y <= item.height)
                return { page: item.page, x: item.toPageX(at.x), y: item.toPageY(at.y), item: item }
        }
        return null
    }

    function prefetchAdjacent() {
        if (!reader.open || surface.count <= 1 || surface.scrolling)
            return
        const first = surface.firstOfSpread(surface.current)
        const nextFirst = Math.min(surface.count - 1, first + surface.step)
        const prevFirst = Math.max(0, first - surface.step)
        const targets = []
        const pushSpread = (start) => {
            if (start < 0 || start >= surface.count)
                return
            if (targets.indexOf(start) < 0)
                targets.push(start)
            if (surface.spread && start > 0 && start + 1 < surface.count && targets.indexOf(start + 1) < 0)
                targets.push(start + 1)
        }
        if (nextFirst !== first)
            pushSpread(nextFirst)
        if (prevFirst !== first)
            pushSpread(prevFirst)
        if (!targets.length)
            return
        const fullW = spreadRow.activeSlotItem ? (spreadRow.activeSlotItem.fitWidth / surface.crop.width) : 600
        const pixelWidth = Math.max(64, Math.ceil(fullW * Screen.devicePixelRatio / 64) * 64)
        if (typeof reader.pdfPrefetch === "function")
            reader.pdfPrefetch(targets, pixelWidth, surface.colors)
    }

    Timer {
        id: prefetchTimer
        interval: 45
        onTriggered: surface.prefetchAdjacent()
    }

    onLayoutChanged: Qt.callLater(() => show(current, false, 0, false))
    onSpreadChanged: Qt.callLater(() => show(current, false, 0, false))
    onColorsChanged: prefetchTimer.restart()
    Connections {
        target: marks
        function onChanged() { surface.marksVersion++ }
    }

    // One page: rendered, cropped, and drawn over (highlights, search,
    // links, the sentence being read, the selection).
    component Page: Item {
        id: pageItem
        property int page: -1
        // "left" or "right" in a 2-page spread so the center spine crease aligns.
        property string spineSide: ""
        // Standby pages render silently off-stage while the current spread stays visible.
        property bool suppressPlaceholder: false
        // The size shown, before cropping.
        property real fullWidth: 100
        readonly property var points: page >= 0 ? reader.pdfPageSize(page) : ({ width: 612, height: 792 })
        readonly property real fullHeight: fullWidth * (points.height || 792) / (points.width || 612)
        readonly property var pageLinks: page >= 0 ? reader.pdfLinks(page) : []
        readonly property bool imageReady: page < 0 || reader.pdfSource === "" || pageImage.status === Image.Ready || pageImage.status === Image.Error
        property bool showPlaceholder: false
        width: fullWidth * surface.crop.width
        height: fullHeight * surface.crop.height

        function toPageX(x) { return (x + surface.crop.x * fullWidth) / fullWidth }
        function toPageY(y) { return (y + surface.crop.y * fullHeight) / fullHeight }
        function linkAt(px, py) {
            for (const link of pageLinks)
                if (px >= link.x && px <= link.x + link.width && py >= link.y && py <= link.y + link.height)
                    return link
            return null
        }
        function reportSelection(chosen) {
            surface.selection = chosen
            const rects = reader.pdfRects(pageItem.page, chosen.start, chosen.end)
            if (!rects.length)
                return
            let top = 1, left = 1, right = 0, bottom = 0
            for (const r of rects) {
                top = Math.min(top, r.y); left = Math.min(left, r.x)
                right = Math.max(right, r.x + r.width); bottom = Math.max(bottom, r.y + r.height)
            }
            const at = whole.mapToItem(surface, left * whole.width, top * whole.height)
            reader.selectionChanged(Object.assign({}, chosen, {
                x: at.x, y: at.y, width: (right - left) * whole.width, height: (bottom - top) * whole.height }))
        }

        // Subtle two-layer paper drop shadow behind the clipped page sheet.
        Rectangle {
            z: -2
            anchors.fill: parent
            anchors.leftMargin: pageItem.spineSide === "right" ? 0 : -4
            anchors.rightMargin: pageItem.spineSide === "left" ? 0 : -4
            anchors.topMargin: 2
            anchors.bottomMargin: -8
            radius: 4
            color: "black"
            opacity: surface.look && surface.look.dark ? 0.40 : 0.09
        }
        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.leftMargin: pageItem.spineSide === "right" ? 0 : -1
            anchors.rightMargin: pageItem.spineSide === "left" ? 0 : -1
            anchors.topMargin: 1
            anchors.bottomMargin: -3
            radius: 2
            color: "black"
            opacity: surface.look && surface.look.dark ? 0.28 : 0.07
        }

        Item {
            id: pageClip
            anchors.fill: parent
            clip: true

            Rectangle {
                anchors.fill: parent
                color: surface.originalColors ? "white" : (surface.look ? surface.look.bg : "white")
            }
            Item {
                id: whole
                x: -surface.crop.x * pageItem.fullWidth
                y: -surface.crop.y * pageItem.fullHeight
                width: pageItem.fullWidth
                height: pageItem.fullHeight
                Image {
                    id: pageImage
                    anchors.fill: parent
                    asynchronous: true
                    cache: false
                    retainWhileLoading: true
                    source: pageItem.page >= 0 && reader.pdfSource !== "" ? reader.pdfSource + "/" + pageItem.page + surface.colors : ""
                    // Bucket width to 64 px steps so slight resizes hit the LRU cache.
                    sourceSize.width: Math.max(64, Math.ceil(pageItem.fullWidth * Screen.devicePixelRatio / 64) * 64)
                    fillMode: Image.Stretch
                    onStatusChanged: {
                        if (status === Image.Loading && !pageItem.suppressPlaceholder) {
                            placeholderDelay.restart()
                        } else {
                            placeholderDelay.stop()
                            pageItem.showPlaceholder = false
                        }
                    }
                }
                Timer {
                    id: placeholderDelay
                    interval: 180
                    onTriggered: if (pageImage.status === Image.Loading && !pageItem.suppressPlaceholder) pageItem.showPlaceholder = true
                }
                // Highlights.
                Repeater {
                    model: {
                        surface.marksVersion
                        const out = []
                        if (pageItem.page < 0)
                            return out
                        for (const mark of marks.highlightsIn(pageItem.page))
                            for (const rect of reader.pdfRects(pageItem.page, mark.start, mark.end))
                                out.push({ rect: rect, color: mark.color, id: mark.id, noted: !!mark.noted })
                        return out
                    }
                    delegate: Rectangle {
                        required property var modelData
                        x: modelData.rect.x * whole.width
                        y: modelData.rect.y * whole.height
                        width: modelData.rect.width * whole.width
                        height: modelData.rect.height * whole.height
                        color: Themes.swatches[modelData.color] || Themes.swatches.yellow
                        opacity: 0.38
                        Rectangle {
                            visible: modelData.noted
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 2
                            color: surface.look ? surface.look.link : "#8a5a12"
                            opacity: 1.0
                        }
                    }
                }
                // Search results (handles both chapter-tagged hits and per-page queries).
                Repeater {
                    model: {
                        surface.foundVersion
                        const out = []
                        if (pageItem.page < 0)
                            return out
                        let pageHits = bookSearch.count > 0 ? bookSearch.inChapter(pageItem.page) : []
                        if (!pageHits.length && surface.foundHits.length)
                            pageHits = surface.foundHits.filter(h => (h.chapter !== undefined ? h.chapter : reader.chapter) === pageItem.page)
                        const cur = surface.foundCurrent
                        const curChapter = cur && cur.chapter !== undefined ? cur.chapter : reader.chapter
                        for (const hit of pageHits)
                            for (const rect of reader.pdfRects(pageItem.page, hit.start, hit.end))
                                out.push({ rect: rect, current: !!cur && cur.start === hit.start && curChapter === pageItem.page })
                        return out
                    }
                    delegate: Item {
                        required property var modelData
                        x: modelData.rect.x * whole.width - 1
                        y: modelData.rect.y * whole.height - 1
                        width: modelData.rect.width * whole.width + 2
                        height: modelData.rect.height * whole.height + 2
                        Rectangle {
                            visible: modelData.current
                            anchors.fill: parent
                            radius: 2
                            color: theme.accent
                            opacity: surface.look && surface.look.dark ? 0.28 : 0.22
                        }
                        Rectangle {
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: modelData.current ? 2.5 : 2
                            color: surface.look ? (surface.look.searchUnderline || surface.look.link) : "#3E5F99"
                        }
                    }
                }
                // The sentence being read aloud (theme.accent @ 28–30%).
                Repeater {
                    model: surface.speaking.chapter === pageItem.page
                           ? reader.pdfRects(pageItem.page, surface.speaking.start, surface.speaking.end) : []
                    delegate: Rectangle {
                        required property var modelData
                        x: modelData.x * whole.width - 2
                        y: modelData.y * whole.height - 1
                        width: modelData.width * whole.width + 4
                        height: modelData.height * whole.height + 2
                        radius: 3
                        color: theme.accent
                        opacity: surface.look && surface.look.dark ? 0.30 : 0.28
                    }
                }
                // The selection.
                Repeater {
                    model: surface.selection && surface.selection.chapter === pageItem.page
                           ? reader.pdfRects(pageItem.page, surface.selection.start, surface.selection.end) : []
                    delegate: Rectangle {
                        required property var modelData
                        x: modelData.x * whole.width
                        y: modelData.y * whole.height
                        width: modelData.width * whole.width
                        height: modelData.height * whole.height
                        color: surface.look ? surface.look.link : "#4a90d9"
                        opacity: 0.3
                    }
                }
                // Hovered internal/external PDF link affordance.
                Rectangle {
                    visible: mouse.hoveredLink !== null
                    x: mouse.hoveredLink ? mouse.hoveredLink.x * whole.width - 2 : 0
                    y: mouse.hoveredLink ? mouse.hoveredLink.y * whole.height - 1 : 0
                    width: mouse.hoveredLink ? mouse.hoveredLink.width * whole.width + 4 : 0
                    height: mouse.hoveredLink ? mouse.hoveredLink.height * whole.height + 2 : 0
                    radius: 2
                    color: surface.look ? Ui.tint(surface.look.link, 0.14) : "transparent"
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1.5
                        color: surface.look ? surface.look.link : "#4a90d9"
                        opacity: 0.75
                    }
                }
            }

            // Gentle loading placeholder while an uncached PDF page renders off-thread.
            Item {
                anchors.fill: parent
                visible: pageItem.showPlaceholder
                Column {
                    anchors.centerIn: parent
                    spacing: 12
                    BusyIndicator {
                        anchors.horizontalCenter: parent.horizontalCenter
                        running: pageItem.showPlaceholder
                        implicitWidth: 26
                        implicitHeight: 26
                        opacity: 0.55
                    }
                    Label {
                        anchors.horizontalCenter: parent.horizontalCenter
                        text: pageItem.page >= 0 ? qsTr("p. %1").arg(pageItem.page + 1) : ""
                        color: surface.look ? surface.look.muted : "#888"
                        font.pixelSize: 12
                    }
                }
            }

            // Center book-spine gutter crease in 2-page spread mode.
            Rectangle {
                visible: pageItem.spineSide === "left"
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.right: parent.right
                width: 22
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: "transparent" }
                    GradientStop { position: 1.0; color: Ui.tint("black", surface.look && surface.look.dark ? 0.36 : 0.11) }
                }
            }
            Rectangle {
                visible: pageItem.spineSide === "right"
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                width: 22
                gradient: Gradient {
                    orientation: Gradient.Horizontal
                    GradientStop { position: 0.0; color: Ui.tint("black", surface.look && surface.look.dark ? 0.36 : 0.11) }
                    GradientStop { position: 1.0; color: "transparent" }
                }
            }
            Rectangle {
                visible: pageItem.spineSide === "right"
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                width: 1
                color: surface.look ? surface.look.line : "transparent"
            }

            // Crisp hairline page border drawn over the sheet edge.
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.width: 1
                border.color: surface.look ? surface.look.line : "transparent"
            }
        }

        // Compact 22×22 margin note badges beside noted PDF highlights (02_reading.html).
        Repeater {
            model: {
                surface.marksVersion
                const out = []
                if (pageItem.page < 0)
                    return out
                for (const mark of marks.highlightsIn(pageItem.page)) {
                    if (!mark.noted || !mark.note)
                        continue
                    const rects = reader.pdfRects(pageItem.page, mark.start, mark.end)
                    if (rects.length > 0)
                        out.push({ id: mark.id, color: mark.color, note: mark.note, rect: rects[0] })
                }
                return out
            }
            delegate: AbstractButton {
                id: pdfNoteBadge
                required property var modelData
                required property int index
                readonly property real lineTop: (modelData.rect.y - surface.crop.y) * pageItem.fullHeight
                readonly property real lineHeight: modelData.rect.height * pageItem.fullHeight
                z: 5
                visible: lineTop >= -12 && lineTop <= pageItem.height + 12
                x: pageItem.spineSide === "left" ? -32 : pageItem.width + 10
                y: Math.max(4, Math.min(pageItem.height - height - 4, lineTop + Math.max(0, (lineHeight - height) / 2)))
                width: 22
                height: 22
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                Accessible.name: qsTr("Note: %1").arg(modelData.note)
                onClicked: {
                    const at = whole.mapToItem(surface, modelData.rect.x * whole.width, modelData.rect.y * whole.height)
                    reader.markClicked(modelData.id, { x: at.x, y: at.y,
                                                       width: modelData.rect.width * whole.width,
                                                       height: modelData.rect.height * whole.height })
                }
                background: Rectangle {
                    radius: Ui.radius(6, theme.radius)
                    color: Themes.swatches[pdfNoteBadge.modelData.color] || (surface.look ? surface.look.highlight : "#F7DE8A")
                    opacity: pdfNoteBadge.hovered ? 1.0 : 0.92
                }
                contentItem: Item {
                    Glyph {
                        anchors.centerIn: parent
                        width: 12
                        height: 12
                        stroke: 2
                        path: Icons.noteLines
                        color: surface.look ? surface.look.fg : "#26221D"
                    }
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                ToolTip.visible: hovered
                ToolTip.delay: 350
                ToolTip.text: modelData.note
            }
        }

        // Selecting with the mouse (drag or double-click a word), panning while
        // zoomed in (Shift+drag still selects); right-click opens the page menu;
        // clicking a highlight or PDF link opens it; clicking the margins turns.
        MouseArea {
            id: mouse
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: surface.zoom > 1.01
                ? (pressed && panning ? Qt.ClosedHandCursor : Qt.OpenHandCursor)
                : (hoveredLink ? Qt.PointingHandCursor : Qt.IBeamCursor)
            preventStealing: true
            property point from
            property bool dragged: false
            property bool panning: false
            property bool skipRelease: false
            property var hoveredLink: null
            property point panStart
            property real startContentX: 0
            property real startContentY: 0
            ToolTip.visible: hoveredLink !== null && !pressed
            ToolTip.delay: 450
            ToolTip.text: {
                if (!hoveredLink)
                    return ""
                if (hoveredLink.url && hoveredLink.url !== "")
                    return hoveredLink.url.replace(/^https?:\/\//, "").split("/")[0]
                if (hoveredLink.page >= 0) {
                    const lbl = reader.pdfPageLabel(hoveredLink.page)
                    return qsTr("Go to p. %1").arg(lbl !== "" ? lbl : String(hoveredLink.page + 1))
                }
                return ""
            }
            onExited: hoveredLink = null
            onPressed: (event) => {
                if (event.button === Qt.RightButton) {
                    const at = mouse.mapToItem(surface, event.x, event.y)
                    surface.contextMenuRequested(at.x, at.y)
                    event.accepted = true
                    return
                }
                skipRelease = false
                from = Qt.point(pageItem.toPageX(event.x), pageItem.toPageY(event.y))
                dragged = false
                panning = surface.zoom > 1.01 && !(event.modifiers & Qt.ShiftModifier)
                if (panning) {
                    panStart = mouse.mapToItem(surface, event.x, event.y)
                    startContentX = surface.scrolling ? pageList.panX : zoomed.contentX
                    startContentY = surface.scrolling ? pageList.contentY : zoomed.contentY
                } else {
                    surface.selection = null
                }
            }
            onDoubleClicked: (event) => {
                if (event.button === Qt.RightButton)
                    return
                const px = pageItem.toPageX(event.x), py = pageItem.toPageY(event.y)
                const word = reader.pdfSelectWord(pageItem.page, px, py)
                if (word && word.text) {
                    skipRelease = true
                    dragged = false
                    pageItem.reportSelection(word)
                }
            }
            onPositionChanged: (event) => {
                if (!(event.buttons & Qt.LeftButton)) {
                    hoveredLink = pageItem.linkAt(pageItem.toPageX(event.x), pageItem.toPageY(event.y))
                    return
                }
                if (panning) {
                    const now = mouse.mapToItem(surface, event.x, event.y)
                    const dx = now.x - panStart.x, dy = now.y - panStart.y
                    if (!dragged && Math.abs(dx) + Math.abs(dy) < 4)
                        return
                    dragged = true
                    if (surface.scrolling) {
                        pageList.panX = Math.max(-pageList.maxPanX, Math.min(pageList.maxPanX, startContentX - dx))
                        pageList.contentY = Math.max(0, Math.min(Math.max(0, pageList.contentHeight - pageList.height), startContentY - dy))
                    } else {
                        zoomed.contentX = Math.max(0, Math.min(Math.max(0, zoomed.contentWidth - zoomed.width), startContentX - dx))
                        zoomed.contentY = Math.max(0, Math.min(Math.max(0, zoomed.contentHeight - zoomed.height), startContentY - dy))
                    }
                    return
                }
                const to = Qt.point(pageItem.toPageX(event.x), pageItem.toPageY(event.y))
                if (Math.abs(to.x - from.x) + Math.abs(to.y - from.y) < 0.004)
                    return
                dragged = true
                const chosen = reader.pdfSelect(pageItem.page, from.x, from.y, to.x, to.y)
                surface.selection = chosen.text ? chosen : null
            }
            onReleased: (event) => {
                if (event.button === Qt.RightButton)
                    return
                if (skipRelease) {
                    skipRelease = false
                    panning = false
                    return
                }
                if (panning && dragged) {
                    panning = false
                    return
                }
                panning = false
                if (dragged && surface.selection) {
                    pageItem.reportSelection(surface.selection)
                    return
                }
                reader.selectionChanged({})
                const x = pageItem.toPageX(event.x), y = pageItem.toPageY(event.y)
                // A highlight under the click opens it.
                for (const mark of marks.highlightsIn(pageItem.page))
                    for (const r of reader.pdfRects(pageItem.page, mark.start, mark.end))
                        if (x >= r.x && x <= r.x + r.width && y >= r.y && y <= r.y + r.height) {
                            const at = whole.mapToItem(surface, r.x * whole.width, r.y * whole.height)
                            reader.markClicked(mark.id, { x: at.x, y: at.y, width: r.width * whole.width, height: r.height * whole.height })
                            return
                        }
                // Clickable PDF link: external URL or internal page jump with history.
                const link = pageItem.linkAt(x, y)
                if (link) {
                    if (link.url && link.url !== "") {
                        reader.openExternal(link.url)
                        return
                    }
                    if (link.page >= 0) {
                        surface.goTo({ chapter: link.page, recordHistory: true })
                        return
                    }
                }
                // Otherwise, as on the EPUB page: the sides turn, the middle shows the toolbar.
                const across = mouse.mapToItem(surface, event.x, event.y).x / surface.width
                if (surface.scrolling || (across > 0.3 && across < 0.7))
                    reader.tapped("center")
                else if (across <= 0.3)
                    surface.prev()
                else
                    surface.next()
            }
        }
    }

    // Double-buffered persistent spread slot so turning pages holds the previous
    // rendered sheet until the next image reaches Image.Ready (zero blank flash).
    component SpreadSlot: Row {
        id: slot
        spacing: 0
        property var pages: []
        property bool isStandby: false
        property real slideOffset: 0
        readonly property int p0: pages.length > 0 ? pages[0] : -1
        readonly property int p1: pages.length > 1 ? pages[1] : -1
        readonly property bool ready: pages.length > 0 && leftPageItem.imageReady && rightPageItem.imageReady
        readonly property alias leftPage: leftPageItem
        readonly property alias rightPage: rightPageItem
        readonly property real fitWidth: {
            const first = p0 >= 0 ? p0 : surface.firstOfSpread(surface.current)
            const size = reader.pdfPageSize(first)
            const aspect = ((size.height || 792) * surface.crop.height) / ((size.width || 612) * surface.crop.width)
            const pageCount = pages.length > 1 ? 2 : 1
            const each = Math.min(spreadRow.roomWidth / pageCount, spreadRow.roomHeight / aspect)
            return Math.max(100, each) * surface.zoom
        }
        visible: pages.length > 0
        enabled: !isStandby
        x: Math.max(24, (zoomed.width - width) / 2) + slideOffset
        y: Math.max(surface.topRoom - 10, (zoomed.height - height) / 2)

        Page {
            id: leftPageItem
            visible: slot.p0 >= 0
            page: slot.p0
            suppressPlaceholder: slot.isStandby
            spineSide: slot.p1 >= 0 ? "left" : ""
            fullWidth: slot.fitWidth / surface.crop.width
        }
        Page {
            id: rightPageItem
            visible: slot.p1 >= 0
            page: slot.p1
            suppressPlaceholder: slot.isStandby
            spineSide: slot.p1 >= 0 ? "right" : ""
            fullWidth: slot.fitWidth / surface.crop.width
        }
    }

    // --- One page or two --------------------------------------------------------
    Item {
        id: spreadRow
        anchors.fill: parent
        visible: !surface.scrolling
        // The space for pages, under the title and above the listen bar.
        readonly property real roomHeight: Math.max(120, height - surface.topRoom - surface.bottomRoom + 20)
        readonly property real roomWidth: Math.max(120, width - 48)
        property int activeSlot: 0
        property var pendingPages: []
        property int pendingDir: 0
        property bool pendingAnimate: false
        property bool showSlowJump: false
        readonly property SpreadSlot activeSlotItem: activeSlot === 0 ? spreadSlotA : spreadSlotB
        readonly property SpreadSlot standbySlotItem: activeSlot === 0 ? spreadSlotB : spreadSlotA

        function samePages(a, b) {
            if (!a || !b || a.length !== b.length)
                return false
            for (let i = 0; i < a.length; ++i)
                if (a[i] !== b[i])
                    return false
            return true
        }
        function activePageItems() {
            const s = activeSlotItem
            return s ? [s.leftPage, s.rightPage] : []
        }
        function finishAnim() {
            if (spreadAnim.running)
                spreadAnim.complete()
        }
        function resetSlots() {
            finishAnim()
            slowJumpTimer.stop()
            showSlowJump = false
            pendingPages = []
            pendingDir = 0
            pendingAnimate = false
            activeSlot = 0
            spreadSlotA.pages = []
            spreadSlotA.opacity = 1
            spreadSlotA.slideOffset = 0
            spreadSlotB.pages = []
            spreadSlotB.opacity = 0
            spreadSlotB.slideOffset = 0
        }
        function present(pages, dir, animate) {
            finishAnim()
            if (!pages || !pages.length)
                return
            const cur = activeSlotItem
            const back = standbySlotItem
            if (!cur.pages.length || samePages(cur.pages, pages)) {
                cur.pages = pages
                cur.opacity = 1
                cur.slideOffset = 0
                back.opacity = 0
                back.slideOffset = 0
                pendingPages = []
                slowJumpTimer.stop()
                showSlowJump = false
                prefetchTimer.restart()
                return
            }
            pendingPages = pages
            pendingDir = dir
            pendingAnimate = animate
            back.opacity = 0
            back.slideOffset = 0
            back.pages = pages
            if (back.ready) {
                Qt.callLater(spreadRow.commitPending)
            } else {
                slowJumpTimer.restart()
            }
        }
        function commitPending() {
            if (!pendingPages.length)
                return
            const outgoing = activeSlotItem
            const incoming = standbySlotItem
            if (!samePages(incoming.pages, pendingPages) || !incoming.ready)
                return
            slowJumpTimer.stop()
            showSlowJump = false
            const dir = pendingDir
            const doAnim = pendingAnimate && surface.style.animate !== false
            pendingPages = []
            activeSlot = 1 - activeSlot
            if (!doAnim) {
                incoming.slideOffset = 0
                incoming.opacity = 1
                outgoing.slideOffset = 0
                outgoing.opacity = 0
                prefetchTimer.restart()
                return
            }
            const reduced = theme.reducedMotion || dir === 0
            const travel = reduced ? 0 : Math.min(84, Math.max(36, Math.round(spreadRow.width * 0.06)))
            const dur = reduced ? 130 : 195
            outgoing.slideOffset = 0
            outgoing.opacity = 1
            incoming.slideOffset = dir * travel
            incoming.opacity = 0
            inFade.target = incoming; inFade.duration = dur
            outFade.target = outgoing; outFade.duration = dur
            inSlide.target = incoming; inSlide.from = dir * travel; inSlide.to = 0; inSlide.duration = dur
            outSlide.target = outgoing; outSlide.from = 0; outSlide.to = -dir * travel * 0.65; outSlide.duration = dur
            spreadAnim.restart()
            prefetchTimer.restart()
        }

        Timer {
            id: slowJumpTimer
            interval: 260
            onTriggered: if (spreadRow.pendingPages.length > 0) spreadRow.showSlowJump = true
        }

        ParallelAnimation {
            id: spreadAnim
            NumberAnimation { id: inFade; property: "opacity"; from: 0; to: 1; easing.type: Easing.OutCubic }
            NumberAnimation { id: outFade; property: "opacity"; from: 1; to: 0; easing.type: Easing.OutQuad }
            NumberAnimation { id: inSlide; property: "slideOffset"; easing.type: Easing.OutCubic }
            NumberAnimation { id: outSlide; property: "slideOffset"; easing.type: Easing.OutCubic }
        }

        Flickable {
            id: zoomed
            anchors.fill: parent
            contentWidth: Math.max(width, (spreadRow.activeSlotItem ? spreadRow.activeSlotItem.width : 0) + 48)
            contentHeight: Math.max(height, (spreadRow.activeSlotItem ? spreadRow.activeSlotItem.height : 0) + surface.topRoom + surface.bottomRoom)
            interactive: surface.zoom > 1.01
            boundsBehavior: Flickable.StopAtBounds
            clip: true

            SpreadSlot {
                id: spreadSlotA
                z: spreadRow.activeSlot === 0 ? 2 : 1
                isStandby: spreadRow.activeSlot !== 0
                opacity: 1
                onReadyChanged: if (isStandby && ready && spreadRow.pendingPages.length > 0) spreadRow.commitPending()
            }
            SpreadSlot {
                id: spreadSlotB
                z: spreadRow.activeSlot === 1 ? 2 : 1
                isStandby: spreadRow.activeSlot !== 1
                opacity: 0
                onReadyChanged: if (isStandby && ready && spreadRow.pendingPages.length > 0) spreadRow.commitPending()
            }

            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                acceptedModifiers: Qt.ControlModifier
                onWheel: (event) => surface.zoom = Math.max(1, Math.min(4, surface.zoom * (event.angleDelta.y > 0 ? 1.1 : 0.9)))
            }
        }

        // Subtle unobtrusive indicator only if an uncached far-jump render takes >260 ms
        // (the previous page remains visible underneath, never flashing blank).
        Rectangle {
            z: 5
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.topMargin: surface.topRoom + 8
            anchors.rightMargin: 28
            width: 32
            height: 32
            radius: 16
            color: surface.look ? surface.look.popover : "#26221D"
            border.width: 1
            border.color: surface.look ? surface.look.line : "transparent"
            opacity: spreadRow.showSlowJump ? 0.9 : 0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
            BusyIndicator {
                anchors.centerIn: parent
                running: parent.visible
                implicitWidth: 18
                implicitHeight: 18
            }
        }
        // The wheel turns pages, as on the EPUB page (unless zoomed in), with
        // an inertia cooldown so touchpad flicks advance one spread at a time.
        Timer {
            id: spreadWheelReset
            interval: 200
            onTriggered: spreadWheel.gathered = 0
        }
        WheelHandler {
            id: spreadWheel
            enabled: surface.zoom <= 1.01
            acceptedModifiers: Qt.NoModifier
            property real gathered: 0
            property double quietUntil: 0
            onWheel: (event) => {
                const now = Date.now()
                if (now < quietUntil)
                    return
                const delta = Math.abs(event.angleDelta.x) > Math.abs(event.angleDelta.y)
                              ? -event.angleDelta.x : event.angleDelta.y
                gathered += delta
                spreadWheelReset.restart()
                if (gathered <= -120) {
                    gathered = 0
                    quietUntil = now + 280
                    surface.next()
                } else if (gathered >= 120) {
                    gathered = 0
                    quietUntil = now + 280
                    surface.prev()
                }
            }
        }
    }

    // --- Scrolling ---------------------------------------------------------------
    ListView {
        id: pageList
        anchors.fill: parent
        anchors.topMargin: surface.topRoom - 14
        anchors.bottomMargin: surface.bottomRoom - 14
        visible: surface.scrolling
        model: surface.scrolling ? surface.count : 0
        spacing: 8
        cacheBuffer: Math.max(0, height * 2)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollBar.vertical: ScrollBar {}
        property real panX: 0
        readonly property real baseWidth: Math.min(Math.max(120, width - 48), 920)
        readonly property real maxPanX: Math.max(0, (baseWidth * surface.zoom - Math.max(120, width - 48)) / 2)
        onMaxPanXChanged: panX = Math.max(-maxPanX, Math.min(maxPanX, panX))
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            acceptedModifiers: Qt.ControlModifier
            onWheel: (event) => surface.zoom = Math.max(1, Math.min(4, surface.zoom * (event.angleDelta.y > 0 ? 1.1 : 0.9)))
        }
        WheelHandler {
            enabled: surface.zoom > 1.01
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            acceptedModifiers: Qt.ShiftModifier
            onWheel: (event) => {
                const delta = event.angleDelta.x !== 0 ? event.angleDelta.x : event.angleDelta.y
                pageList.panX = Math.max(-pageList.maxPanX, Math.min(pageList.maxPanX, pageList.panX - delta * 0.5))
            }
        }
        WheelHandler {
            enabled: surface.zoom > 1.01
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            acceptedModifiers: Qt.NoModifier
            onWheel: (event) => {
                if (Math.abs(event.angleDelta.x) > Math.abs(event.angleDelta.y)) {
                    pageList.panX = Math.max(-pageList.maxPanX, Math.min(pageList.maxPanX, pageList.panX - event.angleDelta.x * 0.5))
                } else {
                    event.accepted = false
                }
            }
        }
        delegate: Item {
            required property int index
            readonly property int page: index
            width: pageList.width
            height: inner.height
            function toPageX(x) { return inner.toPageX(x - inner.x) }
            function toPageY(y) { return inner.toPageY(y) }
            Page {
                id: inner
                page: index
                x: (parent.width - width) / 2 - pageList.panX
                fullWidth: pageList.baseWidth * surface.zoom / surface.crop.width
            }
        }
        onContentYChanged: reportTimer.restart()
        Timer {
            id: reportTimer
            interval: 250
            onTriggered: {
                const pages = surface.visiblePages()
                if (pages.length && pages[0] !== surface.current) {
                    surface.current = pages[0]
                    surface.report()
                }
            }
        }
    }

    Keys.onPressed: (event) => {
        if (event.matches(StandardKey.Copy) && selection && selection.text) {
            reader.copyText(selection.text)
            event.accepted = true
            return
        }
        if (event.modifiers & Qt.ControlModifier) {
            if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) { zoom = Math.min(4, zoom * 1.15); event.accepted = true }
            else if (event.key === Qt.Key_Minus) { zoom = Math.max(1, zoom / 1.15); event.accepted = true }
            else if (event.key === Qt.Key_0) { zoom = 1; event.accepted = true }
        }
    }
}
