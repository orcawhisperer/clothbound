import QtQuick
import QtQuick.Controls
import "../components"

import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui
import "../components/Themes.js" as Themes

// A PDF's reading surface: its pages rendered to the reading theme, one at
// a time, two side by side, or scrolling, with margins cut the same on
// every page. It answers the same calls as the EPUB surface (clothbound.next(),
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
    property string fitMode: "page"
    property string styleFitMode: "page"
    property bool cropping: style.pdfCrop !== undefined ? !!style.pdfCrop : true
    readonly property bool originalColors: !!style.pdfOriginalColors
    readonly property rect crop: cropping ? reader.pdfCrop : Qt.rect(0, 0, 1, 1)
    readonly property real topRoom: style.marginTop || 54
    readonly property real bottomRoom: style.marginBottom || 54
    readonly property real baseFitPageWidth: scrolling
        ? pageList.currentFitPageWidth
        : (spreadRow.activeSlotItem ? spreadRow.activeSlotItem.fitPageWidth : 100)
    readonly property real activeDisplayWidth: scrolling
        ? pageList.currentDisplayWidth
        : (spreadRow.activeSlotItem ? spreadRow.activeSlotItem.fitWidth : 100)
    readonly property real effectiveZoom: fitMode === "custom" ? zoom
        : (baseFitPageWidth > 0 ? Math.max(0.25, Math.min(4.0, activeDisplayWidth / baseFitPageWidth)) : 1.0)
    readonly property bool canPanX: scrolling
        ? pageList.maxPanX > 2
        : (zoomed.contentWidth > zoomed.width + 2)
    readonly property bool canPanY: !scrolling && (zoomed.contentHeight > zoomed.height + 2)
    readonly property bool canPan: canPanX || canPanY
    readonly property bool zoomedIn: canPan || (fitMode === "custom" && zoom > 1.01)

    // What's drawn over the pages.
    property var foundHits: []
    property var foundCurrent: null
    property int foundVersion: 0
    property var speaking: ({ chapter: -1, start: 0, end: 0 })
    property var selection: null   // {chapter, start, end, text}
    property int marksVersion: 0
    property bool snipMode: false
    readonly property bool peekOpen: peekCard.open

    signal contextMenuRequested(real x, real y)
    signal snipActionTriggered(string action, string base64Png, int pageIndex, string customPrompt)

    onSnipModeChanged: {
        if (snipMode) {
            closePeek()
            if (selection) {
                selection = null
                reader.selectionChanged({})
            }
            snipOverlay.resetRect()
        } else {
            snipOverlay.resetRect()
        }
    }

    readonly property string colors: originalColors ? ""
                                     : ("?bg=" + encodeURIComponent(look ? look.bg : "#ffffff")
                                        + "&fg=" + encodeURIComponent(look ? look.fg : "#000000"))

    onZoomChanged: {
        if (!canPanX)
            pageList.panX = 0
        prefetchTimer.restart()
    }
    onFitModeChanged: {
        if (!canPanX)
            pageList.panX = 0
        prefetchTimer.restart()
    }

    function fitPageWidthFor(pageIndex, pageCount, roomW, roomH, cropRect, secondPage) {
        const idx = pageIndex >= 0 ? pageIndex : 0
        const cw = cropRect && cropRect.width > 0 ? cropRect.width : 1
        const ch = cropRect && cropRect.height > 0 ? cropRect.height : 1
        const size0 = reader.pdfPageSize(idx)
        let aspect = ((size0.height || 792) * ch) / ((size0.width || 612) * cw)
        if (secondPage !== undefined && secondPage >= 0 && pageCount > 1) {
            const size1 = reader.pdfPageSize(secondPage)
            const aspect1 = ((size1.height || 792) * ch) / ((size1.width || 612) * cw)
            if (aspect1 > aspect)
                aspect = aspect1
        }
        const count = Math.max(1, pageCount || 1)
        return Math.max(100, Math.min(roomW / count, roomH / aspect))
    }
    function displayWidthFor(pageIndex, pageCount, roomW, roomH, mode, customZoom, cropRect, secondPage) {
        const base = fitPageWidthFor(pageIndex, pageCount, roomW, roomH, cropRect, secondPage)
        const count = Math.max(1, pageCount || 1)
        if (mode === "width")
            return Math.max(100, roomW / count)
        if (mode === "actual") {
            const idx = pageIndex >= 0 ? pageIndex : 0
            const size = reader.pdfPageSize(idx)
            const cw = cropRect && cropRect.width > 0 ? cropRect.width : 1
            return Math.max(100, (size.width || 612) * (96.0 / 72.0) * cw)
        }
        if (mode === "custom")
            return Math.max(100, base * Math.max(0.5, Math.min(4.0, customZoom || 1.0)))
        return base
    }
    function setFitMode(mode) {
        const normalized = (mode === "width" || mode === "actual") ? mode : "page"
        styleFitMode = normalized
        fitMode = normalized
        zoom = 1.0
        pageList.panX = 0
        zoomed.contentX = 0
        zoomed.contentY = 0
        prefetchTimer.restart()
    }
    function setCustomZoom(targetZoom) {
        const clamped = Math.max(0.5, Math.min(4.0, Math.round(targetZoom * 100) / 100))
        fitMode = "custom"
        zoom = clamped
        prefetchTimer.restart()
    }
    function nudgeZoom(factor) {
        const base = effectiveZoom > 0 ? effectiveZoom : 1.0
        setCustomZoom(base * factor)
    }

    // --- The surface's calls --------------------------------------------------

    function open(manifest, position, newStyle) {
        style = newStyle
        const savedFit = (newStyle && (newStyle.pdfFit === "width" || newStyle.pdfFit === "actual"))
            ? newStyle.pdfFit : "page"
        styleFitMode = savedFit
        fitMode = savedFit
        history = []
        forwardHistory = []
        justJumped = false
        current = 0
        zoom = 1.0
        pageList.panX = 0
        zoomed.contentX = 0
        zoomed.contentY = 0
        foundHits = []
        foundCurrent = null
        foundVersion++
        selection = null
        speaking = { chapter: -1, start: 0, end: 0 }
        cancelSnip()
        closePeek()
        spreadRow.resetSlots()
        goTo(Object.assign({}, position || ({ chapter: 0 }), { quiet: true, instant: true }))
        return Promise.resolve(true)
    }
    function setStyle(newStyle) {
        style = newStyle
        const incomingFit = (newStyle && (newStyle.pdfFit === "width" || newStyle.pdfFit === "actual"))
            ? newStyle.pdfFit : "page"
        if (incomingFit !== styleFitMode) {
            styleFitMode = incomingFit
            fitMode = incomingFit
            zoom = 1.0
            pageList.panX = 0
            zoomed.contentX = 0
            zoomed.contentY = 0
        }
        prefetchTimer.restart()
        report()
    }
    function scrollVertical(dir) {
        if (scrolling) {
            const maxY = Math.max(0, pageList.contentHeight - pageList.height)
            const target = Math.max(0, Math.min(maxY, pageList.contentY + dir * pageList.height * 0.72))
            if (Math.abs(target - pageList.contentY) < 1)
                return false
            pageList.contentY = target
            return true
        }
        const maxY = Math.max(0, zoomed.contentHeight - zoomed.height)
        if (maxY <= 4)
            return false
        const stepY = Math.max(120, zoomed.height * 0.72)
        const target = Math.max(0, Math.min(maxY, zoomed.contentY + dir * stepY))
        if (Math.abs(target - zoomed.contentY) < 1)
            return false
        zoomed.contentY = target
        return true
    }
    function pageStep(dir) {
        if (scrolling) {
            if (dir > 0)
                next()
            else
                prev()
            return
        }
        const maxY = Math.max(0, zoomed.contentHeight - zoomed.height)
        if (maxY > 40) {
            if (dir > 0 && zoomed.contentY < maxY - 8) {
                zoomed.contentY = Math.min(maxY, zoomed.contentY + Math.max(120, zoomed.height * 0.82))
                return
            }
            if (dir < 0 && zoomed.contentY > 8) {
                zoomed.contentY = Math.max(0, zoomed.contentY - Math.max(120, zoomed.height * 0.82))
                return
            }
        }
        if (dir > 0)
            next()
        else
            prev()
    }
    function next() {
        if (scrolling) {
            pageList.contentY = Math.min(Math.max(0, pageList.contentHeight - pageList.height), pageList.contentY + pageList.height * 0.9)
            return
        }
        const from = firstOfSpread(current)
        const advance = (spread && from === 0) ? 1 : step
        const target = Math.min(count - 1, from + advance)
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
    function cancelSnip() {
        snipOverlay.resetRect()
        if (snipMode)
            snipMode = false
    }
    function closePeek() {
        peekCard.open = false
    }
    function openPeek(destPage, destY, anchorRect) {
        if (destPage === undefined || destPage < 0)
            return
        const data = reader.peekPdfDestination(destPage, destY !== undefined ? destY : -1.0)
        if (!data || data.page === undefined) {
            goTo({ chapter: destPage, recordHistory: true })
            return
        }
        peekCard.peekInfo = data
        peekCard.anchorRect = anchorRect
        peekCard.open = true
        peekCard.place()
    }
    function dismiss() {
        if (snipMode) {
            cancelSnip()
            return true
        }
        if (peekCard.open) {
            closePeek()
            return true
        }
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
        if (name === "cb-speaking" && (!spans || !spans.length))
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
        closePeek()
        if (snipMode)
            cancelSnip()
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
    // Active Page components currently shown on screen (in both spread and scroll modes).
    function visiblePageItems() {
        if (scrolling) {
            const out = []
            for (let i = 0; i < pageList.count; ++i) {
                const del = pageList.itemAtIndex(i)
                if (del && del.pageItem && del.pageItem.visible && del.pageItem.page >= 0)
                    out.push(del.pageItem)
            }
            return out
        }
        return spreadRow.activePageItems().filter(p => p && p.visible && p.page >= 0)
    }
    // The page under a point of the surface, and where on it (0 to 1).
    function pageAt(x, y) {
        for (const item of visiblePageItems()) {
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
                // The sentence being read aloud: smooth, uniform line-by-line highlight bands.
                Repeater {
                    model: surface.speaking.chapter === pageItem.page
                           ? reader.pdfRects(pageItem.page, surface.speaking.start, surface.speaking.end) : []
                    delegate: Rectangle {
                        id: speakBand
                        required property var modelData
                        property bool entered: false
                        x: modelData.x * whole.width - 3
                        y: modelData.y * whole.height - 1
                        width: modelData.width * whole.width + 6
                        height: modelData.height * whole.height + 2
                        radius: 4
                        color: theme.accent
                        opacity: entered ? (surface.look && surface.look.dark ? 0.26 : 0.22) : 0.0
                        Behavior on opacity {
                            enabled: !theme.reducedMotion
                            NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
                        }
                        Component.onCompleted: entered = true
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
            cursorShape: surface.canPanX
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
                    return qsTr("Peek p. %1 · Ctrl+click to jump").arg(lbl !== "" ? lbl : String(hoveredLink.page + 1))
                }
                return ""
            }
            onExited: hoveredLink = null
            onPressed: (event) => {
                surface.forceActiveFocus()
                if (event.button === Qt.RightButton) {
                    const at = mouse.mapToItem(surface, event.x, event.y)
                    surface.contextMenuRequested(at.x, at.y)
                    event.accepted = true
                    return
                }
                skipRelease = false
                from = Qt.point(pageItem.toPageX(event.x), pageItem.toPageY(event.y))
                dragged = false
                panning = surface.canPanX && !(event.modifiers & Qt.ShiftModifier)
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
                // Clickable PDF link: external URL, or internal citation/figure Quick-Look peek
                // (Ctrl+click or Shift+click jumps immediately with history).
                const link = pageItem.linkAt(x, y)
                if (link) {
                    if (link.url && link.url !== "") {
                        reader.openExternal(link.url)
                        return
                    }
                    if (link.page >= 0) {
                        if ((event.modifiers & (Qt.ControlModifier | Qt.ShiftModifier)) !== 0) {
                            surface.closePeek()
                            surface.goTo({ chapter: link.page, recordHistory: true })
                        } else {
                            const at = whole.mapToItem(surface, link.x * whole.width, link.y * whole.height)
                            const destY = link.destY !== undefined ? link.destY : -1.0
                            surface.openPeek(link.page, destY, {
                                x: at.x,
                                y: at.y,
                                width: Math.max(16, link.width * whole.width),
                                height: Math.max(14, link.height * whole.height)
                            })
                        }
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
        readonly property real fitPageWidth: {
            const first = p0 >= 0 ? p0 : surface.firstOfSpread(surface.current)
            const pageCount = pages.length > 1 ? 2 : 1
            return surface.fitPageWidthFor(first, pageCount, spreadRow.roomWidth, spreadRow.roomHeight, surface.crop, p1)
        }
        readonly property real fitWidth: {
            const first = p0 >= 0 ? p0 : surface.firstOfSpread(surface.current)
            const pageCount = pages.length > 1 ? 2 : 1
            return surface.displayWidthFor(first, pageCount, spreadRow.roomWidth, spreadRow.roomHeight, surface.fitMode, surface.zoom, surface.crop, p1)
        }
        readonly property real contentW: (p0 >= 0 ? leftPageItem.width : 0) + (p1 >= 0 ? rightPageItem.width : 0)
        readonly property real contentH: Math.max(p0 >= 0 ? leftPageItem.height : 0, p1 >= 0 ? rightPageItem.height : 0)
        visible: pages.length > 0
        enabled: !isStandby
        x: Math.max(24, (zoomed.width - contentW) / 2) + slideOffset
        y: Math.max(surface.topRoom, (zoomed.height - contentH) / 2)

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
        // The space for pages, cleanly between the top header and bottom progress bar.
        readonly property real roomHeight: Math.max(120, height - surface.topRoom - surface.bottomRoom)
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
            zoomed.contentX = 0
            zoomed.contentY = 0
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
            contentWidth: Math.max(width, (spreadRow.activeSlotItem ? spreadRow.activeSlotItem.contentW : 0) + 48)
            contentHeight: Math.max(height, (spreadRow.activeSlotItem ? spreadRow.activeSlotItem.contentH : 0) + surface.topRoom + surface.bottomRoom)
            interactive: surface.canPan
            boundsBehavior: Flickable.StopAtBounds
            maximumFlickVelocity: 4500
            flickDeceleration: 2200
            pixelAligned: true
            clip: true
            ScrollBar.vertical: ScrollBar {
                policy: surface.canPanY ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff
            }
            SmoothWheel {
                enabled: surface.canPanY
                stepSize: 135
            }
            onWidthChanged: {
                const maxX = Math.max(0, contentWidth - width)
                if (contentX > maxX)
                    contentX = maxX
            }
            onHeightChanged: {
                const maxY = Math.max(0, contentHeight - height)
                if (contentY > maxY)
                    contentY = maxY
            }
            onContentWidthChanged: {
                const maxX = Math.max(0, contentWidth - width)
                if (contentX > maxX)
                    contentX = maxX
            }
            onContentHeightChanged: {
                const maxY = Math.max(0, contentHeight - height)
                if (contentY > maxY)
                    contentY = maxY
            }

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
                onWheel: (event) => surface.nudgeZoom(event.angleDelta.y > 0 ? 1.1 : 0.9)
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
            enabled: !surface.canPan && !surface.snipMode && !surface.peekOpen
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
        anchors.topMargin: surface.topRoom
        anchors.bottomMargin: surface.bottomRoom
        visible: surface.scrolling
        model: surface.scrolling ? surface.count : 0
        spacing: 8
        cacheBuffer: Math.max(0, height * 2)
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        maximumFlickVelocity: 4500
        flickDeceleration: 2200
        pixelAligned: true
        ScrollBar.vertical: ScrollBar {}
        SmoothWheel { stepSize: 165 }
        property real panX: 0
        readonly property real roomWidth: Math.max(120, width - 48)
        readonly property real roomHeight: Math.max(120, height - 16)
        readonly property real currentFitPageWidth: surface.fitPageWidthFor(surface.current, 1, roomWidth, roomHeight, surface.crop)
        readonly property real currentDisplayWidth: surface.displayWidthFor(surface.current, 1, roomWidth, roomHeight, surface.fitMode, surface.zoom, surface.crop)
        readonly property real maxPanX: Math.max(0, (currentDisplayWidth - roomWidth) / 2)
        onMaxPanXChanged: panX = Math.max(-maxPanX, Math.min(maxPanX, panX))
        WheelHandler {
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            acceptedModifiers: Qt.ControlModifier
            onWheel: (event) => surface.nudgeZoom(event.angleDelta.y > 0 ? 1.1 : 0.9)
        }
        WheelHandler {
            enabled: pageList.maxPanX > 2
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            acceptedModifiers: Qt.ShiftModifier
            onWheel: (event) => {
                const delta = event.angleDelta.x !== 0 ? event.angleDelta.x : event.angleDelta.y
                pageList.panX = Math.max(-pageList.maxPanX, Math.min(pageList.maxPanX, pageList.panX - delta * 0.5))
            }
        }
        WheelHandler {
            enabled: pageList.maxPanX > 2
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            acceptedModifiers: Qt.NoModifier
            orientation: Qt.Horizontal
            onWheel: (event) => {
                const px = event.pixelDelta ? event.pixelDelta.x : 0
                const py = event.pixelDelta ? event.pixelDelta.y : 0
                if (px !== 0 && Math.abs(px) > Math.abs(py)) {
                    pageList.panX = Math.max(-pageList.maxPanX, Math.min(pageList.maxPanX, pageList.panX - px * 1.25))
                } else if (Math.abs(event.angleDelta.x) > Math.abs(event.angleDelta.y)) {
                    pageList.panX = Math.max(-pageList.maxPanX, Math.min(pageList.maxPanX, pageList.panX - event.angleDelta.x * 0.5))
                } else {
                    event.accepted = false
                }
            }
        }
        delegate: Item {
            required property int index
            readonly property int page: index
            readonly property alias pageItem: inner
            width: pageList.width
            height: inner.height
            function toPageX(x) { return inner.toPageX(x - inner.x) }
            function toPageY(y) { return inner.toPageY(y) }
            Page {
                id: inner
                page: index
                x: (parent.width - width) / 2 - pageList.panX
                fullWidth: surface.displayWidthFor(index, 1, pageList.roomWidth, pageList.roomHeight, surface.fitMode, surface.zoom, surface.crop) / surface.crop.width
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

    // --- Zero-Jump Citation & Figure Peek Card (Apple Quick-Look) ----------------
    MouseArea {
        id: peekBackdrop
        anchors.fill: parent
        z: 19
        visible: peekCard.open
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        onPressed: (event) => {
            event.accepted = true
            surface.closePeek()
        }
        onWheel: (wheel) => wheel.accepted = true
    }

    Rectangle {
        id: peekCard
        z: 20
        property bool open: false
        property var peekInfo: ({})
        property var anchorRect: null
        property real reveal: open ? 1 : 0
        readonly property string pageLabelText: peekInfo && peekInfo.pageLabel !== undefined && peekInfo.pageLabel !== ""
                                                ? String(peekInfo.pageLabel)
                                                : (peekInfo && peekInfo.page !== undefined ? String(peekInfo.page + 1) : "")
        readonly property bool hasImage: !!(peekInfo && peekInfo.imageDataUrl && peekInfo.imageDataUrl !== "")

        Behavior on reveal {
            NumberAnimation {
                duration: theme.reducedMotion ? 70 : 160
                easing.type: Easing.OutCubic
            }
        }
        visible: reveal > 0.01
        opacity: reveal
        scale: theme.reducedMotion ? 1.0 : (0.96 + 0.04 * reveal)
        transformOrigin: Item.Top
        width: Math.min(460, Math.max(280, surface.width - 32))
        height: peekCol.implicitHeight
        radius: Ui.radius(14, theme.radius)
        color: surface.look ? surface.look.popover : "#FDFAF3"
        border.width: 1
        border.color: surface.look && surface.look.dark ? Qt.rgba(1, 1, 1, 0.11) : (surface.look ? surface.look.line : "#D8D2C6")

        function place() {
            const margin = 14
            const w = Math.max(200, width)
            const h = Math.max(120, height)
            const maxX = Math.max(margin, surface.width - w - margin)
            const maxY = Math.max(margin, surface.height - h - margin)
            if (!anchorRect) {
                x = Math.max(margin, Math.min(maxX, (surface.width - w) / 2))
                y = Math.max(margin, Math.min(maxY, (surface.height - h) / 2))
                return
            }
            const cx = anchorRect.x + (anchorRect.width || 0) / 2
            x = Math.max(margin, Math.min(maxX, cx - w / 2))
            const below = anchorRect.y + (anchorRect.height || 0) + 10
            const above = anchorRect.y - h - 10
            if (below + h <= surface.height - margin)
                y = Math.max(margin, below)
            else if (above >= margin)
                y = Math.min(maxY, above)
            else
                y = Math.max(margin, Math.min(maxY, below))
        }

        onWidthChanged: if (open) place()
        onHeightChanged: if (open) place()
        Connections {
            target: surface
            function onWidthChanged() {
                if (peekCard.open)
                    peekCard.place()
                if (snipOverlay.actionBarOpen)
                    snipBar.place()
            }
            function onHeightChanged() {
                if (peekCard.open)
                    peekCard.place()
                if (snipOverlay.actionBarOpen)
                    snipBar.place()
            }
        }

        // Soft feathered two-layer drop shadow.
        Rectangle {
            z: -2
            anchors.fill: parent
            anchors.leftMargin: -6
            anchors.rightMargin: -6
            anchors.topMargin: 10
            anchors.bottomMargin: -18
            radius: parent.radius > 0 ? parent.radius + 5 : 0
            color: "black"
            opacity: surface.look && surface.look.dark ? 0.42 : 0.12
        }
        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.topMargin: 4
            anchors.bottomMargin: -6
            radius: parent.radius
            color: "black"
            opacity: surface.look && surface.look.dark ? 0.30 : 0.08
        }

        // Absorb clicks and wheel events on the card itself.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: (wheel) => wheel.accepted = true
        }

        Column {
            id: peekCol
            width: parent.width
            spacing: 0

            // Header: Page N · Reference / Figure preview + close button.
            Item {
                width: parent.width
                height: 40

                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: 14
                    anchors.right: peekCloseBtn.left
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 6

                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Page %1").arg(peekCard.pageLabelText)
                        color: surface.look ? surface.look.fg : "#26221D"
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        font.features: ({ "tnum": 1 })
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: "·"
                        color: surface.look ? surface.look.muted : "#6E665A"
                        font.pixelSize: 12
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Reference / Figure preview")
                        color: surface.look ? surface.look.muted : "#6E665A"
                        font.pixelSize: 12
                        elide: Text.ElideRight
                        width: Math.max(40, parent.width - 96)
                    }
                }

                IconButton {
                    id: peekCloseBtn
                    anchors.right: parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    implicitWidth: 26
                    implicitHeight: 26
                    tabFocusable: false
                    look: surface.look
                    glyph: Icons.close
                    tone: surface.look ? surface.look.muted : "#6E665A"
                    tip: qsTr("Dismiss (Esc)")
                    onClicked: surface.closePeek()
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: surface.look && surface.look.dark ? Qt.rgba(1, 1, 1, 0.08) : (surface.look ? surface.look.line : "#E2DCD0")
            }

            // Body: theme-aware paper frame holding the live-cropped destination PNG.
            Item {
                width: parent.width
                height: peekBodyInner.implicitHeight + 20

                Column {
                    id: peekBodyInner
                    x: 12
                    y: 10
                    width: parent.width - 24
                    spacing: 8

                    Rectangle {
                        visible: peekCard.hasImage
                        width: parent.width
                        readonly property real naturalRatio: peekImage.implicitWidth > 0 && peekImage.implicitHeight > 0
                                                             ? (peekImage.implicitHeight / peekImage.implicitWidth)
                                                             : 0.44
                        height: Math.max(88, Math.min(Math.min(250, Math.max(120, surface.height * 0.36)),
                                                      Math.round(width * naturalRatio)))
                        radius: Ui.radius(8, theme.radius)
                        color: surface.look && surface.look.dark ? "#F2EFE8" : "#FFFFFF"
                        border.width: 1
                        border.color: surface.look ? surface.look.line : "#D8D2C6"
                        clip: true

                        Image {
                            id: peekImage
                            anchors.fill: parent
                            anchors.margins: 1
                            source: peekCard.hasImage ? peekCard.peekInfo.imageDataUrl : ""
                            fillMode: Image.PreserveAspectFit
                            smooth: true
                            mipmap: true
                            cache: false
                            opacity: surface.look && surface.look.dark ? 0.94 : 1.0
                        }
                    }

                    Label {
                        visible: !peekCard.hasImage && !!(peekCard.peekInfo && peekCard.peekInfo.excerpt)
                        width: parent.width
                        text: peekCard.peekInfo && peekCard.peekInfo.excerpt ? String(peekCard.peekInfo.excerpt) : ""
                        textFormat: Text.PlainText
                        color: surface.look ? surface.look.fg : "#26221D"
                        font.pixelSize: 13
                        lineHeight: 1.4
                        wrapMode: Text.Wrap
                        maximumLineCount: 4
                        elide: Text.ElideRight
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: surface.look && surface.look.dark ? Qt.rgba(1, 1, 1, 0.08) : (surface.look ? surface.look.line : "#E2DCD0")
            }

            // Footer: Jump to page <N> -> + Dismiss.
            Item {
                width: parent.width
                height: 44

                TextButton {
                    anchors.left: parent.left
                    anchors.leftMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    size: "compact"
                    ghost: true
                    look: surface.look
                    text: qsTr("Dismiss")
                    onClicked: surface.closePeek()
                }

                TextButton {
                    anchors.right: parent.right
                    anchors.rightMargin: 12
                    anchors.verticalCenter: parent.verticalCenter
                    size: "compact"
                    primary: true
                    look: surface.look
                    text: qsTr("Jump to page %1 →").arg(peekCard.pageLabelText)
                    onClicked: {
                        const targetPage = peekCard.peekInfo && peekCard.peekInfo.page !== undefined ? peekCard.peekInfo.page : -1
                        surface.closePeek()
                        if (targetPage >= 0)
                            surface.goTo({ chapter: targetPage, recordHistory: true })
                    }
                }
            }
        }
    }

    // --- Visual "Snip & Explain" Drag-Region Overlay -----------------------------
    Item {
        id: snipOverlay
        anchors.fill: parent
        z: 25
        visible: surface.snipMode

        property bool dragging: false
        property bool hasRect: false
        property bool actionBarOpen: false
        property real startX: 0
        property real startY: 0
        property rect box: Qt.rect(0, 0, 0, 0)
        property int targetPage: -1
        property rect normBox: Qt.rect(0, 0, 0, 0)
        readonly property color scrimColor: surface.look && surface.look.dark
                                            ? Qt.rgba(6 / 255, 7 / 255, 10 / 255, 0.44)
                                            : Qt.rgba(16 / 255, 15 / 255, 13 / 255, 0.30)
        readonly property color accentColor: surface.look ? surface.look.link : theme.accent

        function resetRect() {
            dragging = false
            hasRect = false
            actionBarOpen = false
            box = Qt.rect(0, 0, 0, 0)
            targetPage = -1
            normBox = Qt.rect(0, 0, 0, 0)
        }

        function triggerAction(action) {
            if (targetPage < 0 || normBox.width <= 0.0005 || normBox.height <= 0.0005) {
                surface.cancelSnip()
                return
            }
            const page = targetPage
            const nx = normBox.x
            const ny = normBox.y
            const nw = normBox.width
            const nh = normBox.height
            const b64 = reader.snipPdfRegionBase64(page, nx, ny, nw, nh)
            surface.cancelSnip()
            if (b64 && b64 !== "")
                surface.snipActionTriggered(action, b64, page, "")
        }

        // Translucent scrim: full-surface before drag, 4-rectangle frosted cutout
        // during/after drag so the snipped region shines at 100% full brightness.
        Rectangle {
            visible: !snipOverlay.hasRect
            anchors.fill: parent
            color: snipOverlay.scrimColor
        }
        Rectangle {
            visible: snipOverlay.hasRect
            x: 0
            y: 0
            width: parent.width
            height: Math.max(0, snipOverlay.box.y)
            color: snipOverlay.scrimColor
        }
        Rectangle {
            visible: snipOverlay.hasRect
            x: 0
            y: snipOverlay.box.y + snipOverlay.box.height
            width: parent.width
            height: Math.max(0, parent.height - (snipOverlay.box.y + snipOverlay.box.height))
            color: snipOverlay.scrimColor
        }
        Rectangle {
            visible: snipOverlay.hasRect
            x: 0
            y: snipOverlay.box.y
            width: Math.max(0, snipOverlay.box.x)
            height: snipOverlay.box.height
            color: snipOverlay.scrimColor
        }
        Rectangle {
            visible: snipOverlay.hasRect
            x: snipOverlay.box.x + snipOverlay.box.width
            y: snipOverlay.box.y
            width: Math.max(0, parent.width - (snipOverlay.box.x + snipOverlay.box.width))
            height: snipOverlay.box.height
            color: snipOverlay.scrimColor
        }

        // Crosshair drag surface covering the reading stage while in snip mode.
        MouseArea {
            id: snipDrag
            anchors.fill: parent
            z: 1
            hoverEnabled: true
            preventStealing: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton
            cursorShape: Qt.CrossCursor
            onWheel: (wheel) => wheel.accepted = true
            onPressed: (event) => {
                surface.forceActiveFocus()
                if (event.button === Qt.RightButton) {
                    surface.cancelSnip()
                    return
                }
                snipOverlay.actionBarOpen = false
                snipOverlay.dragging = true
                snipOverlay.hasRect = false
                snipOverlay.startX = Math.max(0, Math.min(snipOverlay.width, event.x))
                snipOverlay.startY = Math.max(0, Math.min(snipOverlay.height, event.y))
                snipOverlay.box = Qt.rect(snipOverlay.startX, snipOverlay.startY, 0, 0)
            }
            onPositionChanged: (event) => {
                if (!snipOverlay.dragging)
                    return
                const cx = Math.max(0, Math.min(snipOverlay.width, event.x))
                const cy = Math.max(0, Math.min(snipOverlay.height, event.y))
                const rx = Math.round(Math.min(snipOverlay.startX, cx))
                const ry = Math.round(Math.min(snipOverlay.startY, cy))
                const rw = Math.round(Math.abs(cx - snipOverlay.startX))
                const rh = Math.round(Math.abs(cy - snipOverlay.startY))
                snipOverlay.box = Qt.rect(rx, ry, rw, rh)
                snipOverlay.hasRect = rw >= 6 && rh >= 6
            }
            onReleased: (event) => {
                if (event.button === Qt.RightButton || !snipOverlay.dragging)
                    return
                snipOverlay.dragging = false
                if (!snipOverlay.hasRect || snipOverlay.box.width < 14 || snipOverlay.box.height < 14) {
                    snipOverlay.resetRect()
                    return
                }
                // Match the dragged box to the visible PDF page with the largest overlap.
                let best = null
                let bestArea = 36
                for (const p of surface.visiblePageItems()) {
                    const tl = p.mapFromItem(surface, snipOverlay.box.x, snipOverlay.box.y)
                    const br = p.mapFromItem(surface, snipOverlay.box.x + snipOverlay.box.width,
                                             snipOverlay.box.y + snipOverlay.box.height)
                    const ix1 = Math.max(0, Math.min(p.width, Math.min(tl.x, br.x)))
                    const iy1 = Math.max(0, Math.min(p.height, Math.min(tl.y, br.y)))
                    const ix2 = Math.max(0, Math.min(p.width, Math.max(tl.x, br.x)))
                    const iy2 = Math.max(0, Math.min(p.height, Math.max(tl.y, br.y)))
                    const area = (ix2 - ix1) * (iy2 - iy1)
                    if (area > bestArea) {
                        bestArea = area
                        best = { pageItem: p, ix1: ix1, iy1: iy1, ix2: ix2, iy2: iy2 }
                    }
                }
                if (!best) {
                    snipOverlay.resetRect()
                    return
                }
                const p = best.pageItem
                const nx1 = Math.max(0, Math.min(1, p.toPageX(best.ix1)))
                const ny1 = Math.max(0, Math.min(1, p.toPageY(best.iy1)))
                const nx2 = Math.max(nx1, Math.min(1, p.toPageX(best.ix2)))
                const ny2 = Math.max(ny1, Math.min(1, p.toPageY(best.iy2)))
                if (nx2 - nx1 <= 0.002 || ny2 - ny1 <= 0.002) {
                    snipOverlay.resetRect()
                    return
                }
                const stl = p.mapToItem(surface, best.ix1, best.iy1)
                const sbr = p.mapToItem(surface, best.ix2, best.iy2)
                const bx = Math.round(stl.x)
                const by = Math.round(stl.y)
                const bw = Math.max(12, Math.round(sbr.x) - bx)
                const bh = Math.max(12, Math.round(sbr.y) - by)
                snipOverlay.box = Qt.rect(bx, by, bw, bh)
                snipOverlay.targetPage = p.page
                snipOverlay.normBox = Qt.rect(nx1, ny1, nx2 - nx1, ny2 - ny1)
                snipOverlay.actionBarOpen = true
                snipBar.place()
            }
        }

        // Crisp accent frame & L-shaped corner markers around the un-dimmed cutout.
        Item {
            z: 2
            visible: snipOverlay.hasRect
            x: snipOverlay.box.x
            y: snipOverlay.box.y
            width: snipOverlay.box.width
            height: snipOverlay.box.height

            Rectangle {
                anchors.fill: parent
                anchors.margins: -1
                color: "transparent"
                border.width: 1
                border.color: Qt.rgba(1, 1, 1, 0.55)
            }
            Rectangle {
                anchors.fill: parent
                color: "transparent"
                border.width: 1.5
                border.color: snipOverlay.accentColor
            }

            readonly property real arm: Math.min(12, Math.max(6, Math.min(width, height) / 3))
            // Top-left corner marker
            Rectangle { x: -1; y: -1; width: parent.arm; height: 2.5; radius: 1; color: snipOverlay.accentColor }
            Rectangle { x: -1; y: -1; width: 2.5; height: parent.arm; radius: 1; color: snipOverlay.accentColor }
            // Top-right corner marker
            Rectangle { x: parent.width - parent.arm + 1; y: -1; width: parent.arm; height: 2.5; radius: 1; color: snipOverlay.accentColor }
            Rectangle { x: parent.width - 1.5; y: -1; width: 2.5; height: parent.arm; radius: 1; color: snipOverlay.accentColor }
            // Bottom-left corner marker
            Rectangle { x: -1; y: parent.height - 1.5; width: parent.arm; height: 2.5; radius: 1; color: snipOverlay.accentColor }
            Rectangle { x: -1; y: parent.height - parent.arm + 1; width: 2.5; height: parent.arm; radius: 1; color: snipOverlay.accentColor }
            // Bottom-right corner marker
            Rectangle { x: parent.width - parent.arm + 1; y: parent.height - 1.5; width: parent.arm; height: 2.5; radius: 1; color: snipOverlay.accentColor }
            Rectangle { x: parent.width - 1.5; y: parent.height - parent.arm + 1; width: 2.5; height: parent.arm; radius: 1; color: snipOverlay.accentColor }
        }

        // Unobtrusive top hint pill.
        Rectangle {
            id: snipHintPill
            z: 3
            anchors.top: parent.top
            anchors.topMargin: 16
            anchors.horizontalCenter: parent.horizontalCenter
            height: 34
            width: Math.min(parent.width - 32, snipHintRow.implicitWidth + 24)
            radius: Ui.radius(17, theme.radius)
            color: Qt.rgba(26 / 255, 25 / 255, 23 / 255, 0.92)
            border.width: 1
            border.color: Qt.rgba(1, 1, 1, 0.12)
            opacity: (snipBar.visible && snipBar.y < 66) ? 0.0 : 1.0
            visible: opacity > 0.01
            Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

            Row {
                id: snipHintRow
                anchors.centerIn: parent
                spacing: 8
                Glyph {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 14
                    height: 14
                    stroke: 1.9
                    path: "M4 8.5 V5.5 Q4 4 5.5 4 H8.5 M15.5 4 H18.5 Q20 4 20 5.5 V8.5 M20 15.5 V18.5 Q20 20 18.5 20 H15.5 M8.5 20 H5.5 Q4 20 4 18.5 V15.5 M12 9 V15 M9 12 H15"
                    color: "#F2EFE9"
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Drag over an equation, chart, or table · Esc to cancel")
                    color: "#F2EFE9"
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, snipOverlay.width - 78)
                }
            }
        }

        // Sleek floating Snip Action Bar anchored beside the cutout (strictly viewport-clamped).
        Rectangle {
            id: snipBar
            z: 4
            visible: snipOverlay.actionBarOpen && snipOverlay.hasRect
            width: Math.min(snipBarRow.implicitWidth + 14, Math.max(220, snipOverlay.width - 24))
            height: 40
            radius: Ui.radius(12, theme.radius)
            color: surface.look ? surface.look.popover : "#FDFAF3"
            border.width: 1
            border.color: surface.look && surface.look.dark ? Qt.rgba(1, 1, 1, 0.11) : (surface.look ? surface.look.line : "#D8D2C6")
            transformOrigin: Item.Top

            onVisibleChanged: {
                if (visible) {
                    place()
                    if (!theme.reducedMotion) {
                        snipBarAnim.restart()
                    } else {
                        opacity = 1
                        scale = 1
                    }
                }
            }
            onWidthChanged: if (visible) place()

            ParallelAnimation {
                id: snipBarAnim
                NumberAnimation { target: snipBar; property: "opacity"; from: 0.0; to: 1.0; duration: 120; easing.type: Easing.OutCubic }
                NumberAnimation { target: snipBar; property: "scale"; from: 0.96; to: 1.0; duration: 130; easing.type: Easing.OutCubic }
            }

            function place() {
                const margin = 12
                const w = width
                const h = height
                const maxX = Math.max(margin, snipOverlay.width - w - margin)
                const maxY = Math.max(margin, snipOverlay.height - h - margin)
                const cx = snipOverlay.box.x + snipOverlay.box.width / 2
                x = Math.max(margin, Math.min(maxX, cx - w / 2))
                const below = snipOverlay.box.y + snipOverlay.box.height + 10
                const above = snipOverlay.box.y - h - 10
                if (below + h <= snipOverlay.height - margin)
                    y = Math.max(margin, below)
                else if (above >= margin)
                    y = Math.min(maxY, above)
                else
                    y = Math.max(margin, Math.min(maxY, snipOverlay.box.y + snipOverlay.box.height - h - 10))
            }

            // Layered elevation shadow matching SelectionBar.
            Rectangle {
                z: -2
                anchors.fill: parent
                anchors.leftMargin: -4
                anchors.rightMargin: -4
                anchors.topMargin: 8
                anchors.bottomMargin: -14
                radius: parent.radius > 0 ? parent.radius + 4 : 0
                color: "black"
                opacity: surface.look && surface.look.dark ? 0.36 : 0.12
            }
            Rectangle {
                z: -1
                anchors.fill: parent
                anchors.topMargin: 3
                anchors.bottomMargin: -5
                radius: parent.radius
                color: "black"
                opacity: surface.look && surface.look.dark ? 0.28 : 0.08
            }

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                onWheel: (wheel) => wheel.accepted = true
            }

            component SnipAction: AbstractButton {
                id: snipBtn
                property bool accent: false
                property bool muted: false
                property string iconPath: ""
                anchors.verticalCenter: parent.verticalCenter
                implicitHeight: 28
                implicitWidth: snipBtnRow.implicitWidth + 16
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                background: Rectangle {
                    radius: Ui.radius(7, theme.radius)
                    color: snipBtn.accent
                           ? (snipBtn.pressed ? Ui.tint(snipOverlay.accentColor, 0.24)
                              : snipBtn.hovered ? Ui.tint(snipOverlay.accentColor, 0.18)
                              : Ui.tint(snipOverlay.accentColor, 0.12))
                           : (snipBtn.pressed ? Ui.tint(surface.look ? surface.look.fg : "#26221D", 0.14)
                              : snipBtn.hovered ? Ui.tint(surface.look ? surface.look.fg : "#26221D", 0.08)
                              : "transparent")
                    Behavior on color { enabled: !theme.reducedMotion; ColorAnimation { duration: 90 } }
                }
                contentItem: Item {
                    Row {
                        id: snipBtnRow
                        anchors.centerIn: parent
                        spacing: 5
                        Glyph {
                            visible: snipBtn.iconPath !== ""
                            anchors.verticalCenter: parent.verticalCenter
                            width: 13
                            height: 13
                            stroke: 1.9
                            path: snipBtn.iconPath
                            color: snipBtn.accent ? snipOverlay.accentColor : (surface.look ? surface.look.fg : "#26221D")
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: snipBtn.text
                            color: snipBtn.accent ? snipOverlay.accentColor
                                   : snipBtn.muted ? (surface.look ? surface.look.muted : "#6E665A")
                                   : (surface.look ? surface.look.fg : "#26221D")
                            font.pixelSize: 13
                            font.weight: snipBtn.accent ? Font.DemiBold : Font.Medium
                        }
                    }
                }
            }

            Flickable {
                id: snipBarScroll
                anchors.fill: parent
                anchors.leftMargin: 6
                anchors.rightMargin: 6
                clip: true
                interactive: contentWidth > width
                contentWidth: snipBarRow.implicitWidth
                contentHeight: height
                boundsBehavior: Flickable.StopAtBounds

                Row {
                    id: snipBarRow
                    x: Math.max(0, (snipBarScroll.width - width) / 2)
                    y: (40 - height) / 2
                    spacing: 2

                    SnipAction {
                        accent: true
                        iconPath: Icons.sparkle
                        text: qsTr("Explain")
                        onClicked: snipOverlay.triggerAction("explain")
                    }
                    SnipAction {
                        text: qsTr("Copy LaTeX")
                        onClicked: snipOverlay.triggerAction("latex")
                    }
                    SnipAction {
                        text: qsTr("Extract Table")
                        onClicked: snipOverlay.triggerAction("table")
                    }
                    SnipAction {
                        text: qsTr("Ask…")
                        onClicked: snipOverlay.triggerAction("ask")
                    }
                    Item {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 9
                        height: 18
                        Rectangle {
                            anchors.centerIn: parent
                            width: 1
                            height: 18
                            color: surface.look && surface.look.dark ? Qt.rgba(1, 1, 1, 0.10) : (surface.look ? surface.look.line : "#D8D2C6")
                        }
                    }
                    SnipAction {
                        muted: true
                        text: qsTr("Cancel")
                        onClicked: surface.cancelSnip()
                    }
                }
            }
        }
    }

    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Escape && (snipMode || peekCard.open)) {
            dismiss()
            event.accepted = true
            return
        }
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
