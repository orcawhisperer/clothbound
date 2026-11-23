// Dogear's reading surface. It shows one chapter at a time in a sandboxed
// frame and lays it out in pages with CSS columns: one or two per screen,
// turned by moving the frame's scroll position a screen at a time.
//
// Positions are character offsets into the chapter's text, counted the way
// Dogear's C++ counts them (EpubBook::extractText): every text node of the
// body in document order, outside script and style, without soft hyphens.
// Saved places, search hits, highlights, and narration all use them.
//
// The window drives it through `window.dogear`; it reports back to `bridge`
// (the Reader object) over the web channel.
'use strict'

const dogear = (() => {
    const SHY = '­'
    const OPS = 'http://www.idpf.org/2007/ops'
    const XLINK = 'http://www.w3.org/1999/xlink'

    const state = {
        bridge: null,
        book: null,
        style: null,
        chapter: -1,
        page: 0,
        pages: 1,
        geometry: null,
        index: null,
        labels: [],
        location: null,
        history: [],
        animation: null,
        queue: Promise.resolve(),
        hyphenation: false,
        note: null,
    }

    const stage = () => document.getElementById('stage')
    const frame = () => document.getElementById('book')
    const overlay = () => document.getElementById('overlay')
    const chapterDocument = () => frame().contentDocument
    const clamp = (value, low, high) => Math.max(low, Math.min(high, value))

    function log(message) {
        if (state.bridge)
            state.bridge.log(String(message))
    }

    // Navigation runs one step at a time, so a burst of key presses never
    // loads two chapters over each other.
    function run(step) {
        state.queue = state.queue.then(step).catch(error => log(error && error.stack || error))
        return state.queue
    }

    // --- Geometry --------------------------------------------------------

    // The frame holds exactly one screen: one column (or two side by side)
    // with half a gap on each side, so moving by the frame's width shows the
    // next screen with the same margins. A wide window keeps lines at a
    // comfortable measure and centers the frame.
    function computeGeometry() {
        const s = state.style
        const W = stage().clientWidth, H = stage().clientHeight
        const em = 16 * s.fontScale
        const measure = s.measure * em
        const side = Math.max(20, Math.round(W * s.margin))
        if (s.layout === 'scroll') {
            const column = Math.floor(Math.min(measure, W - 2 * side))
            const width = column + 2 * side
            return { scroll: true, width, height: H, top: 0, left: Math.round((W - width) / 2), columns: 1,
                     gap: 2 * side, column, step: Math.max(100, H - s.marginTop - s.marginBottom - 2 * em) }
        }
        const height = Math.max(160, H - s.marginTop - s.marginBottom)
        const twoFit = (W - 4 * side) / 2 >= 22 * em
        const columns = s.layout === 'double' || (s.layout === 'auto' && twoFit && W > H * 1.15) ? 2 : 1
        const gap = columns === 2 ? clamp(2 * side, 56, 160) : 2 * side
        const column = Math.floor(Math.min(measure, columns === 2 ? (W - 2 * gap) / 2 : W - gap))
        const width = columns * (column + gap)
        return { scroll: false, width, height, top: s.marginTop, left: Math.round((W - width) / 2),
                 columns, gap, column, step: width }
    }

    function layoutCss(g) {
        const s = state.style
        if (g.scroll)
            return `html { height: auto !important; columns: auto !important; overflow-x: hidden !important;
                           overflow-y: auto !important; box-sizing: border-box !important; margin: 0 !important;
                           padding: ${s.marginTop}px ${g.gap / 2}px ${s.marginBottom}px !important; }
                    html::-webkit-scrollbar { display: none; }
                    img, svg, video { max-height: ${Math.round(g.height * 0.9)}px !important; }`
        return `html { height: ${g.height}px !important; width: ${g.width}px !important;
                       box-sizing: border-box !important; margin: 0 !important; padding: 0 ${g.gap / 2}px !important;
                       column-count: ${g.columns} !important; column-width: auto !important;
                       column-gap: ${g.gap}px !important; column-fill: auto !important; overflow: hidden !important; }
                img, svg, video, object, embed { max-height: ${g.height - 6}px !important; }`
    }

    // --- Styles ----------------------------------------------------------

    function typographyCss() {
        const s = state.style, t = s.theme
        const own = !s.publisher
        const family = own && s.font && s.font !== 'publisher' ? `"${s.font}", "Literata", serif` : ''
        return `
@font-face { font-family: "Literata"; src: url("dogear://local/app/fonts/Literata.ttf"); font-weight: 200 900; font-style: normal; font-display: block; }
@font-face { font-family: "Literata"; src: url("dogear://local/app/fonts/Literata-Italic.ttf"); font-weight: 200 900; font-style: italic; font-display: block; }
html { background: ${t.bg} !important; color: ${t.fg} !important; font-size: ${s.fontScale * 100}% !important;
       -webkit-text-size-adjust: none; text-rendering: optimizeLegibility; font-kerning: normal;
       font-variant-ligatures: common-ligatures; font-optical-sizing: auto; }
body { margin: 0 !important; padding: 0 !important; width: auto !important; max-width: none !important;
       min-height: 0 !important; height: auto !important; background: transparent !important; }
body *, body *::before, body *::after { color: inherit !important; background-color: transparent !important; }
a, a * { color: ${t.link} !important; }
a { text-decoration: none !important; }
::selection { background: ${t.selection}; }
::highlight(dg-search) { background-color: ${t.search}; }
::highlight(dg-speaking) { background-color: ${t.speaking}; }
img, svg, video { max-width: 100% !important; object-fit: contain; break-inside: avoid; }
img { height: auto; }
${t.dark ? 'img { filter: brightness(0.88) contrast(1.04); }' : ''}
h1, h2, h3, h4, h5, h6 { break-after: avoid; break-inside: avoid; }
p { widows: 2; orphans: 2; }
figure, table, pre { break-inside: avoid; }
pre, code, kbd, samp, tt { font-family: ui-monospace, "JetBrainsMono Nerd Font", monospace !important; }
pre { white-space: pre-wrap !important; }
${family ? `body, body :not(pre):not(code):not(kbd):not(samp):not(tt) { font-family: ${family} !important; }` : ''}
${own ? `p, li, dd, dt, blockquote, div, td { line-height: ${s.lineHeight} !important; }` : ''}
[data-dg-flow] { text-align: ${s.justify ? 'justify' : 'left'} !important;
                 ${s.justify && state.hyphenation ? 'hyphens: manual; -webkit-hyphens: manual;' : 'hyphens: none;'} }
`
    }

    function styleElement(doc, id) {
        let element = doc.getElementById(id)
        if (!element) {
            element = doc.createElementNS('http://www.w3.org/1999/xhtml', 'style')
            element.id = id
            const head = doc.head || doc.querySelector('head') || doc.documentElement
            head.appendChild(element)
        }
        return element
    }

    function applyTypography(doc) {
        styleElement(doc, 'dg-typography').textContent = typographyCss()
    }

    function applyLayout() {
        const doc = chapterDocument()
        const g = state.geometry = computeGeometry()
        const f = frame()
        f.style.left = g.left + 'px'
        f.style.top = g.top + 'px'
        f.style.width = g.width + 'px'
        f.style.height = g.height + 'px'
        styleElement(doc, 'dg-layout').textContent = layoutCss(g)
        // Cover pages often size their SVG at 100% of a height that columns
        // don't have; give them the page's.
        for (const svg of doc.querySelectorAll('svg')) {
            const height = svg.getAttribute('height')
            if (height && height.trim().endsWith('%')) {
                svg.style.setProperty('height', g.height - 6 + 'px', 'important')
                svg.style.setProperty('width', '100%', 'important')
            }
        }
    }

    // --- Preparing a chapter -----------------------------------------------

    // Running text is marked so the reader's alignment applies to it and
    // not to centered headings, verse, or right-aligned signatures.
    function markFlow(doc) {
        const view = doc.defaultView
        for (const element of doc.querySelectorAll('p, li, dd, blockquote, div')) {
            let hasText = false
            for (const child of element.childNodes)
                if (child.nodeType === Node.TEXT_NODE && child.data.trim()) {
                    hasText = true
                    break
                }
            if (!hasText)
                continue
            const align = view.getComputedStyle(element).textAlign
            if (align === 'start' || align === 'left' || align === 'justify' || align === '-webkit-auto')
                element.setAttribute('data-dg-flow', '')
        }
    }

    function hyphenate(doc) {
        const walker = doc.createTreeWalker(doc.body || doc.documentElement, NodeFilter.SHOW_TEXT)
        for (let node = walker.nextNode(); node; node = walker.nextNode()) {
            const parent = node.parentElement
            if (parent && parent.closest('[data-dg-flow]') && !parent.closest('a, code, pre, kbd, samp, tt'))
                Hyphenation.node(node)
        }
    }

    async function settle(doc) {
        try { await doc.fonts.ready } catch (e) { }
        const images = [...doc.images].filter(image => !image.complete)
        if (images.length)
            await Promise.race([
                Promise.all(images.map(image => image.decode().catch(() => { }))),
                new Promise(resolve => setTimeout(resolve, 1500)),
            ])
    }

    async function prepare(doc) {
        applyTypography(doc)
        markFlow(doc)
        const s = state.style
        state.hyphenation = s.justify && s.hyphenate && await Hyphenation.load(state.book.language)
        if (state.hyphenation) {
            hyphenate(doc)
            applyTypography(doc)
        }
        doc.addEventListener('click', onChapterClick, true)
        doc.addEventListener('wheel', onWheel, { passive: false })
        doc.addEventListener('keydown', onKey, true)
        doc.addEventListener('dragstart', event => event.preventDefault())
        doc.addEventListener('selectionchange', onSelection)
    }

    // --- The text model ------------------------------------------------------

    function buildIndex(doc) {
        const root = doc.body || doc.documentElement
        const skip = new Set(['script', 'style', 'template'])
        if (!doc.body)
            skip.add('head')
        const walker = doc.createTreeWalker(root, NodeFilter.SHOW_TEXT | NodeFilter.SHOW_CDATA_SECTION, {
            acceptNode(node) {
                for (let element = node.parentElement; element && element !== root; element = element.parentElement)
                    if (skip.has(element.localName))
                        return NodeFilter.FILTER_REJECT
                return NodeFilter.FILTER_ACCEPT
            },
        })
        const nodes = [], parts = [], byNode = new Map()
        let length = 0
        for (let node = walker.nextNode(); node; node = walker.nextNode()) {
            const shy = node.data.includes(SHY)
            const text = shy ? node.data.split(SHY).join('') : node.data
            const entry = { node, start: length, length: text.length, shy }
            nodes.push(entry)
            byNode.set(node, entry)
            parts.push(text)
            length += text.length
        }
        return { nodes, length, text: parts.join(''), byNode }
    }

    function dataIndex(entry, modelIndex) {
        if (!entry.shy)
            return Math.min(modelIndex, entry.node.data.length)
        const data = entry.node.data
        let model = 0
        for (let i = 0; i < data.length; ++i) {
            if (model === modelIndex)
                return i
            if (data[i] !== SHY)
                ++model
        }
        return data.length
    }

    function offsetOf(node, index) {
        const entry = state.index && state.index.byNode.get(node)
        if (!entry)
            return null
        if (!entry.shy)
            return entry.start + Math.min(index, entry.length)
        let model = 0
        for (let i = 0; i < index && i < node.data.length; ++i)
            if (node.data[i] !== SHY)
                ++model
        return entry.start + model
    }

    // The text node and index for an offset, preferring a node that has the
    // character over one that merely ends there.
    function locate(offset) {
        const nodes = state.index.nodes
        if (!nodes.length)
            return null
        offset = clamp(offset, 0, state.index.length)
        let low = 0, high = nodes.length - 1
        while (low < high) {
            const middle = (low + high + 1) >> 1
            if (nodes[middle].start <= offset)
                low = middle
            else
                high = middle - 1
        }
        let at = low
        while (at + 1 < nodes.length && offset >= nodes[at].start + nodes[at].length)
            ++at
        const entry = nodes[at]
        return { node: entry.node, index: dataIndex(entry, offset - entry.start) }
    }

    function rangeFor(start, end) {
        const from = locate(start), to = locate(Math.max(start, end))
        if (!from || !to)
            return null
        const range = chapterDocument().createRange()
        range.setStart(from.node, from.index)
        range.setEnd(to.node, to.index)
        return range
    }

    // A rectangle for the character at an offset: the first visible one at
    // or after it, so an offset in collapsed whitespace still has a place.
    function rectAt(offset) {
        const doc = chapterDocument()
        const range = doc.createRange()
        for (let probe = offset, tries = 0; probe < state.index.length && tries < 400; ++probe, ++tries) {
            const at = locate(probe)
            if (!at)
                break
            const end = Math.min(at.node.data.length, at.index + 1)
            range.setStart(at.node, at.index)
            range.setEnd(at.node, end)
            const rects = range.getClientRects()
            if (rects.length && (rects[0].width > 0 || rects[0].height > 0))
                return rects[0]
        }
        const at = locate(offset)
        const element = at && at.node.parentElement
        return element ? element.getBoundingClientRect() : null
    }

    function scroller() {
        const doc = chapterDocument()
        return doc.scrollingElement || doc.documentElement
    }

    // The page a document position falls on.
    function pageAtPosition(x, y) {
        const g = state.geometry
        if (g.scroll)
            return clamp(Math.floor((y + 1) / g.step), 0, state.pages - 1)
        return clamp(Math.floor((x + 1) / g.step), 0, state.pages - 1)
    }

    function pageOfRect(rect) {
        if (!rect)
            return 0
        const el = scroller()
        return pageAtPosition(rect.left + el.scrollLeft, rect.top + el.scrollTop)
    }

    function pageOfOffset(offset) { return pageOfRect(rectAt(offset)) }

    function elementFor(fragment) {
        if (!fragment)
            return null
        const doc = chapterDocument()
        return doc.getElementById(fragment) || doc.querySelector(`[name="${CSS.escape(fragment)}"]`)
    }

    function pageOfElement(element) {
        if (!element)
            return 0
        const rects = element.getClientRects()
        return pageOfRect(rects.length ? rects[0] : element.getBoundingClientRect())
    }

    // The first character shown on the current screen.
    function firstVisibleOffset() {
        const nodes = state.index.nodes
        if (!nodes.length)
            return 0
        const g = state.geometry
        const range = chapterDocument().createRange()
        const past = rect => g.scroll ? rect.bottom > 1 : rect.right > 1
        const nodeIsPast = entry => {
            range.selectNodeContents(entry.node)
            const rects = range.getClientRects()
            if (rects.length)
                return past(rects[rects.length - 1])
            return null
        }
        let low = 0, high = nodes.length - 1, found = nodes.length - 1
        while (low <= high) {
            let middle = (low + high) >> 1
            let verdict = nodeIsPast(nodes[middle])
            // Nodes with no boxes (collapsed whitespace) take a neighbor's.
            let probe = middle
            while (verdict === null && probe < high)
                verdict = nodeIsPast(nodes[++probe])
            if (verdict === null)
                verdict = false
            if (verdict) {
                found = middle
                high = middle - 1
            } else {
                low = probe + 1
            }
        }
        for (let at = found; at < nodes.length && at < found + 200; ++at) {
            const entry = nodes[at], data = entry.node.data
            if (!data.trim())
                continue
            let lowChar = 0, highChar = data.length - 1, first = -1
            while (lowChar <= highChar) {
                const middle = (lowChar + highChar) >> 1
                range.setStart(entry.node, middle)
                range.setEnd(entry.node, middle + 1)
                const rects = range.getClientRects()
                const rect = rects.length ? rects[rects.length - 1] : null
                if (rect && past(rect)) {
                    first = middle
                    highChar = middle - 1
                } else {
                    lowChar = middle + 1
                }
            }
            if (first >= 0) {
                while (first < data.length && /\s/.test(data[first]))
                    ++first
                if (first < data.length)
                    return offsetOf(entry.node, first)
            }
        }
        return offsetOf(nodes[found].node, 0) || 0
    }

    // A saved offset, checked against the text it pointed at, so a place
    // survives small differences in how the text is counted.
    function resolveOffset(offset, snippet) {
        const text = state.index.text
        offset = clamp(offset || 0, 0, text.length)
        if (!snippet || text.startsWith(snippet, offset))
            return offset
        const reach = 4000
        const from = Math.max(0, offset - reach)
        const near = text.slice(from, offset + reach + snippet.length).indexOf(snippet)
        if (near >= 0)
            return from + near
        const anywhere = text.indexOf(snippet)
        return anywhere >= 0 ? anywhere : offset
    }

    // --- Pages ---------------------------------------------------------------

    function measure() {
        const g = state.geometry
        const el = scroller()
        if (g.scroll) {
            state.pages = Math.max(1, Math.ceil((el.scrollHeight - 2) / g.step))
        } else {
            const end = Math.max(el.scrollWidth, ...lastBoxes())
            state.pages = Math.max(1, Math.ceil((end - 2) / g.step))
        }
        // Printed page numbers in this chapter, by where they fall.
        state.labels = []
        const targets = (state.book.pageTargets || []).filter(target => target.chapter === state.chapter)
        for (const target of targets) {
            const element = elementFor(target.fragment)
            if (!element)
                continue
            const rect = element.getBoundingClientRect()
            state.labels.push({ label: target.label, at: g.scroll ? rect.top + el.scrollTop : rect.left + el.scrollLeft })
        }
        state.labels.sort((a, b) => a.at - b.at)
    }

    // Where the last content box ends, for books whose final column is not
    // counted in the scroll width.
    function lastBoxes() {
        const doc = chapterDocument(), el = scroller()
        const body = doc.body || doc.documentElement
        const last = body.lastElementChild
        if (!last)
            return [0]
        const rects = last.getClientRects()
        const rect = rects.length ? rects[rects.length - 1] : last.getBoundingClientRect()
        return [rect.right + el.scrollLeft]
    }

    function labelForPage(page) {
        const g = state.geometry
        const start = page * g.step + 2
        let label = ''
        for (const entry of state.labels)
            if (entry.at <= start)
                label = entry.label
        if (label)
            return label
        // Before this chapter's first marker, the page that began earlier.
        const earlier = (state.book.pageTargets || []).filter(target => target.chapter < state.chapter)
        return earlier.length ? earlier[earlier.length - 1].label : ''
    }

    function cancelAnimation() {
        if (state.animation) {
            cancelAnimationFrame(state.animation.frame)
            const el = scroller()
            if (state.geometry.scroll)
                el.scrollTop = state.animation.target
            else
                el.scrollLeft = state.animation.target
            state.animation.resolve()
            state.animation = null
        }
    }

    function turnTo(page, animate) {
        cancelAnimation()
        const g = state.geometry
        state.page = clamp(page, 0, state.pages - 1)
        const el = scroller()
        const target = state.page * g.step
        const reduced = matchMedia('(prefers-reduced-motion: reduce)').matches
        if (!animate || !state.style.animate || reduced) {
            if (g.scroll)
                el.scrollTop = target
            else
                el.scrollLeft = target
            return Promise.resolve()
        }
        const from = g.scroll ? el.scrollTop : el.scrollLeft
        const distance = target - from
        const duration = g.scroll ? 260 : 230
        return new Promise(resolve => {
            const started = performance.now()
            const step = now => {
                const t = Math.min(1, (now - started) / duration)
                const eased = 1 - Math.pow(1 - t, 3)
                if (g.scroll)
                    el.scrollTop = from + distance * eased
                else
                    el.scrollLeft = from + distance * eased
                if (t < 1) {
                    state.animation.frame = requestAnimationFrame(step)
                } else {
                    state.animation = null
                    resolve()
                }
            }
            state.animation = { frame: requestAnimationFrame(step), target, resolve }
        })
    }

    function linearAfter(chapter) {
        const spine = state.book.spine
        const fromLinear = spine[chapter] && spine[chapter].linear
        for (let i = chapter + 1; i < spine.length; ++i)
            if (!fromLinear || spine[i].linear)
                return i
        return -1
    }

    function linearBefore(chapter) {
        const spine = state.book.spine
        const fromLinear = spine[chapter] && spine[chapter].linear
        for (let i = chapter - 1; i >= 0; --i)
            if (!fromLinear || spine[i].linear)
                return i
        return -1
    }

    function report() {
        if (!state.index)
            return
        const offset = firstVisibleOffset()
        state.location = {
            chapter: state.chapter,
            offset,
            snippet: state.index.text.substr(offset, 48),
            page: state.page,
            pages: state.pages,
            pageLabel: labelForPage(state.page),
            atEnd: state.page >= state.pages - 1 && linearAfter(state.chapter) < 0,
        }
        if (state.bridge)
            state.bridge.relocated(state.location)
    }

    async function load(chapter) {
        const f = frame()
        f.classList.remove('shown')
        closeNote()
        const url = state.book.base + state.book.spine[chapter].href
        await new Promise(resolve => {
            f.onload = () => resolve()
            f.src = url
        })
        const doc = chapterDocument()
        state.chapter = chapter
        state.index = null
        await prepare(doc)
        state.geometry = computeGeometry()
        applyLayout()
        await settle(doc)
        state.index = buildIndex(doc)
        measure()
    }

    // Shows a place: {chapter, offset/snippet, fragment, pageLabel, end, fraction}.
    async function show(place, animate) {
        if (place.chapter !== state.chapter || !state.index)
            await load(place.chapter)
        let page = 0
        if (place.end) {
            page = state.pages - 1
        } else if (place.pageLabel) {
            const target = (state.book.pageTargets || []).find(t => t.chapter === place.chapter && t.label === place.pageLabel)
            page = target ? pageOfElement(elementFor(target.fragment)) : 0
        } else if (place.fragment) {
            page = pageOfElement(elementFor(place.fragment))
        } else if (place.offset !== undefined && place.offset !== null) {
            page = pageOfOffset(resolveOffset(place.offset, place.snippet))
        } else if (place.fraction !== undefined) {
            page = Math.floor(place.fraction * state.pages)
        }
        await turnTo(page, animate)
        frame().classList.add('shown')
        report()
    }

    function remember() {
        if (state.location)
            state.history.push({ chapter: state.location.chapter, offset: state.location.offset,
                                 snippet: state.location.snippet })
        if (state.history.length > 50)
            state.history.shift()
    }

    // --- Links and notes -------------------------------------------------------

    function hrefOf(anchor) {
        return anchor.getAttribute('href') || anchor.getAttributeNS(XLINK, 'href') || ''
    }

    function spineIndexOf(url) {
        const base = new URL(state.book.base)
        if (url.origin !== base.origin || !url.pathname.startsWith(base.pathname))
            return -1
        const path = decodeURIComponent(url.pathname.slice(base.pathname.length))
        return state.book.spine.findIndex(item => decodeURIComponent(item.href) === path)
    }

    // Note references are marked as such in good books; in others they're a
    // short marker (a number, an asterisk, a dagger) set apart from the
    // text, never a heading or a contents entry.
    function isNoteReference(anchor) {
        const type = (anchor.getAttributeNS(OPS, 'type') || anchor.getAttribute('epub:type') || '') + ' ' +
                     (anchor.getAttribute('role') || '')
        if (/noteref|doc-noteref|doc-biblioref/.test(type))
            return true
        if (anchor.closest('h1, h2, h3, h4, h5, h6, nav, header, [role="doc-toc"]'))
            return false
        const text = anchor.textContent.trim()
        const marker = text.length <= 5 && /^[\[(]?([0-9]{1,3}|[*†‡§¶#]+|[a-z]|[ivx]{1,4})[\])]?$/.test(text)
        if (marker || anchor.closest('sup, sub'))
            return true
        return /(^|[\s_-])(fn|note|noteref|footnote|endnote)/i.test(`${anchor.getAttribute('class') || ''} ${anchor.id || ''}`)
    }

    async function documentAt(url, chapter) {
        if (chapter === state.chapter)
            return chapterDocument()
        const response = await fetch(url.origin + url.pathname)
        const type = (response.headers.get('content-type') || 'application/xhtml+xml').split(';')[0]
        return new DOMParser().parseFromString(await response.text(), type === 'text/html' ? 'text/html' : 'application/xhtml+xml')
    }

    // The note's own text: its block, without the link back to the text.
    function noteContent(target, sourcePath) {
        let element = target
        const thin = element.textContent.trim().length < 2 || ['a', 'span', 'sup', 'sub'].includes(element.localName)
        if (thin)
            element = element.closest('li, p, aside, div, section, dd, dt, td, blockquote') || element.parentElement || element
        if (!element || !element.textContent.trim())
            return null
        const clone = element.cloneNode(true)
        for (const link of [...clone.querySelectorAll('a')]) {
            const href = hrefOf(link)
            const backward = /^[\s↩↑⮐⤴]+$|^(back|return)$/i.test(link.textContent.trim()) ||
                             (href && sourcePath && decodeURIComponent(href.split('#')[0]).endsWith(sourcePath))
            if (backward && /^[\s↩↑⮐⤴]+$|^(back|return)$/i.test(link.textContent.trim())) {
                link.remove()
            } else {
                const span = link.ownerDocument.createElementNS('http://www.w3.org/1999/xhtml', 'span')
                span.textContent = link.textContent
                link.replaceWith(span)
            }
        }
        for (const element of [...clone.querySelectorAll('script, style, iframe, object, embed, form')])
            element.remove()
        return new XMLSerializer().serializeToString(clone)
    }

    function noteCss() {
        const s = state.style, t = s.theme
        return `:root { color-scheme: ${t.dark ? 'dark' : 'light'}; }
                html, body { margin: 0; padding: 0; background: transparent; color: ${t.fg}; }
                body { font-family: ${s.font && s.font !== 'publisher' ? `"${s.font}", ` : ''}"Literata", serif;
                       font-size: ${15 * Math.min(1.3, s.fontScale)}px; line-height: 1.5; overflow-wrap: anywhere; }
                body * { color: inherit !important; background: transparent !important; font-family: inherit !important; }
                p { margin: 0 0 .5em; text-indent: 0 !important; }
                img { max-width: 100%; height: auto; }
                @font-face { font-family: "Literata"; src: url("dogear://local/app/fonts/Literata.ttf"); font-weight: 200 900; }
                @font-face { font-family: "Literata"; src: url("dogear://local/app/fonts/Literata-Italic.ttf"); font-style: italic; font-weight: 200 900; }`
    }

    function closeNote() {
        if (!state.note)
            return false
        state.note.remove()
        state.note = null
        return true
    }

    async function showNote(anchor, url, chapter, fragment) {
        let doc
        try {
            doc = await documentAt(url, chapter)
        } catch (error) {
            return false
        }
        const target = doc.getElementById(fragment) || doc.querySelector(`[name="${CSS.escape(fragment)}"]`)
        if (!target)
            return false
        const sourceDocumentPath = decodeURIComponent(new URL(chapterDocument().URL).pathname.split('/').pop())
        const html = noteContent(target, sourceDocumentPath)
        if (!html)
            return false

        closeNote()
        const t = state.style.theme
        const note = document.createElement('div')
        note.className = 'note'
        const view = document.createElement('iframe')
        view.setAttribute('sandbox', 'allow-same-origin')
        view.style.colorScheme = t.dark ? 'dark' : 'light'
        view.srcdoc = `<!doctype html><html><head><base href="${url.origin + url.pathname}"><style>${noteCss()}</style></head><body>${html}</body></html>`
        const actions = document.createElement('div')
        actions.className = 'actions'
        const go = document.createElement('button')
        go.textContent = state.style.strings.goToNote
        go.addEventListener('click', () => {
            closeNote()
            run(async () => {
                remember()
                await show({ chapter, fragment }, false)
            })
        })
        actions.appendChild(go)
        note.appendChild(view)
        note.appendChild(actions)
        overlay().appendChild(note)
        state.note = note

        // Beside the reference: below it when there's room, else above.
        const frameRect = frame().getBoundingClientRect()
        const rect = anchor.getBoundingClientRect()
        const width = Math.min(440, window.innerWidth - 32)
        note.style.width = width + 'px'
        const x = clamp(frameRect.left + rect.left - 40, 16, window.innerWidth - width - 16)
        note.style.left = x + 'px'
        view.addEventListener('load', () => {
            const content = view.contentDocument
            const height = Math.min(window.innerHeight * 0.45, content.documentElement.scrollHeight + 2)
            view.style.height = height + 'px'
            if (content.documentElement.scrollHeight > height)
                view.style.overflowY = 'auto'
            const below = frameRect.top + rect.bottom + 10
            const total = note.offsetHeight
            const y = below + total < window.innerHeight - 12 ? below : Math.max(12, frameRect.top + rect.top - total - 10)
            note.style.top = y + 'px'
            requestAnimationFrame(() => note.classList.add('shown'))
        })
        return true
    }

    async function follow(anchor) {
        const href = hrefOf(anchor)
        if (!href)
            return
        const url = new URL(href, anchor.ownerDocument.baseURI)
        if (url.protocol !== 'dogear:') {
            if (state.bridge)
                state.bridge.openExternal(url.href)
            return
        }
        const chapter = spineIndexOf(url)
        if (chapter < 0)
            return
        const fragment = decodeURIComponent(url.hash.slice(1))
        if (fragment && isNoteReference(anchor) && await showNote(anchor, url, chapter, fragment))
            return
        await run(async () => {
            remember()
            await show({ chapter, fragment }, chapter === state.chapter)
        })
    }

    // --- Input ---------------------------------------------------------------

    function onChapterClick(event) {
        const doc = event.currentTarget
        if (state.note && !event.target.closest?.('.note')) {
            closeNote()
            event.preventDefault()
            return
        }
        const selection = doc.getSelection()
        if (selection && !selection.isCollapsed)
            return
        const anchor = event.target.closest ? event.target.closest('a[href], area[href], a') : null
        if (anchor && hrefOf(anchor)) {
            event.preventDefault()
            event.stopPropagation()
            follow(anchor)
            return
        }
        const fraction = event.clientX / state.geometry.width
        tap(fraction)
    }

    function tap(fraction) {
        if (state.geometry.scroll) {
            if (state.bridge)
                state.bridge.tapped('center')
            return
        }
        if (fraction < 0.28)
            api.prev()
        else if (fraction > 0.72)
            api.next()
        else if (state.bridge)
            state.bridge.tapped('center')
    }

    let wheelTotal = 0, wheelQuietUntil = 0, wheelReset = 0
    function onWheel(event) {
        if (!state.geometry)
            return
        if (state.geometry.scroll) {
            const el = scroller()
            const atTop = el.scrollTop <= 0 && event.deltaY < 0
            const atBottom = el.scrollTop + el.clientHeight >= el.scrollHeight - 2 && event.deltaY > 0
            if (!atTop && !atBottom) {
                clearTimeout(wheelReset)
                wheelReset = setTimeout(() => {
                    state.page = pageAtPosition(0, el.scrollTop)
                    report()
                }, 180)
                return
            }
        }
        event.preventDefault()
        const now = performance.now()
        if (now < wheelQuietUntil)
            return
        const scale = event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? 400 : 1
        const delta = (Math.abs(event.deltaX) > Math.abs(event.deltaY) ? event.deltaX : event.deltaY) * scale
        wheelTotal += delta
        clearTimeout(wheelReset)
        wheelReset = setTimeout(() => { wheelTotal = 0 }, 200)
        if (Math.abs(wheelTotal) >= 40) {
            wheelTotal > 0 ? api.next() : api.prev()
            wheelTotal = 0
            wheelQuietUntil = now + 300
        }
    }

    // The window handles keys; these only reach the page if it took the
    // focus, so they are passed on rather than scrolling the page.
    function onKey(event) {
        if (!state.bridge || event.isComposing)
            return
        const modifiers = (event.shiftKey ? 1 : 0) | (event.ctrlKey ? 2 : 0) | (event.altKey ? 4 : 0) | (event.metaKey ? 8 : 0)
        if (event.ctrlKey && (event.key === 'c' || event.key === 'C'))
            return
        event.preventDefault()
        event.stopPropagation()
        state.bridge.keyPressed(event.key, modifiers)
    }

    let selectionTimer = 0
    function onSelection() {
        clearTimeout(selectionTimer)
        selectionTimer = setTimeout(() => {
            if (!state.bridge || !state.index)
                return
            const selection = chapterDocument().getSelection()
            if (!selection || selection.isCollapsed || !selection.rangeCount) {
                state.bridge.selectionChanged({})
                return
            }
            const range = selection.getRangeAt(0)
            const start = offsetOf(range.startContainer, range.startOffset)
            const end = offsetOf(range.endContainer, range.endOffset)
            const rect = range.getBoundingClientRect()
            const frameRect = frame().getBoundingClientRect()
            state.bridge.selectionChanged({
                chapter: state.chapter,
                start, end,
                text: selection.toString().split(SHY).join(''),
                x: frameRect.left + rect.left, y: frameRect.top + rect.top,
                width: rect.width, height: rect.height,
            })
        }, 220)
    }

    let resizeTimer = 0
    function onResize() {
        clearTimeout(resizeTimer)
        resizeTimer = setTimeout(() => run(relayout), 120)
    }

    async function relayout() {
        if (!state.index || !state.location)
            return
        const place = { chapter: state.chapter, offset: state.location.offset, snippet: state.location.snippet }
        applyLayout()
        measure()
        await show(place, false)
    }

    // --- Listening ---------------------------------------------------------------

    // What can be read aloud from a chapter document, block by block, in
    // the text model's offsets: each paragraph's text, the spans not to say
    // (note markers), and what kind of block it is.
    const sceneBreak = /^\s*(?:[*⁂※✻❦•·~#]\s*){1,7}$/

    function blocksOf(doc) {
        const index = buildIndex(doc)
        const blocks = []
        const blockSelector = 'p, h1, h2, h3, h4, h5, h6, li, blockquote, dd, dt, figcaption, td, th, pre, div, section, article, aside, body'
        let current = null
        for (const entry of index.nodes) {
            const parent = entry.node.parentElement
            const block = parent ? parent.closest(blockSelector) : null
            if (!current || current.element !== block) {
                current = { element: block, start: entry.start, end: entry.start, skips: [] }
                blocks.push(current)
            }
            current.end = entry.start + entry.length
            // Note references and their markers are seen, not heard.
            const anchor = parent && parent.closest('a')
            const marker = parent && parent.closest('sup, sub')
            if ((anchor && hrefOf(anchor) && isNoteReference(anchor)) ||
                (marker && /^[\s\d*†‡§¶#a-z.,]*$/i.test(entry.node.data)))
                current.skips.push([entry.start, entry.start + entry.length])
        }
        const result = []
        for (const block of blocks) {
            const text = index.text.slice(block.start, block.end)
            if (!text.trim())
                continue
            const tag = block.element ? block.element.localName : ''
            const kind = sceneBreak.test(text) ? 'break' : /^h[1-6]$/.test(tag) ? 'heading' : 'text'
            result.push({ start: block.start, end: block.end, text, skips: block.skips, kind })
        }
        return result
    }

    // The visible chapter's blocks, or another chapter's, parsed aside
    // without showing it.
    async function blocksOfChapter(chapter) {
        if (chapter === state.chapter && state.index)
            return blocksOf(chapterDocument())
        const url = new URL(state.book.base + state.book.spine[chapter].href)
        const doc = await documentAt(url, chapter)
        return blocksOf(doc)
    }

        // --- The window's interface ------------------------------------------------

    const api = {
        open(book, position, style) {
            return run(async () => {
                state.book = book
                state.style = style
                state.history = []
                state.chapter = -1
                state.index = null
                applyPageColors()
                await show({ chapter: position.chapter || 0, offset: position.offset || 0, snippet: position.snippet || '' }, false)
            })
        },

        setStyle(style) {
            const reload = !state.style || style.hyphenate !== state.style.hyphenate || style.justify !== state.style.justify ||
                           style.font !== state.style.font || style.publisher !== state.style.publisher
            state.style = style
            applyPageColors()
            return run(async () => {
                if (!state.index || !state.location)
                    return
                const place = { chapter: state.chapter, offset: state.location.offset, snippet: state.location.snippet }
                if (reload) {
                    state.index = null
                    await show(place, false)
                    return
                }
                applyTypography(chapterDocument())
                await settle(chapterDocument())
                applyLayout()
                measure()
                await show(place, false)
            })
        },

        next() {
            return run(async () => {
                if (!state.index)
                    return
                if (state.page < state.pages - 1) {
                    await turnTo(state.page + 1, true)
                    report()
                    return
                }
                const chapter = linearAfter(state.chapter)
                if (chapter < 0) {
                    report()
                    return
                }
                await show({ chapter, offset: 0 }, false)
            })
        },

        prev() {
            return run(async () => {
                if (!state.index)
                    return
                if (state.page > 0) {
                    await turnTo(state.page - 1, true)
                    report()
                    return
                }
                const chapter = linearBefore(state.chapter)
                if (chapter >= 0)
                    await show({ chapter, end: true }, false)
            })
        },

        nextChapter() {
            return run(async () => {
                const chapter = linearAfter(state.chapter)
                if (chapter >= 0)
                    await show({ chapter, offset: 0 }, false)
            })
        },

        prevChapter() {
            return run(async () => {
                // From inside a chapter, its own start; from its start, the one before.
                const chapter = state.page > 0 ? state.chapter : linearBefore(state.chapter)
                if (chapter >= 0)
                    await show({ chapter, offset: 0 }, false)
            })
        },

        goTo(place) {
            return run(async () => {
                if (!place.quiet)
                    remember()
                await show(place, place.chapter === state.chapter && !place.instant)
            })
        },

        back() {
            return run(async () => {
                const place = state.history.pop()
                if (place)
                    await show(place, false)
            })
        },

        canGoBack() { return state.history.length > 0 },

        // Closes the note pop-over or the selection; false when there was
        // nothing to close, so the window can go back to the library.
        escape() {
            if (closeNote())
                return true
            const selection = state.index ? chapterDocument().getSelection() : null
            if (selection && !selection.isCollapsed) {
                selection.removeAllRanges()
                return true
            }
            return false
        },

        location() { return state.location },

        // What the surface is showing, for tests.
        debug() {
            const f = frame()
            return JSON.stringify({
                chapter: state.chapter, page: state.page, pages: state.pages, bridge: !!state.bridge,
                book: !!state.book, geometry: state.geometry, src: f.src, shown: f.classList.contains('shown'),
                indexed: state.index ? state.index.length : -1, location: state.location,
            })
        },

        // Marks spans of the chapter's text: [{start, end}] under a name
        // styled in the typography (dg-search, dg-speaking).
        highlight(name, spans) {
            const view = frame().contentWindow
            if (!state.index || !view || !view.CSS || !view.CSS.highlights)
                return
            const ranges = (spans || []).map(span => rangeFor(span.start, span.end)).filter(Boolean)
            if (ranges.length)
                view.CSS.highlights.set(name, new view.Highlight(...ranges))
            else
                view.CSS.highlights.delete(name)
        },

        // The blocks to read aloud from a chapter, reported to the bridge
        // when they're ready (the window can't wait on a promise).
        requestSpeakable(chapter) {
            const wanted = chapter === undefined || chapter === null || chapter < 0 ? state.chapter : chapter
            blocksOfChapter(wanted)
                .then(blocks => state.bridge.speakableReady(wanted, JSON.stringify(blocks)))
                .catch(error => {
                    log(error && error.stack || error)
                    state.bridge.speakableReady(wanted, '[]')
                })
        },

        // The offset the narration should start from: the top of this page.
        listeningStart() {
            return state.location ? { chapter: state.location.chapter, offset: state.location.offset } : null
        },

        // The sentence being spoken: highlighted, and the page turned to it
        // when `follow` is set.
        speak(start, end, follow) {
            if (!state.index)
                return
            api.highlight('dg-speaking', [{ start, end }])
            if (!follow)
                return
            const page = pageOfOffset(start)
            if (page !== state.page)
                run(async () => {
                    await turnTo(page, true)
                    report()
                })
        },

        // A few sentences from the page in view: how a voice would read
        // this book. A paragraph that begins on the page is preferred to one
        // carried over from the page before.
        sampleText() {
            if (!state.index)
                return ''
            const blocks = blocksOf(chapterDocument()).filter(b => b.kind === 'text' && b.text.trim().length > 60)
            const from = state.location ? state.location.offset : 0
            const block = blocks.find(b => b.start >= from) || blocks.find(b => b.end > from) || blocks[0]
            if (!block)
                return ''
            let text = ''
            for (let i = 0; i < block.text.length; ++i) {
                const at = block.start + i
                if (!block.skips.some(([start, end]) => at >= start && at < end))
                    text += block.text[i]
            }
            text = text.replace(/\s+/g, ' ').trim()
            if (text.length <= 320)
                return text
            let end = -1
            for (const match of text.matchAll(/[.!?\u2026]['"\u2019\u201d)\]]*(?=\s)/g)) {
                const stop = match.index + match[0].length
                if (stop > 320)
                    break
                end = stop
            }
            return end > 60 ? text.slice(0, end) : text.slice(0, text.lastIndexOf(' ', 320)) + '\u2026'
        },

        // The selected text, for copying from the window.
        selectedText() {
            const selection = state.index ? chapterDocument().getSelection() : null
            return selection ? selection.toString().split(SHY).join('') : ''
        },
    }

    function applyPageColors() {
        const t = state.style.theme
        const root = document.documentElement.style
        root.setProperty('--dg-bg', t.bg)
        root.setProperty('--dg-fg', t.fg)
        root.setProperty('--dg-link', t.link)
        root.setProperty('--dg-muted', t.muted)
        root.setProperty('--dg-popover', t.popover)
        root.setProperty('--dg-border', t.border)
        root.setProperty('--dg-radius', (state.style.radius || 0) + 'px')
        document.documentElement.style.colorScheme = t.dark ? 'dark' : 'light'
    }

    document.addEventListener('DOMContentLoaded', () => {
        new ResizeObserver(onResize).observe(stage())
        document.addEventListener('wheel', onWheel, { passive: false })
        document.addEventListener('keydown', onKey, true)
        stage().addEventListener('click', event => {
            if (event.target !== stage())
                return
            if (state.note) {
                closeNote()
                return
            }
            const rect = stage().getBoundingClientRect()
            tap((event.clientX - rect.left) / rect.width)
        })
        new QWebChannel(qt.webChannelTransport, channel => {
            state.bridge = channel.objects.bridge
            state.bridge.surfaceLoaded()
        })
    })

    return api
})()
