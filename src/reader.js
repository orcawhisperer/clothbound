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
        rtl: false,
        vertical: '',
        geometry: null,
        stageWidth: 0,
        stageHeight: 0,
        nudgeGuardUntil: 0,
        index: null,
        labels: [],
        location: null,
        history: [],
        forward: [],
        justJumped: false,
        prefetched: new Set(),
        hyphenatedNodes: new Map(),
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
    const sessionId = () => (state.book && state.book.session) || 0
    const escapeAttr = text => String(text).replace(/&/g, '&amp;').replace(/"/g, '&quot;').replace(/</g, '&lt;').replace(/>/g, '&gt;')

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

    function isNudgeGuarded() {
        return performance.now() < state.nudgeGuardUntil
    }

    function isNudgeDelta(w, h) {
        if (state.stageWidth <= 0 || state.stageHeight <= 0)
            return false
        return w === state.stageWidth
            && h < state.stageHeight
            && state.stageHeight - h <= 2
    }

    function effectiveStageSize() {
        const s = stage()
        const w = s ? s.clientWidth : 0
        const h = s ? s.clientHeight : 0
        if (isNudgeGuarded() && isNudgeDelta(w, h))
            return { width: state.stageWidth, height: state.stageHeight }
        return { width: w, height: h }
    }

    // The frame holds exactly one screen: one column (or two side by side)
    // with half a gap on each side, so moving by the frame's width shows the
    // next screen with the same margins. A wide window keeps lines at a
    // comfortable measure and centers the frame.
    function computeGeometry() {
        const s = state.style
        const size = effectiveStageSize()
        const W = Math.max(160, size.width), H = Math.max(160, size.height)
        const em = 16 * s.fontScale
        const measure = s.measure * em
        const side = Math.max(20, Math.min(Math.round(W * s.margin), Math.floor((W - 120) / 2)))
        if (state.vertical) {
            const availH = Math.max(160, H - s.marginTop - s.marginBottom)
            const height = Math.max(160, Math.min(Math.round(measure * 1.2), availH))
            const width = Math.max(160, W - 2 * side)
            return { scroll: s.layout === 'scroll', vertical: true, width, height,
                     top: Math.max(s.marginTop, Math.round((H - height) / 2)),
                     left: Math.round((W - width) / 2), columns: 1, gap: 2 * side, column: width, step: width }
        }
        if (s.layout === 'scroll') {
            const column = Math.max(120, Math.floor(Math.min(measure, W - 2 * side)))
            const width = column + 2 * side
            return { scroll: true, vertical: false, width, height: H, top: 0, left: Math.round((W - width) / 2), columns: 1,
                     gap: 2 * side, column, step: Math.max(100, H - s.marginTop - s.marginBottom - 2 * em) }
        }
        const height = Math.max(160, H - s.marginTop - s.marginBottom)
        const twoFit = (W - 4 * side) / 2 >= 22 * em
        // Fall back to one column when tiled too narrow for two readable columns.
        const wantDouble = s.layout === 'double' || (s.layout === 'auto' && twoFit && W > H * 1.15)
        const columns = wantDouble && W >= 360 ? 2 : 1
        let gap = columns === 2 ? clamp(2 * side, 56, 160) : 2 * side
        if (columns === 2 && W - 2 * gap < 240)
            gap = Math.max(24, Math.floor((W - 240) / 2) & ~1)
        const column = Math.max(120, Math.floor(Math.min(measure, columns === 2 ? (W - 2 * gap) / 2 : W - gap)))
        const width = Math.max(120, columns * (column + gap))
        return { scroll: false, vertical: false, width, height, top: s.marginTop, left: Math.round((W - width) / 2),
                 columns, gap, column, step: width }
    }

    function layoutCss(g) {
        const s = state.style
        if (g.vertical)
            return `html { height: ${g.height}px !important; width: ${g.width}px !important;
                           box-sizing: border-box !important; margin: 0 !important; padding: 0 !important;
                           columns: auto !important;
                           overflow-x: ${g.scroll ? 'auto' : 'hidden'} !important;
                           overflow-y: hidden !important; }
                    html::-webkit-scrollbar { display: none; }
                    img, svg, video, object, embed { max-height: ${g.height - 6}px !important; max-width: ${g.width - 6}px !important; }`
        const dirRule = state.rtl ? 'direction: rtl;' : ''
        if (g.scroll)
            return `html { ${dirRule} height: auto !important; columns: auto !important; overflow-x: hidden !important;
                           overflow-y: auto !important; box-sizing: border-box !important; margin: 0 !important;
                           padding: ${s.marginTop}px ${g.gap / 2}px ${s.marginBottom}px !important; }
                    html::-webkit-scrollbar { display: none; }
                    img, svg, video { max-height: ${Math.round(g.height * 0.9)}px !important; }`
        return `html { ${dirRule} height: ${g.height}px !important; width: ${g.width}px !important;
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
        const weight = clamp(Math.round(s.fontWeight || 400), 300, 700)
        const boldWeight = Math.min(900, weight + 250)
        const headingWeight = Math.min(900, weight + 200)
        const paragraphSpacing = clamp(s.paragraphSpacing !== undefined ? Number(s.paragraphSpacing) : 0.4, 0, 1.2)
        const colorRules = own
            ? `body *, body *::before, body *::after { color: inherit !important; background-color: transparent !important; }
               pre { background-color: ${t.dark ? 'rgba(255,255,255,0.05)' : 'rgba(0,0,0,0.04)'} !important;
                     padding: 0.7em 0.9em; border-radius: 6px; }
               :not(pre) > code { background-color: ${t.dark ? 'rgba(255,255,255,0.07)' : 'rgba(0,0,0,0.05)'} !important;
                                  padding: 0.08em 0.28em; border-radius: 3px; }`
            : t.dark
                ? `body *, body *::before, body *::after { color: inherit !important; background-color: transparent !important; }
                   pre, code, blockquote, aside, th, [class*="callout"], [class*="note"], [class*="box"], [class*="tip"], [class*="sidebar"] {
                       background-color: rgba(255, 255, 255, 0.06) !important;
                   }`
                : `body *, body *::before, body *::after { color: inherit; }`
        const darkMediaRules = t.dark ? `
svg:not(:has(image)) { filter: invert(0.86) hue-rotate(180deg); }
img[src$=".svg" i], img[src*=".svg?" i], img[src^="data:image/svg+xml" i], img[data-dg-lineart] {
    filter: invert(0.86) hue-rotate(180deg) !important;
}
img[data-dg-matte] {
    background-color: rgba(242, 239, 232, 0.86) !important;
    border-radius: 4px;
    padding: 3px;
    filter: brightness(0.92) contrast(1.02) !important;
}
img:not([data-dg-lineart]):not([data-dg-matte]):not([src$=".svg" i]):not([src*=".svg?" i]):not([src^="data:image/svg+xml" i]) {
    filter: brightness(0.88) contrast(1.04);
}` : ''
        return `
@font-face { font-family: "Literata"; src: url("dogear://local/app/fonts/Literata.ttf"); font-weight: 200 900; font-style: normal; font-display: block; }
@font-face { font-family: "Literata"; src: url("dogear://local/app/fonts/Literata-Italic.ttf"); font-weight: 200 900; font-style: italic; font-display: block; }
html { background: ${t.bg} !important; color: ${t.fg} !important; font-size: ${s.fontScale * 100}% !important;
       -webkit-text-size-adjust: none; text-rendering: optimizeLegibility; font-kerning: normal;
       font-variant-ligatures: common-ligatures; font-optical-sizing: auto; }
body { margin: 0 !important; padding: 0 !important; width: auto !important; max-width: none !important;
       min-height: 0 !important; height: auto !important; background: transparent !important; }
${colorRules}
a, a * { color: ${t.link} !important; }
a { text-decoration: none !important; }
::selection { background: ${t.selection}; }
::highlight(dg-search) { background-color: ${t.search}; }
::highlight(dg-found) { background-color: ${t.found}; }
::highlight(dg-mark-yellow) { background-color: ${t.markYellow}; }
::highlight(dg-mark-green) { background-color: ${t.markGreen}; }
::highlight(dg-mark-blue) { background-color: ${t.markBlue}; }
::highlight(dg-mark-pink) { background-color: ${t.markPink}; }
::highlight(dg-noted) { text-decoration: underline dotted ${t.link}; text-decoration-thickness: 2px; text-underline-offset: 3px; }
::highlight(dg-speaking) { background-color: ${t.speaking}; }
img, svg, video { max-width: 100% !important; object-fit: contain; break-inside: avoid; }
img, svg { cursor: zoom-in; }
a img, a svg { cursor: pointer; }
img { height: auto; }
math, math * { color: ${t.fg} !important; fill: currentColor; stroke: currentColor; }
${darkMediaRules}
h1, h2, h3, h4, h5, h6 { break-after: avoid; break-inside: avoid; }
p { widows: 2; orphans: 2; }
figure { break-inside: avoid; }
figure[data-dg-tall] { break-inside: auto !important; }
pre, table { break-inside: auto; max-width: 100%; }
pre[data-dg-short], table[data-dg-short] { break-inside: avoid; }
table[data-dg-wide] { display: block !important; max-width: 100% !important; overflow-x: auto !important; }
pre, code, kbd, samp, tt { font-family: ui-monospace, "JetBrainsMono Nerd Font", monospace !important; }
pre { white-space: pre-wrap !important; overflow-wrap: anywhere; }
${family ? `body, body :not(pre):not(code):not(kbd):not(samp):not(tt) { font-family: ${family} !important; }` : ''}
${own ? `body, p, li, dd, dt, blockquote, td { font-weight: ${weight} !important; }
b, strong, th { font-weight: ${boldWeight} !important; }
h1, h2, h3, h4, h5, h6 { font-weight: ${headingWeight} !important; }
p, li, dd, dt, blockquote, div, td { line-height: ${s.lineHeight} !important; }
p { margin-top: 0 !important; margin-bottom: ${paragraphSpacing}em !important; }
[data-dg-dropcap]::first-letter { initial-letter: 3; font-weight: ${headingWeight}; margin-inline-end: 0.06em; }
[data-dg-flow] { text-align: ${s.justify ? 'justify' : 'start'} !important;
                 ${s.justify && state.hyphenation ? 'hyphens: manual; -webkit-hyphens: manual;' : 'hyphens: none;'} }` : ''}
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
        const size = effectiveStageSize()
        state.stageWidth = size.width
        state.stageHeight = size.height
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

    // Prevent tall <pre>/<table>/<figure> blocks from clipping off multi-column
    // pages and let overly wide tables scroll horizontally. Batches all layout
    // reads before attribute writes to avoid reflow thrashing.
    function fitBlocks(doc, g) {
        if (!doc || !g)
            return
        const blocks = [...doc.querySelectorAll('pre, table, figure')]
        if (!blocks.length)
            return
        const measurements = blocks.map(el => {
            const rect = el.getBoundingClientRect()
            return { el, tag: el.localName, height: Math.max(rect.height, el.scrollHeight || 0),
                     width: Math.max(rect.width, el.scrollWidth || 0) }
        })
        const tallThreshold = g.height * 0.78
        const shortThreshold = g.height * 0.6
        for (const m of measurements) {
            if (m.tag === 'figure') {
                if (m.height > tallThreshold)
                    m.el.setAttribute('data-dg-tall', '')
                else
                    m.el.removeAttribute('data-dg-tall')
            } else {
                if (!g.scroll && m.height > 0 && m.height <= shortThreshold)
                    m.el.setAttribute('data-dg-short', '')
                else
                    m.el.removeAttribute('data-dg-short')
            }
            if (m.tag === 'table') {
                if (m.width > g.column + 2)
                    m.el.setAttribute('data-dg-wide', '')
                else
                    m.el.removeAttribute('data-dg-wide')
            }
        }
    }

    // Inspect decoded raster images once so transparent monochrome line-art /
    // math equations invert cleanly in dark mode instead of disappearing.
    let probeCanvas = null
    function classifyImages(doc) {
        if (!doc || !doc.images || !doc.images.length)
            return
        try {
            if (!probeCanvas) {
                probeCanvas = document.createElement('canvas')
                probeCanvas.width = 32
                probeCanvas.height = 32
            }
            const ctx = probeCanvas.getContext('2d', { willReadFrequently: true })
            if (!ctx)
                return
            for (const img of doc.images) {
                if (img.hasAttribute('data-dg-img-checked') || !img.complete || img.naturalWidth <= 0 || img.naturalHeight <= 0)
                    continue
                const src = img.currentSrc || img.src || ''
                if (/\.svg(?:$|\?)|^data:image\/svg\+xml/i.test(src)) {
                    img.setAttribute('data-dg-img-checked', '1')
                    continue
                }
                img.setAttribute('data-dg-img-checked', '1')
                const w = Math.min(32, img.naturalWidth), h = Math.min(32, img.naturalHeight)
                ctx.clearRect(0, 0, w, h)
                ctx.drawImage(img, 0, 0, w, h)
                const data = ctx.getImageData(0, 0, w, h).data
                const total = w * h
                let transparent = 0, opaque = 0, darkLowChroma = 0
                for (let i = 0; i < data.length; i += 4) {
                    const a = data[i + 3]
                    if (a < 200) {
                        ++transparent
                    } else {
                        ++opaque
                        const r = data[i], g = data[i + 1], b = data[i + 2]
                        const luma = (r * 299 + g * 587 + b * 114) / 1000
                        const chroma = Math.max(r, g, b) - Math.min(r, g, b)
                        if (luma < 95 && chroma < 45)
                            ++darkLowChroma
                    }
                }
                if (transparent / total > 0.08 && opaque > 0) {
                    const darkShare = darkLowChroma / opaque
                    if (darkShare > 0.72)
                        img.setAttribute('data-dg-lineart', '')
                    else if (darkShare > 0.15)
                        img.setAttribute('data-dg-matte', '')
                }
            }
        } catch (e) { }
    }

    // --- Preparing a chapter -----------------------------------------------

    function detectDirection(doc) {
        const view = doc.defaultView
        const htmlStyle = view.getComputedStyle(doc.documentElement)
        const bodyStyle = doc.body ? view.getComputedStyle(doc.body) : htmlStyle
        const wm = bodyStyle.writingMode && bodyStyle.writingMode !== 'horizontal-tb'
            ? bodyStyle.writingMode : (htmlStyle.writingMode || 'horizontal-tb')
        state.vertical = wm === 'vertical-rl' || wm === 'vertical-lr' ? wm : ''
        if (state.vertical) {
            state.rtl = state.vertical === 'vertical-rl'
        } else {
            const bookRtl = !!(state.book && state.book.rtl)
            const docRtl = htmlStyle.direction === 'rtl' || bodyStyle.direction === 'rtl'
            state.rtl = bookRtl || docRtl
        }
    }

    // Running text is marked so the reader's alignment applies to it and
    // not to centered headings, verse, or end-aligned signatures.
    // Batch all getComputedStyle reads before setAttribute writes to avoid O(N²) reflows.
    function markFlow(doc) {
        const view = doc.defaultView
        const startEdge = state.rtl ? 'right' : 'left'
        const candidates = []
        for (const element of doc.querySelectorAll('p, li, dd, blockquote, div')) {
            for (const child of element.childNodes) {
                if (child.nodeType === Node.TEXT_NODE && child.data.trim()) {
                    candidates.push(element)
                    break
                }
            }
        }
        const flow = []
        for (const element of candidates) {
            const align = view.getComputedStyle(element).textAlign
            if (align === 'start' || align === startEdge || align === 'justify' || align === '-webkit-auto')
                flow.push(element)
        }
        for (const element of flow)
            element.setAttribute('data-dg-flow', '')
    }

    function hyphenate(doc) {
        const walker = doc.createTreeWalker(doc.body || doc.documentElement, NodeFilter.SHOW_TEXT)
        for (let node = walker.nextNode(); node; node = walker.nextNode()) {
            const parent = node.parentElement
            if (parent && parent.closest('[data-dg-flow]') && !parent.closest('a, code, pre, kbd, samp, tt')) {
                const before = node.data
                Hyphenation.node(node)
                if (node.data !== before && !state.hyphenatedNodes.has(node))
                    state.hyphenatedNodes.set(node, before)
            }
        }
    }

    function unhyphenate() {
        for (const [node, original] of state.hyphenatedNodes)
            node.data = original
        state.hyphenatedNodes.clear()
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

    // A chapter opens with a drop cap, in Dogear's own typography, for
    // horizontal left-to-right text (DropCap, dropcap.js, picks the paragraph).
    function markDropCap(doc) {
        for (const old of doc.querySelectorAll('[data-dg-dropcap]'))
            old.removeAttribute('data-dg-dropcap')
        const s = state.style
        if (s.publisher || s.dropCap === false || state.rtl || state.vertical || typeof DropCap === 'undefined')
            return
        const paragraphs = [...doc.querySelectorAll('p[data-dg-flow]')]
        const index = DropCap.choose(paragraphs.map(p => ({
            text: p.textContent,
            excluded: !!p.closest('blockquote, aside, figure, table, nav, header, footer, li, [class*="epigraph" i]'),
        })))
        if (index >= 0)
            paragraphs[index].setAttribute('data-dg-dropcap', '')
    }

    async function prepare(doc) {
        state.hyphenatedNodes = new Map()
        detectDirection(doc)
        markFlow(doc)
        markDropCap(doc)
        const s = state.style
        state.hyphenation = !s.publisher && s.justify && s.hyphenate && await Hyphenation.load(state.book.language)
        if (state.hyphenation)
            hyphenate(doc)
        applyTypography(doc)
        doc.addEventListener('click', onChapterClick, true)
        doc.addEventListener('wheel', onWheel, { passive: false })
        doc.addEventListener('scroll', scheduleMarginNotes, { passive: true })
        doc.addEventListener('keydown', onKey, true)
        doc.addEventListener('copy', event => {
            const selection = doc.getSelection()
            if (!selection || selection.isCollapsed || !event.clipboardData)
                return
            event.clipboardData.setData('text/plain', selection.toString().split(SHY).join(''))
            event.preventDefault()
        })
        doc.addEventListener('dragstart', event => event.preventDefault())
        doc.addEventListener('selectionchange', onSelection)
        doc.addEventListener('mousemove', onHover, { passive: true })
        doc.addEventListener('mouseleave', () => hideFromHere(400))
    }

    // --- Listening from a paragraph -------------------------------------------

    // While the book is read aloud, a play button waits in the margin beside
    // the paragraph under the pointer: a click moves the voice there.
    let fromHere = null, fromHereBlock = null, fromHereTimer = 0
    function fromHereButton() {
        if (!fromHere) {
            fromHere = document.createElement('button')
            fromHere.className = 'from-here'
            fromHere.title = state.style.strings.listenFromHere || ''
            fromHere.innerHTML = '<svg viewBox="0 0 16 16" width="12" height="12"><path d="M5 3.5v9l7-4.5z" fill="currentColor"/></svg>'
            fromHere.addEventListener('mouseenter', () => clearTimeout(fromHereTimer))
            fromHere.addEventListener('mouseleave', () => hideFromHere(300))
            fromHere.addEventListener('click', event => {
                event.preventDefault()
                if (fromHereBlock && state.bridge) {
                    const start = blockStart(fromHereBlock)
                    if (start !== null) {
                        if (state.bridge.listenFromSession)
                            state.bridge.listenFromSession(sessionId(), state.chapter, start)
                        else
                            state.bridge.listenFrom(state.chapter, start)
                    }
                }
                hideFromHere(0)
            })
            document.getElementById('overlay').appendChild(fromHere)
        }
        return fromHere
    }
    function hideFromHere(delay) {
        clearTimeout(fromHereTimer)
        fromHereTimer = setTimeout(() => {
            if (fromHere)
                fromHere.classList.remove('shown')
            fromHereBlock = null
        }, delay)
    }
    // Where a paragraph's words begin, as an offset.
    function blockStart(block) {
        const walker = block.ownerDocument.createTreeWalker(block, NodeFilter.SHOW_TEXT)
        for (let node = walker.nextNode(); node; node = walker.nextNode())
            if (node.data.trim()) {
                const at = offsetOf(node, node.data.search(/\S/))
                if (at !== null)
                    return at
            }
        return null
    }
    function onHover(event) {
        if (!state.style || !state.style.listening || !state.index)
            return
        const block = event.target.closest && event.target.closest('p, h1, h2, h3, h4, h5, h6, li, blockquote, dd, figcaption')
        if (!block || !block.textContent.trim()) {
            hideFromHere(400)
            return
        }
        clearTimeout(fromHereTimer)
        if (block === fromHereBlock)
            return
        fromHereBlock = block
        // Beside the paragraph's first line, in the margin or the gutter.
        const range = block.ownerDocument.createRange()
        range.selectNodeContents(block)
        const line = range.getClientRects()[0]
        if (!line)
            return
        const frameRect = frame().getBoundingClientRect()
        const button = fromHereButton()
        if (state.vertical) {
            button.style.left = clamp(frameRect.left + line.left + line.width / 2 - 13, 4, window.innerWidth - 30) + 'px'
            button.style.top = Math.max(4, frameRect.top + line.top - 32) + 'px'
        } else if (state.rtl) {
            button.style.left = Math.min(window.innerWidth - 30, frameRect.left + line.right + 8) + 'px'
            button.style.top = (frameRect.top + line.top + line.height / 2 - 13) + 'px'
        } else {
            button.style.left = Math.max(4, frameRect.left + line.left - 34) + 'px'
            button.style.top = (frameRect.top + line.top + line.height / 2 - 13) + 'px'
        }
        button.classList.add('shown')
    }

    // What's at a point of the page (in the window's terms): its chapter and
    // offset, and the words selected, if any, for the right-click menu.
    function placeAt(x, y) {
        if (!state.index)
            return null
        const frameRect = frame().getBoundingClientRect()
        const doc = chapterDocument()
        let node = null, index = 0
        const fx = x - frameRect.left, fy = y - frameRect.top
        if (doc.caretPositionFromPoint) {
            const position = doc.caretPositionFromPoint(fx, fy)
            if (position) {
                node = position.offsetNode
                index = position.offset
            }
        } else if (doc.caretRangeFromPoint) {
            const range = doc.caretRangeFromPoint(fx, fy)
            if (range) {
                node = range.startContainer
                index = range.startOffset
            }
        }
        const offset = node ? offsetOf(node, index) : null
        if (offset === null)
            return null
        // From the start of the sentence the click was in.
        const text = state.index.text
        let start = offset
        while (start > 0 && !(start >= 2 && /[.!?\u2026]\s/.test(text.slice(start - 2, start))) && text[start - 1] !== '\n')
            --start
        while (start < text.length && /\s/.test(text[start]))
            ++start
        const selection = doc.getSelection()
        return { chapter: state.chapter, offset: start, selected: selection ? selection.toString().split(SHY).join('') : '' }
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
        if (!state.index || !node)
            return null
        const entry = state.index.byNode.get(node)
        if (entry) {
            if (!entry.shy)
                return entry.start + Math.min(index, entry.length)
            let model = 0
            for (let i = 0; i < index && i < node.data.length; ++i)
                if (node.data[i] !== SHY)
                    ++model
            return entry.start + model
        }
        // Selections (e.g. triple-click, cross-paragraph drags) often land on
        // an Element boundary instead of a Text node: map that DOM boundary to
        // the exact text-model offset via binary search over indexed nodes.
        const nodes = state.index.nodes
        if (!nodes.length)
            return 0
        const doc = chapterDocument()
        if (!doc || !doc.contains(node))
            return null
        const boundary = doc.createRange()
        try {
            const childCount = node.childNodes ? node.childNodes.length : 0
            boundary.setStart(node, clamp(index || 0, 0, childCount))
            boundary.collapse(true)
        } catch (e) {
            return null
        }
        let low = 0, high = nodes.length
        while (low < high) {
            const mid = (low + high) >> 1
            if (boundary.comparePoint(nodes[mid].node, 0) < 0)
                low = mid + 1
            else
                high = mid
        }
        return low < nodes.length ? nodes[low].start : state.index.length
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
        if (g.scroll && !g.vertical)
            return clamp(Math.floor((y + 1) / g.step), 0, state.pages - 1)
        if (state.rtl)
            return clamp(Math.floor((g.width - x) / g.step), 0, state.pages - 1)
        return clamp(Math.floor((x + 1) / g.step), 0, state.pages - 1)
    }

    function pageOfRect(rect) {
        if (!rect)
            return 0
        const el = scroller()
        const x = state.rtl ? (rect.left + rect.right) / 2 + el.scrollLeft : rect.left + el.scrollLeft
        return pageAtPosition(x, rect.top + el.scrollTop)
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
        const g = state.geometry
        const found = firstOffsetPast(rect => {
            if (g.scroll && !g.vertical)
                return rect.bottom > 1
            return state.rtl ? rect.left < g.width - 1 : rect.right > 1
        })
        return found === null ? 0 : found
    }

    // The first character of the next screen: where this one ends.
    function nextPageOffset() {
        const g = state.geometry
        const found = firstOffsetPast(rect => {
            if (g.scroll && !g.vertical)
                return rect.top > g.height - 1
            return state.rtl ? rect.right < 1 : rect.left > g.width - 1
        })
        return found === null ? state.index.length : found
    }

    // The first character, in the text's order, whose box is past a line on
    // the screen; null when none is.
    function firstOffsetPast(past) {
        const nodes = state.index.nodes
        if (!nodes.length)
            return null
        const range = chapterDocument().createRange()
        let any = false
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
                any = true
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
                // Unbroken soft hyphens and collapsed whitespace have no boxes;
                // probe forward to the next visible character before deciding.
                let probe = middle, rect = null
                while (probe <= highChar) {
                    const code = data.charCodeAt(probe)
                    const step = code >= 0xD800 && code <= 0xDBFF && probe + 1 < data.length ? 2 : 1
                    range.setStart(entry.node, probe)
                    range.setEnd(entry.node, probe + step)
                    const rects = range.getClientRects()
                    const candidate = rects.length ? rects[rects.length - 1] : null
                    if (candidate && (candidate.width > 0 || candidate.height > 0)) {
                        rect = candidate
                        break
                    }
                    probe += step
                }
                if (rect && past(rect)) {
                    first = probe
                    highChar = middle - 1
                } else {
                    lowChar = probe + 1
                }
            }
            if (first >= 0) {
                while (first < data.length && /\s/.test(data[first]))
                    ++first
                if (first < data.length)
                    return offsetOf(entry.node, first)
            }
        }
        return any ? offsetOf(nodes[found].node, 0) || 0 : null
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
        if (g.scroll && !g.vertical) {
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
            const at = (g.scroll && !g.vertical)
                ? rect.top + el.scrollTop
                : state.rtl
                    ? g.width - ((rect.left + rect.right) / 2 + el.scrollLeft)
                    : rect.left + el.scrollLeft
            state.labels.push({ label: target.label, at })
        }
        state.labels.sort((a, b) => a.at - b.at)
    }

    // Where the last content box ends, for books whose final column is not
    // counted in the scroll width.
    function lastBoxes() {
        const doc = chapterDocument(), el = scroller(), g = state.geometry
        const body = doc.body || doc.documentElement
        const last = body.lastElementChild
        if (!last)
            return [0]
        const rects = last.getClientRects()
        const rect = rects.length ? rects[rects.length - 1] : last.getBoundingClientRect()
        return [state.rtl ? g.width - (rect.left + el.scrollLeft) : rect.right + el.scrollLeft]
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

    // A turn under way lands at once (another turn, or a jump, overtakes it).
    function cancelAnimation() {
        const animation = state.animation
        if (!animation)
            return
        state.animation = null
        const horizontal = !state.geometry.scroll || state.geometry.vertical
        scroller().scrollTo({ [horizontal ? 'left' : 'top']: animation.target, behavior: 'instant' })
        animation.finish()
    }

    function turnTo(page, animate) {
        cancelAnimation()
        closeNote()
        const g = state.geometry
        state.page = clamp(page, 0, state.pages - 1)
        const el = scroller()
        const horizontal = !g.scroll || g.vertical
        const target = horizontal && state.rtl ? -(state.page * g.step) : state.page * g.step
        const axis = horizontal ? 'left' : 'top'
        const now = horizontal ? el.scrollLeft : el.scrollTop
        const reduced = matchMedia('(prefers-reduced-motion: reduce)').matches
        if (!animate || !state.style.animate || reduced || Math.abs(target - now) < 1) {
            el.scrollTo({ [axis]: target, behavior: 'instant' })
            return Promise.resolve()
        }
        // Chromium's own smooth scroll runs on its compositor, so a turn stays
        // fluid however busy the page is (a new chapter's hyphens, the
        // voice's highlight moving on).
        return new Promise(resolve => {
            const doc = chapterDocument()
            let timer = 0
            const finish = () => {
                clearTimeout(timer)
                doc.removeEventListener('scrollend', finish)
                if (state.animation && state.animation.finish === finish)
                    state.animation = null
                // Land exactly on the page, whatever the scroll settled on.
                if (Math.abs((horizontal ? el.scrollLeft : el.scrollTop) - target) > 0.5)
                    el.scrollTo({ [axis]: target, behavior: 'instant' })
                resolve()
            }
            timer = setTimeout(finish, 700)
            doc.addEventListener('scrollend', finish)
            state.animation = { target, finish }
            el.scrollTo({ [axis]: target, behavior: 'smooth' })
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

    // Warm the next chapter in BookScheme and Chromium's memory cache when
    // approaching the end of the current chapter so chapter turns feel instant.
    function prefetchAdjacent() {
        if (!state.book || state.chapter < 0)
            return
        if (state.pages > 2 && state.page < state.pages - 2)
            return
        const nextChapter = linearAfter(state.chapter)
        if (nextChapter < 0 || state.prefetched.has(nextChapter))
            return
        state.prefetched.add(nextChapter)
        const url = state.book.base + state.book.spine[nextChapter].href
        const kick = () => fetch(url, { priority: 'low' }).catch(() => { })
        if (typeof requestIdleCallback === 'function')
            requestIdleCallback(kick, { timeout: 600 })
        else
            setTimeout(kick, 120)
    }

    function report(automated = false) {
        hideFromHere(0)
        if (!state.index)
            return
        const offset = firstVisibleOffset()
        const backEntry = state.history.length ? state.history[state.history.length - 1] : null
        const jumped = state.justJumped
        state.justJumped = false
        state.location = {
            chapter: state.chapter,
            offset,
            end: nextPageOffset(),
            snippet: state.index.text.slice(offset, offset + 48),
            page: state.page,
            pages: state.pages,
            pageLabel: labelForPage(state.page),
            atEnd: state.page >= state.pages - 1 && linearAfter(state.chapter) < 0,
            rtl: state.rtl,
            canGoBack: state.history.length > 0,
            canGoForward: state.forward.length > 0,
            backChapter: backEntry ? backEntry.chapter : -1,
            backPage: backEntry ? backEntry.page : 0,
            backPageLabel: backEntry ? (backEntry.pageLabel || '') : '',
            jumped,
            automated: !!automated,
            session: sessionId(),
        }
        if (state.bridge)
            state.bridge.relocated(state.location)
        prefetchAdjacent()
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
        applyLayout()
        await settle(doc)
        classifyImages(doc)
        fitBlocks(doc, state.geometry)
        state.index = buildIndex(doc)
        measure()
        restoreDecorations()
    }

    // Notes beside their highlights, in the outer margin of the page they're
    // on, when that margin can hold a readable card (otherwise a note shows as
    // the highlight's dotted underline, and opens with a click). Cards on one
    // side stack downwards rather than overlap. Laid out again on every turn,
    // resize and change of marks.
    const marginNoteMinimum = 140
    let marginNotesFrame = 0
    function scheduleMarginNotes() {
        if (marginNotesFrame)
            return
        marginNotesFrame = requestAnimationFrame(() => {
            marginNotesFrame = 0
            layoutMarginNotes()
        })
    }
    let marginCards = []
    function layoutMarginNotes() {
        for (const old of marginCards)
            old.remove()
        marginCards = []
        const noted = (state.marks || []).filter(span => span.noted && span.note)
        if (!noted.length || !state.index || state.vertical || state.style.marginNotes === false)
            return
        const frameRect = frame().getBoundingClientRect()
        const g = state.geometry
        // The text block's outer edges in the window: the left of the first
        // column and the right of the last (the frame holds the gaps around them).
        const textLeft = frameRect.left + g.gap / 2, textRight = frameRect.left + frameRect.width - g.gap / 2
        const cards = []
        for (const span of noted) {
            const range = rangeFor(span.start, span.end)
            if (!range)
                continue
            const line = [...range.getClientRects()].find(r => r.width > 0 && r.height > 0
                && r.right > 0 && r.left < frameRect.width && r.bottom > 0 && r.top < frameRect.height)
            if (!line)
                continue
            // On a spread, the left page's notes go in the left margin.
            const leftPage = g.columns > 1 && (line.left + line.right) / 2 < frameRect.width / 2
            const room = leftPage ? textLeft - 20 : window.innerWidth - textRight - 20
            if (room < marginNoteMinimum)
                continue
            cards.push({ span, line, leftPage, width: Math.min(220, room - 8) })
        }
        cards.sort((a, b) => a.line.top - b.line.top)
        const bottoms = { left: -Infinity, right: -Infinity }
        for (const { span, line, leftPage, width } of cards) {
            const card = document.createElement('div')
            card.className = 'margin-note'
            card.textContent = span.note
            card.style.width = width + 'px'
            card.style.left = (leftPage ? textLeft - 16 - width : textRight + 16) + 'px'
            const mark = state.style.theme['mark' + span.color.charAt(0).toUpperCase() + span.color.slice(1)]
            if (mark)
                card.style.setProperty('--dg-mark', mark)
            card.addEventListener('click', event => {
                event.preventDefault()
                if (state.bridge)
                    state.bridge.markClicked(span.id, { x: frameRect.left + line.left, y: frameRect.top + line.top,
                                                        width: line.width, height: line.height, session: sessionId() })
            })
            overlay().appendChild(card)
            marginCards.push(card)
            const side = leftPage ? 'left' : 'right'
            const top = Math.max(frameRect.top + line.top - 2, bottoms[side] + 8)
            card.style.top = top + 'px'
            bottoms[side] = top + card.offsetHeight
        }
    }

    // A chapter's highlights, search hits and spoken sentence, drawn again
    // after it's laid out anew; a different chapter starts with none.
    function restoreDecorations() {
        scheduleMarginNotes()
        if (state.decorated !== state.chapter) {
            state.decorations = {}
            state.decorated = state.chapter
            return
        }
        for (const [name, spans] of Object.entries(state.decorations || {}))
            api.highlight(name, spans)
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
        report(!!place.automated)
    }

    function snapshotLocation() {
        if (!state.location)
            return null
        return {
            chapter: state.location.chapter,
            offset: state.location.offset,
            snippet: state.location.snippet,
            page: state.location.page,
            pageLabel: state.location.pageLabel || '',
        }
    }

    function remember() {
        const snap = snapshotLocation()
        if (!snap)
            return
        const top = state.history[state.history.length - 1]
        if (!top || top.chapter !== snap.chapter || Math.abs(top.offset - snap.offset) > 40)
            state.history.push(snap)
        if (state.history.length > 50)
            state.history.shift()
        state.forward = []
        state.justJumped = true
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
        const text = await response.text()
        const type = (response.headers.get('content-type') || 'application/xhtml+xml').split(';')[0]
        const parser = new DOMParser()
        const parsed = parser.parseFromString(text, type === 'text/html' ? 'text/html' : 'application/xhtml+xml')
        if (type !== 'text/html' && parsed.querySelector('parsererror'))
            return parser.parseFromString(text, 'text/html')
        return parsed
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
            const text = link.textContent.trim()
            const isBackArrow = /^[\s↩↑⮐⤴]+$|^(back|return)$/i.test(text)
            const pointsBack = !!(href && sourcePath && decodeURIComponent(href.split('#')[0]).endsWith(sourcePath))
            const isMarker = text.length <= 6 && /^[\s↩↑⮐⤴\[\]()0-9*†‡§¶.]+$/.test(text)
            if (isBackArrow || (pointsBack && isMarker)) {
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
        view.srcdoc = `<!doctype html><html dir="${state.rtl ? 'rtl' : 'ltr'}"><head><base href="${escapeAttr(url.origin + url.pathname)}"><style>${noteCss()}</style></head><body>${html}</body></html>`
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
        view.addEventListener('load', async () => {
            const content = view.contentDocument
            if (!content || state.note !== note)
                return
            try { await content.fonts.ready } catch (e) { }
            if (state.note !== note)
                return
            const height = Math.min(window.innerHeight * 0.45, content.documentElement.scrollHeight + 2)
            view.style.height = height + 'px'
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

    function captionFor(media) {
        const figure = media.closest ? media.closest('figure') : null
        const figcap = figure ? figure.querySelector('figcaption') : null
        const figText = figcap ? figcap.textContent.replace(/\s+/g, ' ').trim() : ''
        const alt = (media.getAttribute('alt') || media.getAttribute('aria-label') || media.getAttribute('title') || '').trim()
        return figText || alt
    }

    function toBase64Utf8(text) {
        const bytes = new TextEncoder().encode(text)
        let binary = ''
        for (let i = 0; i < bytes.length; i += 0x8000)
            binary += String.fromCharCode(...bytes.subarray(i, i + 0x8000))
        return btoa(binary)
    }

    function extractImage(media) {
        const rect = media.getBoundingClientRect()
        const caption = captionFor(media)
        if (media.localName === 'svg') {
            const inner = media.querySelector('image')
            if (inner) {
                const href = inner.getAttribute('href') || inner.getAttributeNS(XLINK, 'href') || ''
                if (href) {
                    try {
                        return { url: new URL(href, media.ownerDocument.baseURI).href, caption, session: sessionId() }
                    } catch (e) { }
                }
            }
            if (rect.width < 32 && rect.height < 32)
                return null
            const clone = media.cloneNode(true)
            clone.setAttribute('xmlns', 'http://www.w3.org/2000/svg')
            if (!clone.getAttribute('viewBox') && rect.width > 0 && rect.height > 0)
                clone.setAttribute('viewBox', `0 0 ${Math.round(rect.width)} ${Math.round(rect.height)}`)
            if (!clone.getAttribute('width') || clone.getAttribute('width').endsWith('%'))
                clone.setAttribute('width', String(Math.max(120, Math.round(rect.width || 600))))
            if (!clone.getAttribute('height') || clone.getAttribute('height').endsWith('%'))
                clone.setAttribute('height', String(Math.max(120, Math.round(rect.height || 600))))
            const xml = new XMLSerializer().serializeToString(clone)
            return { dataUrl: 'data:image/svg+xml;base64,' + toBase64Utf8(xml), caption, session: sessionId() }
        }
        if (media.localName === 'img') {
            if (rect.width < 32 && rect.height < 32 && (media.naturalWidth || 0) < 48 && (media.naturalHeight || 0) < 48)
                return null
            const url = media.currentSrc || media.src || ''
            if (!url)
                return null
            return { url, caption, session: sessionId() }
        }
        return null
    }

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
        const mark = markAtPoint(doc, event.clientX, event.clientY)
        if (mark) {
            event.preventDefault()
            const range = rangeFor(mark.start, mark.end)
            let rect = { left: event.clientX, top: event.clientY, width: 0, height: 0 }
            if (range) {
                const rects = [...range.getClientRects()].filter(r => r.width > 0 && r.height > 0)
                rect = rects.find(r => event.clientX >= r.left - 4 && event.clientX <= r.right + 4
                                    && event.clientY >= r.top - 4 && event.clientY <= r.bottom + 4)
                    || rects[0] || range.getBoundingClientRect()
            }
            const frameRect = frame().getBoundingClientRect()
            if (state.bridge)
                state.bridge.markClicked(mark.id, { x: frameRect.left + rect.left, y: frameRect.top + rect.top,
                                                    width: rect.width, height: rect.height, session: sessionId() })
            return
        }
        const media = event.target.closest ? event.target.closest('img, svg') : null
        if (media && state.bridge && state.bridge.imageClicked) {
            const info = extractImage(media)
            if (info) {
                event.preventDefault()
                event.stopPropagation()
                state.bridge.imageClicked(info)
                return
            }
        }
        const fraction = event.clientX / state.geometry.width
        tap(fraction)
    }

    // The highlight under a point, if any.
    function markAtPoint(doc, x, y) {
        if (!state.marks || !state.marks.length)
            return null
        let node = null, index = 0
        if (doc.caretPositionFromPoint) {
            const position = doc.caretPositionFromPoint(x, y)
            if (position) {
                node = position.offsetNode
                index = position.offset
            }
        } else if (doc.caretRangeFromPoint) {
            const range = doc.caretRangeFromPoint(x, y)
            if (range) {
                node = range.startContainer
                index = range.startOffset
            }
        }
        const offset = node ? offsetOf(node, index) : null
        if (offset === null)
            return null
        return state.marks.find(span => offset >= span.start && offset < span.end) || null
    }

    function tap(fraction) {
        if (state.geometry.scroll) {
            if (state.bridge)
                state.bridge.tapped('center')
            return
        }
        // A turn the reader makes leaves the voice to read on unfollowed.
        // In RTL (and vertical-rl), the left edge advances and the right edge goes back.
        if (fraction < 0.28) {
            if (state.bridge)
                state.bridge.tapped('turn')
            state.rtl ? api.next() : api.prev()
        } else if (fraction > 0.72) {
            if (state.bridge)
                state.bridge.tapped('turn')
            state.rtl ? api.prev() : api.next()
        } else if (state.bridge) {
            state.bridge.tapped('center')
        }
    }

    let wheelTotal = 0, wheelQuietUntil = 0, wheelReset = 0
    let swipe = null, swipeEnd = 0

    // A sideways swipe on a touchpad moves the page with the fingers; when
    // they lift it settles on the next page, or back if it went only a little.
    function settleSwipe() {
        if (!swipe)
            return
        const moved = scroller().scrollLeft - swipe.from
        const rawDelta = swipe.delta
        const page = swipe.page
        swipe = null
        // At page 0 or the final page, scrollLeft clamps to the edge; fall
        // back to accumulated deltaX so swiping still crosses chapter bounds.
        const sign = state.rtl ? -1 : 1
        const effective = (Math.abs(moved) > 1 ? moved : rawDelta) * sign
        const threshold = state.geometry.step * 0.12
        const forward = effective > threshold, backward = effective < -threshold
        if (forward || backward)
            wheelQuietUntil = performance.now() + 320
        if (forward && page + 1 >= state.pages) {
            turnTo(page, true).then(() => { if (state.bridge) state.bridge.tapped('turn'); api.next() })
            return
        }
        if (backward && page === 0) {
            turnTo(page, true).then(() => { if (state.bridge) state.bridge.tapped('turn'); api.prev() })
            return
        }
        const target = forward ? page + 1 : backward ? page - 1 : page
        if (target !== page && state.bridge)
            state.bridge.tapped('turn')
        turnTo(target, true).then(() => report())
    }

    function onWheel(event) {
        if (!state.geometry)
            return
        if (!state.geometry.scroll && event.deltaMode === 0 && Math.abs(event.deltaX) > Math.abs(event.deltaY)) {
            event.preventDefault()
            if (!swipe && performance.now() < wheelQuietUntil)
                return
            const el = scroller()
            if (!swipe) {
                cancelAnimation()
                swipe = { from: el.scrollLeft, page: state.page, delta: 0 }
            }
            swipe.delta += event.deltaX
            // Never more than a page either way.
            const step = state.geometry.step
            const next = clamp(el.scrollLeft + event.deltaX, swipe.from - step, swipe.from + step)
            el.scrollTo({ left: next, behavior: 'instant' })
            clearTimeout(swipeEnd)
            swipeEnd = setTimeout(settleSwipe, 90)
            return
        }
        if (state.geometry.scroll) {
            const el = scroller()
            if (state.geometry.vertical) {
                const raw = Math.abs(event.deltaX) > Math.abs(event.deltaY)
                    ? event.deltaX : (state.rtl ? -event.deltaY : event.deltaY)
                const atStart = state.rtl ? el.scrollLeft >= -1 && raw > 0 : el.scrollLeft <= 1 && raw < 0
                const maxScroll = Math.max(0, el.scrollWidth - el.clientWidth)
                const atEnd = state.rtl ? el.scrollLeft <= -maxScroll + 2 && raw < 0 : el.scrollLeft >= maxScroll - 2 && raw > 0
                if (!atStart && !atEnd) {
                    event.preventDefault()
                    el.scrollLeft += raw
                    clearTimeout(wheelReset)
                    wheelReset = setTimeout(() => {
                        state.page = clamp(Math.floor((Math.abs(el.scrollLeft) + 1) / state.geometry.step), 0, state.pages - 1)
                        report()
                    }, 180)
                    return
                }
            } else {
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
        }
        event.preventDefault()
        const now = performance.now()
        if (now < wheelQuietUntil)
            return
        const scale = event.deltaMode === 1 ? 16 : event.deltaMode === 2 ? 400 : 1
        const horiz = Math.abs(event.deltaX) > Math.abs(event.deltaY)
        const delta = (horiz ? event.deltaX * (state.rtl ? -1 : 1) : event.deltaY) * scale
        wheelTotal += delta
        clearTimeout(wheelReset)
        wheelReset = setTimeout(() => { wheelTotal = 0 }, 200)
        if (Math.abs(wheelTotal) >= 40) {
            if (state.bridge)
                state.bridge.tapped('turn')
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
                state.bridge.selectionChanged({ session: sessionId() })
                return
            }
            const range = selection.getRangeAt(0)
            const start = offsetOf(range.startContainer, range.startOffset)
            const end = offsetOf(range.endContainer, range.endOffset)
            if (start === null || end === null || end <= start) {
                state.bridge.selectionChanged({ session: sessionId() })
                return
            }
            let rect = range.getBoundingClientRect()
            if (state.geometry && rect.width > state.geometry.column * 1.1) {
                const rects = [...range.getClientRects()].filter(r => r.width > 0 && r.height > 0 && r.right > 0 && r.left < state.geometry.width)
                if (rects.length)
                    rect = rects[0]
            }
            const frameRect = frame().getBoundingClientRect()
            state.bridge.selectionChanged({
                chapter: state.chapter,
                start, end,
                text: selection.toString().split(SHY).join(''),
                x: frameRect.left + rect.left, y: frameRect.top + rect.top,
                width: rect.width, height: rect.height,
                session: sessionId(),
            })
        }, 220)
    }

    let resizeTimer = 0
    let refreshGen = 0
    let refreshCleanupTimer = 0
    let refreshParity = false
    let paintProbe = null
    function paintProbeElement() {
        if (!paintProbe && document.body) {
            paintProbe = document.createElement('div')
            paintProbe.id = 'dg-paint-probe'
            paintProbe.setAttribute('aria-hidden', 'true')
            paintProbe.style.cssText = 'position:fixed;right:0;bottom:0;width:1px;height:1px;pointer-events:none;z-index:2147483647;'
            document.body.appendChild(paintProbe)
        }
        return paintProbe
    }
    function clearRefreshStyles() {
        clearTimeout(refreshCleanupTimer)
        refreshCleanupTimer = 0
        if (paintProbe)
            paintProbe.style.backgroundColor = ''
    }
    function scheduleResizeCheck(delay) {
        clearTimeout(resizeTimer)
        resizeTimer = setTimeout(() => {
            resizeTimer = 0
            run(relayout)
        }, delay)
    }
    function onResize() {
        const s = stage()
        if (s && state.stageWidth > 0 && state.stageHeight > 0) {
            const w = s.clientWidth, h = s.clientHeight
            if (w === state.stageWidth && h === state.stageHeight) {
                clearTimeout(resizeTimer)
                resizeTimer = 0
                return
            }
            if (isNudgeGuarded() && isNudgeDelta(w, h)) {
                const remaining = Math.max(120, Math.ceil(state.nudgeGuardUntil - performance.now()) + 20)
                scheduleResizeCheck(remaining)
                return
            }
        }
        scheduleResizeCheck(120)
    }

    async function relayout() {
        if (!state.index || !state.location)
            return
        const s = stage()
        if (s && state.stageWidth > 0 && state.stageHeight > 0) {
            const w = s.clientWidth, h = s.clientHeight
            if (w === state.stageWidth && h === state.stageHeight)
                return
            if (isNudgeGuarded() && isNudgeDelta(w, h)) {
                const remaining = Math.max(20, Math.ceil(state.nudgeGuardUntil - performance.now()) + 20)
                scheduleResizeCheck(remaining)
                return
            }
        }
        closeNote()
        const place = { chapter: state.chapter, offset: state.location.offset, snippet: state.location.snippet }
        applyLayout()
        fitBlocks(chapterDocument(), state.geometry)
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
                cancelAnimation()
                closeNote()
                hideFromHere(0)
                state.book = book
                state.style = style
                state.rtl = !!(book && book.rtl)
                state.vertical = ''
                state.history = []
                state.forward = []
                state.justJumped = false
                state.prefetched = new Set()
                state.hyphenatedNodes = new Map()
                state.chapter = -1
                state.stageWidth = 0
                state.stageHeight = 0
                state.nudgeGuardUntil = 0
                clearTimeout(resizeTimer)
                resizeTimer = 0
                state.index = null
                state.location = null
                state.decorated = -1
                state.decorations = {}
                state.marks = []
                applyPageColors()
                await show({ chapter: position.chapter || 0, offset: position.offset || 0, snippet: position.snippet || '' }, false)
            })
        },

        close() {
            return run(async () => {
                cancelAnimation()
                closeNote()
                hideFromHere(0)
                clearTimeout(selectionTimer)
                ++refreshGen
                clearRefreshStyles()
                state.book = null
                state.rtl = false
                state.vertical = ''
                state.chapter = -1
                state.stageWidth = 0
                state.stageHeight = 0
                state.nudgeGuardUntil = 0
                clearTimeout(resizeTimer)
                resizeTimer = 0
                state.index = null
                state.location = null
                state.decorated = -1
                state.decorations = {}
                state.marks = []
                state.history = []
                state.forward = []
                state.justJumped = false
                state.prefetched = new Set()
                state.hyphenatedNodes = new Map()
                const f = frame()
                f.classList.remove('shown')
                f.onload = null
                f.src = 'about:blank'
            })
        },

        setStyle(style) {
            return run(async () => {
                const prev = state.style
                state.style = style
                applyPageColors()
                if (!state.index || !state.location)
                    return
                const doc = chapterDocument()
                const layoutKeys = ['font', 'fontScale', 'fontWeight', 'lineHeight', 'paragraphSpacing',
                                    'margin', 'measure', 'justify', 'hyphenate', 'layout', 'publisher',
                                    'marginTop', 'marginBottom', 'dropCap']
                const layoutChanged = !prev || layoutKeys.some(k => prev[k] !== style[k])
                if (layoutChanged && (!prev || prev.dropCap !== style.dropCap || prev.publisher !== style.publisher))
                    markDropCap(doc)
                if (!layoutChanged) {
                    applyTypography(doc)
                    return
                }
                const place = { chapter: state.chapter, offset: state.location.offset, snippet: state.location.snippet }
                const wantHyphen = !style.publisher && style.justify && style.hyphenate
                if (wantHyphen && !state.hyphenation) {
                    state.hyphenation = await Hyphenation.load(state.book.language)
                    if (state.hyphenation) {
                        hyphenate(doc)
                        state.index = buildIndex(doc)
                        restoreDecorations()
                    }
                } else if (!wantHyphen && state.hyphenation) {
                    unhyphenate()
                    state.hyphenation = false
                    state.index = buildIndex(doc)
                    restoreDecorations()
                }
                applyTypography(doc)
                if (!prev || prev.font !== style.font || prev.publisher !== style.publisher) {
                    try { await doc.fonts.ready } catch (e) { }
                }
                applyLayout()
                fitBlocks(doc, state.geometry)
                measure()
                const page = pageOfOffset(resolveOffset(place.offset, place.snippet))
                await turnTo(page, false)
                report()
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
                if (!place)
                    return
                const current = snapshotLocation()
                if (current) {
                    state.forward.push(current)
                    if (state.forward.length > 50)
                        state.forward.shift()
                }
                state.justJumped = false
                await show(place, false)
            })
        },

        forward() {
            return run(async () => {
                const place = state.forward.pop()
                if (!place)
                    return
                const current = snapshotLocation()
                if (current) {
                    state.history.push(current)
                    if (state.history.length > 50)
                        state.history.shift()
                }
                state.justJumped = false
                await show(place, false)
            })
        },

        canGoBack() { return state.history.length > 0 },
        canGoForward() { return state.forward.length > 0 },

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
            // Kept, so the chapter laid out again (a new size, hyphens) gets
            // its highlights back.
            if (state.decorated !== state.chapter) {
                state.decorations = {}
                state.decorated = state.chapter
            }
            state.decorations[name] = spans || []
            const view = frame().contentWindow
            if (!state.index || !view || !view.CSS || !view.CSS.highlights)
                return
            const ranges = (spans || []).map(span => rangeFor(span.start, span.end)).filter(Boolean)
            if (ranges.length) {
                const highlight = new view.Highlight(...ranges)
                // The hit gone to shows over the others, and over the voice.
                highlight.priority = name === 'dg-found' ? 2 : name === 'dg-speaking' ? 1 : 0
                view.CSS.highlights.set(name, highlight)
            } else {
                view.CSS.highlights.delete(name)
            }
        },

        // The chapter's highlights, each in its color; those with notes are
        // underlined too. A click on one is reported, not taken as a turn.
        marks(spans) {
            state.marks = spans || []
            for (const color of ['yellow', 'green', 'blue', 'pink'])
                api.highlight('dg-mark-' + color, state.marks.filter(span => span.color === color))
            api.highlight('dg-noted', state.marks.filter(span => span.noted))
            scheduleMarginNotes()
        },

        clearSelection() {
            const selection = state.index ? chapterDocument().getSelection() : null
            if (selection)
                selection.removeAllRanges()
        },

        // Search hits in the chapter shown, and the one gone to.
        found(spans, current) {
            api.highlight('dg-search', spans)
            api.highlight('dg-found', current ? [current] : [])
        },

        // The blocks to read aloud from a chapter, reported to the bridge
        // when they're ready (the window can't wait on a promise).
        requestSpeakable(chapter) {
            const sid = sessionId()
            const wanted = chapter === undefined || chapter === null || chapter < 0 ? state.chapter : chapter
            const deliver = blocks => {
                if (sid !== sessionId() || !state.bridge)
                    return
                if (state.bridge.speakableReadyFor)
                    state.bridge.speakableReadyFor(sid, wanted, blocks)
                else
                    state.bridge.speakableReady(wanted, blocks)
            }
            blocksOfChapter(wanted)
                .then(blocks => deliver(JSON.stringify(blocks)))
                .catch(error => {
                    log(error && error.stack || error)
                    deliver('[]')
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
                    report(true)
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

        placeAt(x, y) { return placeAt(x, y) },

        // Guards ResizeObserver/relayout against the transient 1 px surface
        // recovery nudge from EpubReader.qml so returning from a workspace
        // never reflows text lines, while genuine window resizes still relayout.
        beginSurfaceNudge(maxMs = 1000) {
            const parsed = Number(maxMs)
            const duration = Number.isFinite(parsed) ? clamp(parsed, 100, 3000) : 1000
            const now = performance.now()
            state.nudgeGuardUntil = Math.max(state.nudgeGuardUntil, now + duration)
            const s = stage()
            if (resizeTimer && s && isNudgeDelta(s.clientWidth, s.clientHeight)) {
                const remaining = Math.max(120, Math.ceil(state.nudgeGuardUntil - now) + 20)
                scheduleResizeCheck(remaining)
            }
            return true
        },

        endSurfaceNudge(settleMs = 450) {
            const parsed = Number(settleMs)
            const settle = Number.isFinite(parsed) ? clamp(parsed, 0, 2000) : 450
            const now = performance.now()
            const target = now + settle
            state.nudgeGuardUntil = state.nudgeGuardUntil > now ? Math.min(state.nudgeGuardUntil, target) : target
            const s = stage()
            if (s && state.stageWidth > 0 && state.stageHeight > 0) {
                const w = s.clientWidth, h = s.clientHeight
                if (w === state.stageWidth && h === state.stageHeight) {
                    clearTimeout(resizeTimer)
                    resizeTimer = 0
                } else if (resizeTimer && isNudgeDelta(w, h)) {
                    const remaining = Math.max(20, Math.ceil(state.nudgeGuardUntil - now) + 20)
                    scheduleResizeCheck(remaining)
                }
            }
            return true
        },

        // The window is back in view: force Blink and cc's damage tracker to
        // submit a real CompositorFrame (a transparent outline is elided as
        // zero-damage) using a 1x1 corner sentinel inside the outer reading
        // margin. Never toggles opacity or transform on the page or chapter
        // iframe, keeping subpixel glyph rendering and line positions locked.
        refresh() {
            const gen = ++refreshGen
            clearTimeout(refreshCleanupTimer)
            const probe = paintProbeElement()
            if (probe) {
                refreshParity = !refreshParity
                probe.style.backgroundColor = refreshParity
                    ? 'rgba(0, 0, 0, 0.008)'
                    : 'rgba(255, 255, 255, 0.008)'
            }
            const restore = () => {
                if (gen !== refreshGen)
                    return
                clearRefreshStyles()
            }
            requestAnimationFrame(() => requestAnimationFrame(restore))
            refreshCleanupTimer = setTimeout(restore, 120)
            return true
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
