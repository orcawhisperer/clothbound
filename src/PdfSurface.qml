import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

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
    readonly property string layout: style.layout || "auto"
    readonly property bool scrolling: layout === "scroll"
    readonly property bool spread: !scrolling && count > 1
                                   && (layout === "double" || (layout === "auto" && width > height * 1.3))
    readonly property int step: spread ? 2 : 1
    property real zoom: 1.0
    property bool cropping: true
    readonly property rect crop: cropping ? reader.pdfCrop : Qt.rect(0, 0, 1, 1)
    readonly property real topRoom: style.marginTop || 54
    readonly property real bottomRoom: style.marginBottom || 54

    // What's drawn over the pages.
    property var foundHits: []
    property var foundCurrent: null
    property var speaking: ({ chapter: -1, start: 0, end: 0 })
    property var selection: null   // {chapter, start, end, text}
    property int marksVersion: 0

    readonly property string colors: "?bg=" + encodeURIComponent(look ? look.bg : "#ffffff")
                                     + "&fg=" + encodeURIComponent(look ? look.fg : "#000000")

    // --- The surface's calls --------------------------------------------------

    function open(manifest, position, newStyle) {
        style = newStyle
        history = []
        current = 0
        goTo(position)
        return true
    }
    function setStyle(newStyle) { style = newStyle; report() }
    function next() {
        if (scrolling) {
            pageList.contentY = Math.min(pageList.contentHeight - pageList.height, pageList.contentY + pageList.height * 0.9)
            return
        }
        show(Math.min(count - 1, firstOfSpread(current) + step))
        reader.tapped("turn")
    }
    function prev() {
        if (scrolling) {
            pageList.contentY = Math.max(0, pageList.contentY - pageList.height * 0.9)
            return
        }
        show(Math.max(0, firstOfSpread(current) - step))
        reader.tapped("turn")
    }
    function nextChapter() {
        for (const entry of reader.contents)
            if (entry.chapter > current + step - 1) {
                goTo({ chapter: entry.chapter })
                return
            }
    }
    function prevChapter() {
        let target = 0
        for (const entry of reader.contents)
            if (entry.chapter < current)
                target = entry.chapter
        goTo({ chapter: target })
    }
    function back() {
        if (!history.length)
            return false
        const page = history[history.length - 1]
        history = history.slice(0, -1)
        show(page)
        return true
    }
    function goTo(target) {
        if (!target)
            return
        const page = Math.max(0, Math.min(count - 1, target.chapter || 0))
        if (Math.abs(page - current) > step && !target.quiet)
            history = history.concat([current])
        show(page)
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
    }
    function setMarks() { marksVersion++ }
    function speak(start, end, follow) {
        speaking = { chapter: narration.chapter, start: start, end: end }
        if (follow && !visiblePages().includes(narration.chapter))
            show(narration.chapter)
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
        while (offset > 0 && !/[.!?]\s/.test(text.substr(offset - 2, 2)))
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
    function show(page) {
        current = Math.max(0, Math.min(count - 1, page))
        if (scrolling)
            pageList.positionViewAtIndex(current, ListView.Beginning)
        selection = null
        report()
    }
    function report() {
        if (!reader.open)
            return
        const pages = visiblePages()
        const page = pages[0]
        const length = reader.charactersIn(page)
        const label = reader.pdfPageLabel(page)
        reader.relocated({ chapter: page, offset: 0, end: length, page: 0, pages: 1,
                           pageLabel: label !== "" ? label : String(page + 1),
                           atEnd: pages[pages.length - 1] >= count - 1,
                           snippet: reader.pdfPageText(page).replace(/\s+/g, " ").slice(0, 120) })
    }
    // The page under a point of the surface, and where on it (0 to 1).
    function pageAt(x, y) {
        const holder = scrolling ? pageList : spreadRow
        for (const item of holder === pageList ? Array.from({ length: pageList.count }, (_, i) => pageList.itemAtIndex(i)) : spreadPages.children) {
            if (!item || item.page === undefined || item.page < 0)
                continue
            const at = item.mapFromItem(surface, x, y)
            if (at.x >= 0 && at.y >= 0 && at.x <= item.width && at.y <= item.height)
                return { page: item.page, x: item.toPageX(at.x), y: item.toPageY(at.y), item: item }
        }
        return null
    }

    onLayoutChanged: Qt.callLater(show, current)
    onSpreadChanged: Qt.callLater(show, current)
    Connections {
        target: marks
        function onChanged() { surface.marksVersion++ }
    }

    // One page: rendered, cropped, and drawn over (highlights, search,
    // the sentence being read, the selection).
    component Page: Item {
        id: pageItem
        property int page: -1
        // The size shown, before cropping.
        property real fullWidth: 100
        readonly property var points: page >= 0 ? reader.pdfPageSize(page) : ({ width: 612, height: 792 })
        readonly property real fullHeight: fullWidth * (points.height || 792) / (points.width || 612)
        width: fullWidth * surface.crop.width
        height: fullHeight * surface.crop.height
        clip: true

        function toPageX(x) { return (x + surface.crop.x * fullWidth) / fullWidth }
        function toPageY(y) { return (y + surface.crop.y * fullHeight) / fullHeight }

        Rectangle { anchors.fill: parent; color: surface.look ? surface.look.bg : "white" }
        Item {
            id: whole
            x: -surface.crop.x * pageItem.fullWidth
            y: -surface.crop.y * pageItem.fullHeight
            width: pageItem.fullWidth
            height: pageItem.fullHeight
            Image {
                anchors.fill: parent
                asynchronous: true
                cache: false
                retainWhileLoading: true
                source: pageItem.page >= 0 && reader.pdfSource !== "" ? reader.pdfSource + "/" + pageItem.page + surface.colors : ""
                sourceSize.width: Math.round(pageItem.fullWidth * Screen.devicePixelRatio)
                fillMode: Image.Stretch
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
                            out.push({ rect: rect, color: mark.color, id: mark.id })
                    return out
                }
                delegate: Rectangle {
                    required property var modelData
                    x: modelData.rect.x * whole.width
                    y: modelData.rect.y * whole.height
                    width: modelData.rect.width * whole.width
                    height: modelData.rect.height * whole.height
                    color: ({ yellow: "#f2c94c", green: "#6fcf97", blue: "#56ccf2", pink: "#f78fb3" })[modelData.color] || "#f2c94c"
                    opacity: 0.38
                }
            }
            // Search results.
            Repeater {
                model: {
                    const out = []
                    for (const hit of surface.foundHits)
                        if (hit.chapter === pageItem.page)
                            for (const rect of reader.pdfRects(pageItem.page, hit.start, hit.end))
                                out.push({ rect: rect, current: !!surface.foundCurrent && surface.foundCurrent.start === hit.start
                                                                 && surface.foundCurrent.chapter === hit.chapter })
                    return out
                }
                delegate: Rectangle {
                    required property var modelData
                    x: modelData.rect.x * whole.width - 1
                    y: modelData.rect.y * whole.height - 1
                    width: modelData.rect.width * whole.width + 2
                    height: modelData.rect.height * whole.height + 2
                    radius: 2
                    color: modelData.current ? "#ff9f43" : "#ffd166"
                    opacity: modelData.current ? 0.55 : 0.35
                }
            }
            // The sentence being read aloud.
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
                    color: surface.look ? surface.look.link : "#4a90d9"
                    opacity: 0.22
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
        }

        // Selecting with the mouse; a click on a highlight opens it, a click
        // in the margins turns the page.
        MouseArea {
            id: mouse
            anchors.fill: parent
            acceptedButtons: Qt.LeftButton
            cursorShape: Qt.IBeamCursor
            preventStealing: true
            property point from
            property bool dragged: false
            onPressed: (event) => {
                from = Qt.point(pageItem.toPageX(event.x), pageItem.toPageY(event.y))
                dragged = false
                surface.selection = null
            }
            onPositionChanged: (event) => {
                const to = Qt.point(pageItem.toPageX(event.x), pageItem.toPageY(event.y))
                if (Math.abs(to.x - from.x) + Math.abs(to.y - from.y) < 0.004)
                    return
                dragged = true
                const chosen = reader.pdfSelect(pageItem.page, from.x, from.y, to.x, to.y)
                surface.selection = chosen.text ? chosen : null
            }
            onReleased: (event) => {
                if (dragged && surface.selection) {
                    const rects = reader.pdfRects(pageItem.page, surface.selection.start, surface.selection.end)
                    let top = 1, left = 1, right = 0, bottom = 0
                    for (const r of rects) {
                        top = Math.min(top, r.y); left = Math.min(left, r.x)
                        right = Math.max(right, r.x + r.width); bottom = Math.max(bottom, r.y + r.height)
                    }
                    const at = whole.mapToItem(surface, left * whole.width, top * whole.height)
                    reader.selectionChanged(Object.assign({}, surface.selection, {
                        x: at.x, y: at.y, width: (right - left) * whole.width, height: (bottom - top) * whole.height }))
                    return
                }
                reader.selectionChanged({})
                // A highlight under the click opens it.
                const x = pageItem.toPageX(event.x), y = pageItem.toPageY(event.y)
                for (const mark of marks.highlightsIn(pageItem.page))
                    for (const r of reader.pdfRects(pageItem.page, mark.start, mark.end))
                        if (x >= r.x && x <= r.x + r.width && y >= r.y && y <= r.y + r.height) {
                            const at = whole.mapToItem(surface, r.x * whole.width, r.y * whole.height)
                            reader.markClicked(mark.id, { x: at.x, y: at.y, width: r.width * whole.width, height: r.height * whole.height })
                            return
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

    // --- One page or two --------------------------------------------------------
    Item {
        id: spreadRow
        anchors.fill: parent
        visible: !surface.scrolling
        // The space for pages, under the title and above the listen bar.
        readonly property real roomHeight: height - surface.topRoom - surface.bottomRoom + 20
        readonly property real roomWidth: width - 48
        Flickable {
            id: zoomed
            anchors.fill: parent
            contentWidth: Math.max(width, spreadPages.width + 48)
            contentHeight: Math.max(height, spreadPages.height + surface.topRoom + surface.bottomRoom)
            interactive: surface.zoom > 1.01
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            Row {
                id: spreadPages
                spacing: surface.spread ? 2 : 0
                x: Math.max(24, (zoomed.width - width) / 2)
                y: Math.max(surface.topRoom - 10, (zoomed.height - height) / 2)
                readonly property var pages: surface.visiblePages()
                readonly property real fitWidth: {
                    const first = surface.firstOfSpread(surface.current)
                    const size = reader.pdfPageSize(first)
                    const aspect = ((size.height || 792) * surface.crop.height) / ((size.width || 612) * surface.crop.width)
                    const each = Math.min(spreadRow.roomWidth / pages.length - spacing, spreadRow.roomHeight / aspect)
                    return Math.max(100, each) * surface.zoom
                }
                Repeater {
                    model: spreadPages.pages
                    delegate: Page {
                        required property int modelData
                        page: modelData
                        fullWidth: spreadPages.fitWidth / surface.crop.width
                    }
                }
            }
            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                acceptedModifiers: Qt.ControlModifier
                onWheel: (event) => surface.zoom = Math.max(1, Math.min(4, surface.zoom * (event.angleDelta.y > 0 ? 1.1 : 0.9)))
            }
        }
        // The wheel turns pages, as on the EPUB page (unless zoomed in).
        WheelHandler {
            enabled: surface.zoom <= 1.01
            acceptedModifiers: Qt.NoModifier
            property real gathered: 0
            onWheel: (event) => {
                gathered += event.angleDelta.y + event.angleDelta.x
                if (gathered <= -120) { gathered = 0; surface.next() }
                else if (gathered >= 120) { gathered = 0; surface.prev() }
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
                x: (parent.width - width) / 2
                fullWidth: Math.min(pageList.width - 48, 920) * surface.zoom / surface.crop.width
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
        if (event.modifiers & Qt.ControlModifier) {
            if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) { zoom = Math.min(4, zoom * 1.15); event.accepted = true }
            else if (event.key === Qt.Key_Minus) { zoom = Math.max(1, zoom / 1.15); event.accepted = true }
            else if (event.key === Qt.Key_0) { zoom = 1; event.accepted = true }
        }
    }
}
