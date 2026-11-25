import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "components"
import "library"

import "components/Format.js" as Format
import "components/Icons.js" as Icons
import "components/Themes.js" as Themes
import "components/Ui.js" as Ui

// Go anywhere (Ctrl+K): 680 px spotlight palette with Everything / This book
// scope switcher (Tab), cross-book full-text and meaning search with Literata
// italic quotes, highlights & notes, and query-aware Do actions (07_command_palette).
FocusScope {
    id: cmdPalette

    property bool open: false
    property bool reading: false
    property string currentTheme: "paper"
    property string viewMode: "grid"
    property string scope: "all"

    signal openBook(int bookId)
    signal jumpChapter(int chapter, string fragment)
    signal jumpMark(int bookId, int chapter, var offset)
    signal jumpTarget(var target)
    signal triggerAction(string actionId)
    signal askCompanion(string question)
    signal closed()

    property var results: []
    property var cachedBooks: []
    property var cachedMarks: []
    property int selectedIndex: 0

    readonly property int scopedBookId: reading ? reader.bookId : library.lastOpenedBook()
    readonly property bool canScopeBook: scopedBookId > 0
    readonly property string scopedBookTitle: {
        if (reading && reader.title !== "")
            return reader.title
        if (scopedBookId > 0) {
            const b = library.book(scopedBookId)
            return b.title || ""
        }
        return ""
    }
    readonly property string refreshIcon: "M19 12 A7 7 0 1 1 16.95 7.05 M19 4.5 V8.5 H15"
    readonly property var swatches: Themes.swatches

    function show() {
        cachedBooks = library.allBooks()
        cachedMarks = marks.searchAll("", 30)
        scope = "all"
        meaningQuery = ""
        input.text = ""
        open = true
        rebuild()
        input.forceActiveFocus()
    }
    // Opens with something already typed, as from the library's search field.
    function search(text) {
        show()
        input.text = text
        input.cursorPosition = text.length
        rebuild()
        meaningDebounce.restart()
    }

    function close() {
        if (!open)
            return
        open = false
        searchDebounce.stop()
        meaningDebounce.stop()
        cachedBooks = []
        cachedMarks = []
        closed()
    }

    function toggleScope() {
        if (!canScopeBook)
            return
        scope = scope === "all" ? "book" : "all"
        rebuild()
    }

    function chapterLabel(book, chapterIdx) {
        if (reading && book && book.id === reader.bookId) {
            const title = reader.titleOfChapter(chapterIdx)
            if (title !== "")
                return title
        }
        if (book && book.format === "pdf")
            return qsTr("Page %1").arg(chapterIdx + 1)
        return qsTr("Chapter %1").arg(chapterIdx + 1)
    }

    function norm(s) {
        if (!s)
            return ""
        return String(s).normalize("NFD").replace(/[\u0300-\u036f]/g, "").toLowerCase()
    }

    // Scores `query` against `primary` and `secondary`; returns -1 if no match.
    function fuzzyScore(rawQuery, primary, secondary) {
        const q = norm(rawQuery).trim()
        if (q === "")
            return 1
        const p = norm(primary)
        const s = norm(secondary)
        const full = s !== "" ? (p + " " + s) : p

        const pIdx = p.indexOf(q)
        if (pIdx === 0)
            return 1000 - p.length * 0.2
        if (pIdx > 0) {
            const boundary = p.charAt(pIdx - 1) === " " || p.charAt(pIdx - 1) === ":" || p.charAt(pIdx - 1) === "-"
            return (boundary ? 860 : 720) - pIdx
        }
        const sIdx = s.indexOf(q)
        if (sIdx >= 0)
            return (sIdx === 0 ? 640 : 540) - sIdx * 0.5

        const tokens = q.split(/\s+/).filter(t => t.length > 0)
        if (tokens.length > 1) {
            let tokenScore = 0
            let allHit = true
            for (let i = 0; i < tokens.length; ++i) {
                const tp = p.indexOf(tokens[i])
                const ts = s.indexOf(tokens[i])
                if (tp >= 0)
                    tokenScore += (tp === 0 ? 180 : 120)
                else if (ts >= 0)
                    tokenScore += 70
                else {
                    allHit = false
                    break
                }
            }
            if (allHit)
                return 320 + tokenScore
        }

        let qi = 0
        let score = 0
        let consecutive = 0
        for (let i = 0; i < full.length && qi < q.length; ++i) {
            if (full.charAt(i) === q.charAt(qi)) {
                const isStart = i === 0 || full.charAt(i - 1) === " " || full.charAt(i - 1) === ":"
                consecutive += 1
                score += (i < p.length ? 16 : 8) + consecutive * 6 + (isStart ? 24 : 0)
                qi += 1
            } else {
                consecutive = 0
            }
        }
        return qi === q.length ? score : -1
    }

    function voiceSubtitle() {
        const name = narration.voiceName !== "" ? narration.voiceName : qsTr("Read aloud")
        const isOffline = narration.voice.startsWith("builtin:") || narration.voice.startsWith("studio:")
        const where = isOffline ? qsTr("on this computer") : qsTr("Gemini")
        return narration.voiceName !== "" ? (name + " · " + where) : name
    }

    function buildActions() {
        const list = []
        if (reading) {
            list.push({ id: "search-book", title: qsTr("Search in book"), subtitle: qsTr("Find words or search by meaning"), hint: "/", glyph: Icons.search })
            list.push({ id: "go-to", title: qsTr("Go to page or percentage…"), subtitle: reader.hasPageList ? qsTr("Jump by printed page or %") : qsTr("Jump by percentage"), hint: "G", glyph: Icons.page })
            list.push({ id: "contents", title: qsTr("Table of contents"), subtitle: reader.title, hint: "T", glyph: Icons.contents })
            list.push({ id: "marks", title: qsTr("Highlights, notes & bookmarks"), subtitle: qsTr("%Ln marks", "", marks.count), hint: "M", glyph: Icons.pen })
            list.push({ id: "bookmark", title: qsTr("Toggle bookmark"), subtitle: reader.chapterTitle || reader.title, hint: "B", glyph: Icons.bookmark })
            list.push({ id: "companion-ask", title: qsTr("Companion: Ask about this book"), subtitle: qsTr("Spoiler-free reading companion"), hint: "C", glyph: Icons.sparkle })
            list.push({ id: "companion-prev", title: qsTr("Companion: Previously…"), subtitle: qsTr("Recap the story up to your page"), hint: "", glyph: Icons.sparkle })
            list.push({ id: "companion-people", title: qsTr("Companion: Who's who"), subtitle: qsTr("Characters and places so far"), hint: "", glyph: Icons.sparkle })
            list.push({ id: "voices", title: qsTr("Voice & narration panel"), subtitle: qsTr("Narrator, style, full cast, audiobook"), hint: "V", glyph: Icons.headphones })
            list.push({ id: "listen", title: narration.active ? qsTr("Stop listening") : qsTr("Listen from here"), subtitle: voiceSubtitle(), hint: "P", glyph: Icons.headphones })
            list.push({ id: "appearance", title: qsTr("Typography & appearance"), subtitle: qsTr("Typeface, spacing, margins, layout"), hint: "A", glyph: Icons.book })
            if (reader.format === "pdf") {
                list.push({ id: "pdf-fit-page", title: qsTr("PDF: Fit page"), subtitle: qsTr("Show the whole page inside the window"), hint: "0", glyph: Icons.page })
                list.push({ id: "pdf-fit-width", title: qsTr("PDF: Fit width"), subtitle: qsTr("Fill the reading stage width and scroll"), hint: "W", glyph: Icons.columns })
                list.push({ id: "pdf-fit-actual", title: qsTr("PDF: Actual size"), subtitle: qsTr("True 1:1 print scale"), hint: "1", glyph: Icons.zoomIn })
            }
            list.push({ id: "toggle-theme", title: qsTr("Cycle reading theme"), subtitle: Themes.name(currentTheme), hint: "D", glyph: Icons.sun })
            if (marks.count > 0) {
                list.push({ id: "export-notes-file", title: qsTr("Export notes to Markdown file"), subtitle: qsTr("Obsidian & Readwise format"), hint: "", glyph: Icons.download })
                list.push({ id: "copy-notes", title: qsTr("Copy notes as Markdown"), subtitle: qsTr("Copy all marks to clipboard"), hint: "", glyph: Icons.copy })
            }
            list.push({ id: "add-url", title: qsTr("Add from URL"), subtitle: qsTr("Save a web article, arXiv paper, EPUB, or PDF"), hint: "Ctrl ⇧ U", glyph: Icons.link })
            list.push({ id: "browse-catalogs", title: qsTr("Browse catalogs"), subtitle: qsTr("Public OPDS catalogs and your servers"), hint: "Ctrl ⇧ O", glyph: Icons.globe })
            list.push({ id: "close-book", title: qsTr("Back to library"), subtitle: qsTr("Close current book"), hint: "Esc", glyph: Icons.back })
        } else {
            if (library.lastOpenedBook() > 0)
                list.push({ id: "continue-reading", title: qsTr("Continue reading"), subtitle: qsTr("Resume your last opened book"), hint: "Space", glyph: Icons.book })
            list.push({ id: "add-folder", title: qsTr("Add folder of books…"), subtitle: qsTr("Watch a folder for EPUB & PDF books"), hint: "A", glyph: Icons.folder })
            list.push({ id: "add-books", title: qsTr("Add individual books…"), subtitle: qsTr("Pick EPUB or PDF files"), hint: "", glyph: Icons.plus })
            list.push({ id: "add-url", title: qsTr("Add from URL"), subtitle: qsTr("Save a web article, arXiv paper, EPUB, or PDF"), hint: "Ctrl ⇧ U", glyph: Icons.link })
            list.push({ id: "browse-catalogs", title: qsTr("Browse catalogs"), subtitle: qsTr("Public OPDS catalogs and your servers"), hint: "Ctrl ⇧ O", glyph: Icons.globe })
            list.push({ id: "manage-folders", title: qsTr("Manage watched folders…"), subtitle: qsTr("%1 watched").arg(library.folders.length), hint: "", glyph: Icons.folder })
            list.push({ id: "rescan", title: qsTr("Rescan library"), subtitle: qsTr("Check watched folders for new or moved books"), hint: "", glyph: refreshIcon })
            list.push({ id: "toggle-view", title: viewMode === "grid" ? qsTr("Switch to list view") : qsTr("Switch to cover grid"), subtitle: qsTr("Library layout"), hint: "V", glyph: viewMode === "grid" ? Icons.list : Icons.grid })
            list.push({ id: "cycle-sort", title: qsTr("Cycle sort order"), subtitle: qsTr("Recent, title, author, series, added, progress"), hint: "S", glyph: Icons.sort })
            list.push({ id: "toggle-grouping", title: library.works.grouped ? qsTr("Show every edition separately") : qsTr("Group editions together"), subtitle: qsTr("Edition grouping"), hint: "G", glyph: Icons.books })
            list.push({ id: "toggle-theme", title: qsTr("Cycle reading theme"), subtitle: Themes.name(currentTheme), hint: "", glyph: Icons.sun })
        }
        list.push({ id: "stats", title: qsTr("Reading stats & insights"), subtitle: qsTr("Reading time, streak, and finished books"), hint: "", glyph: Icons.stats })
        list.push({ id: "settings", title: qsTr("Settings"), subtitle: qsTr("General, reading, voices, intelligence"), hint: "Ctrl ,", glyph: Icons.gear })
        list.push({ id: "ai-settings", title: qsTr("Intelligence settings"), subtitle: qsTr("Gemini, local models, budget"), hint: "", glyph: Icons.sparkle })
        list.push({ id: "fullscreen", title: qsTr("Toggle full screen"), subtitle: "", hint: "F", glyph: Icons.fullscreen })
        list.push({ id: "help", title: qsTr("Keyboard shortcuts"), subtitle: qsTr("View all keys"), hint: "?", glyph: Icons.keyboard })
        list.push({ id: "quit", title: qsTr("Quit Clothbound"), subtitle: "", hint: "Q", glyph: Icons.close })
        return list
    }

    function appendActions(out, mode, q) {
        const actions = buildActions()
        const matchedActions = []
        for (let i = 0; i < actions.length; ++i) {
            const a = actions[i]
            const score = fuzzyScore(q, a.title, a.subtitle + " " + a.id)
            if (score >= 0)
                matchedActions.push({ score: score, order: i, item: a })
        }
        if (q !== "")
            matchedActions.sort((a, b) => b.score - a.score || a.order - b.order)

        // Dynamic query-aware Do actions (07_command_palette.html).
        let hasListen = false
        if (mode === "all" && q.length >= 2 && scopedBookId > 0) {
            out.push({
                kind: "ask",
                section: qsTr("Do"),
                question: q,
                title: qsTr("Ask the companion: “%1”").arg(q),
                subtitle: scopedBookTitle !== ""
                          ? qsTr("About %1, up to your page").arg(scopedBookTitle)
                          : qsTr("About your book, up to your page"),
                hint: "C",
                glyph: Icons.sparkle,
                quote: false,
            })
        }

        const actionCap = mode === "actions" ? 25 : (q === "" ? 7 : 4)
        for (let i = 0; i < Math.min(actionCap, matchedActions.length); ++i) {
            const a = matchedActions[i].item
            if (a.id === "listen")
                hasListen = true
            out.push({
                kind: "action",
                section: qsTr("Do"),
                actionId: a.id,
                title: a.title,
                subtitle: a.subtitle,
                hint: a.hint,
                glyph: a.glyph,
                quote: false,
            })
        }

        if (mode === "all" && q.length >= 2 && scopedBookId > 0 && !hasListen) {
            out.push({
                kind: "action",
                section: qsTr("Do"),
                actionId: "listen",
                title: qsTr("Listen from here"),
                subtitle: voiceSubtitle(),
                hint: "P",
                glyph: Icons.headphones,
                quote: false,
            })
        }
    }

    function rebuild() {
        const raw = input.text.trim()
        let mode = "all"
        let q = raw
        if (raw.startsWith(">")) {
            mode = "actions"
            q = raw.slice(1).trim()
        } else if (raw.startsWith("#")) {
            mode = "chapters"
            q = raw.slice(1).trim()
        } else if (raw.startsWith("@") || raw.startsWith("\"")) {
            mode = "marks"
            q = raw.slice(1).trim()
        }

        const bookOnly = scope === "book" && scopedBookId > 0
        const out = []

        // Direct jump when reading and user types a percentage or printed page.
        if (reading && mode === "all" && q !== "") {
            const pctMatch = q.match(/^(\d+(?:[.,]\d+)?)\s*%$/)
            if (pctMatch) {
                const frac = Math.max(0, Math.min(1, Number(pctMatch[1].replace(",", ".")) / 100))
                const target = reader.targetForFraction(frac)
                if (target && target.chapter !== undefined) {
                    out.push({
                        kind: "target",
                        section: qsTr("Jump"),
                        title: qsTr("Go to %1%").arg(pctMatch[1]),
                        subtitle: reader.titleOfChapter(target.chapter) || reader.title,
                        hint: "",
                        glyph: Icons.page,
                        target: target,
                        quote: false,
                    })
                }
            } else if (reader.hasPageList && /^(?:p(?:age)?\.?\s*)?([ivxlcdm]+|\d+)$/i.test(q)) {
                const lbl = q.replace(/^p(?:age)?\.?\s*/i, "")
                const target = reader.targetForPageLabel(lbl)
                if (target && target.chapter !== undefined) {
                    out.push({
                        kind: "target",
                        section: qsTr("Jump"),
                        title: qsTr("Go to page %1").arg(lbl),
                        subtitle: reader.titleOfChapter(target.chapter) || reader.title,
                        hint: "",
                        glyph: Icons.page,
                        target: target,
                        quote: false,
                    })
                }
            }
        }

        // Empty query or explicit `>` puts Do actions first; active queries
        // prioritize Books / Passages / Meaning ahead of Do (07_command_palette).
        if (mode === "actions" || (mode === "all" && q === ""))
            appendActions(out, mode, q)

        // Current book TOC chapters when reading.
        if (reading && (mode === "all" || mode === "chapters")) {
            const toc = reader.contents || []
            const matchedChapters = []
            for (let i = 0; i < toc.length; ++i) {
                const entry = toc[i]
                if (!entry || entry.chapter < 0)
                    continue
                const score = fuzzyScore(q, entry.title, "")
                if (score >= 0)
                    matchedChapters.push({ score: score, order: i, entry: entry })
            }
            if (q !== "")
                matchedChapters.sort((a, b) => b.score - a.score || a.order - b.order)
            const chapCap = mode === "chapters" ? 40 : (q === "" ? 5 : 6)
            for (let i = 0; i < Math.min(chapCap, matchedChapters.length); ++i) {
                const entry = matchedChapters[i].entry
                out.push({
                    kind: "chapter",
                    section: qsTr("Chapters"),
                    title: entry.title,
                    subtitle: entry.chapter === reader.chapter ? qsTr("Current chapter") : reader.title,
                    hint: "",
                    glyph: Icons.contents,
                    chapter: entry.chapter,
                    fragment: entry.fragment || "",
                    depth: entry.depth || 0,
                    quote: false,
                })
            }
        }

        // Books in library (skipped when scoped to "This book").
        if (mode === "all" && !bookOnly) {
            const matchedBooks = []
            for (let i = 0; i < cachedBooks.length; ++i) {
                const b = cachedBooks[i]
                const meta = [b.author, b.series, b.format, b.editionLine].filter(x => !!x).join(" ")
                const score = fuzzyScore(q, b.title, meta)
                if (score >= 0)
                    matchedBooks.push({ score: score, order: i, book: b })
            }
            if (q !== "")
                matchedBooks.sort((a, b) => b.score - a.score || a.order - b.order)
            const bookCap = q === "" ? 6 : 6
            for (let i = 0; i < Math.min(bookCap, matchedBooks.length); ++i) {
                const b = matchedBooks[i].book
                const parts = []
                if (b.author)
                    parts.push(b.author)
                if (b.series)
                    parts.push(b.seriesIndex > 0 ? b.series + " #" + b.seriesIndex : b.series)
                if (b.finished)
                    parts.push(qsTr("Finished"))
                else if (b.progress > 0.005)
                    parts.push(qsTr("%1%").arg(Math.round(b.progress * 100).toLocaleString(Qt.locale(), "f", 0)))
                out.push({
                    kind: "book",
                    section: qsTr("Books"),
                    bookId: b.id,
                    title: b.title,
                    subtitle: parts.join(" · "),
                    hint: (b.format || "").toUpperCase(),
                    glyph: Icons.book,
                    cover: b.cover || "",
                    author: b.author || "",
                    bookTitle: b.title || "",
                    quote: false,
                })
            }
        }

        // Full-text passages from the library index ("In your books" / "In this book").
        if (mode === "all" && q.length >= 3 && libraryIndex.available) {
            const found = libraryIndex.search(q, 6, bookOnly ? scopedBookId : 0)
            for (const hit of found) {
                const book = library.book(hit.bookId)
                if (!book.id)
                    continue
                const snip = (hit.snippet || "").replace(/[‹›]/g, "")
                out.push({
                    kind: "text",
                    section: bookOnly ? qsTr("In this book") : qsTr("In your books"),
                    bookId: hit.bookId,
                    chapter: hit.chapter,
                    offset: hit.offset,
                    title: book.title + " · " + chapterLabel(book, hit.chapter),
                    subtitle: snip !== "" ? qsTr("“%1”").arg(snip) : "",
                    hint: "",
                    glyph: Icons.search,
                    cover: book.cover || "",
                    author: book.author || "",
                    bookTitle: book.title || "",
                    quote: true,
                })
            }
        }

        // Passages close in meaning ("By meaning").
        if (mode === "all" && q.length >= 3 && q === meaningQuery) {
            const shown = out.filter(r => r.kind === "text").map(r => r.bookId + ":" + r.chapter)
            for (const hit of bookSearch.libraryResults) {
                if (bookOnly && hit.bookId !== scopedBookId)
                    continue
                if (shown.indexOf(hit.bookId + ":" + hit.chapter) >= 0)
                    continue
                const book = library.book(hit.bookId)
                const text = libraryIndex.passage(hit.bookId, hit.chapter, hit.start, Math.min(hit.end, hit.start + 160))
                if (!book.id || text === "")
                    continue
                out.push({
                    kind: "text",
                    section: qsTr("By meaning"),
                    bookId: hit.bookId,
                    chapter: hit.chapter,
                    offset: hit.start,
                    title: book.title + " · " + chapterLabel(book, hit.chapter),
                    subtitle: qsTr("“%1…”").arg(text),
                    hint: "",
                    glyph: Icons.sparkle,
                    cover: book.cover || "",
                    author: book.author || "",
                    bookTitle: book.title || "",
                    quote: true,
                })
            }
        }

        // Cross-book or single-book highlights and notes.
        if (mode === "all" || mode === "marks") {
            const rawHits = q === "" ? cachedMarks : marks.searchAll(q, mode === "marks" ? 30 : 12)
            const hits = bookOnly ? rawHits.filter(m => m.bookId === scopedBookId) : rawHits
            const markCap = mode === "marks" ? 30 : (q === "" ? 4 : 6)
            for (let i = 0; i < Math.min(markCap, hits.length); ++i) {
                const m = hits[i]
                const subParts = []
                if (m.bookTitle)
                    subParts.push(m.bookTitle)
                if (m.label)
                    subParts.push(m.label)
                if (m.note && m.note !== "" && m.text)
                    subParts.push("“" + m.text.slice(0, 60) + (m.text.length > 60 ? "…" : "") + "”")
                out.push({
                    kind: "mark",
                    section: qsTr("Highlights & notes"),
                    bookId: m.bookId,
                    chapter: m.chapter,
                    offset: m.start,
                    markKind: m.kind,
                    color: m.color || "yellow",
                    title: m.kind === "bookmark" ? m.text : (m.note !== "" ? m.note : "“" + m.text + "”"),
                    subtitle: subParts.join(" · "),
                    hint: "",
                    glyph: m.kind === "bookmark" ? Icons.bookmark : Icons.pen,
                    quote: false,
                })
            }
        }

        // For non-empty general queries, place contextual Do actions at the foot.
        if (mode === "all" && q !== "")
            appendActions(out, mode, q)

        results = out
        selectedIndex = out.length > 0 ? 0 : -1
        list.currentIndex = selectedIndex
    }

    function activate(idx) {
        if (idx < 0 || idx >= results.length)
            return
        const item = results[idx]
        close()
        if (item.kind === "ask")
            askCompanion(item.question)
        else if (item.kind === "action")
            triggerAction(item.actionId)
        else if (item.kind === "book")
            openBook(item.bookId)
        else if (item.kind === "chapter")
            jumpChapter(item.chapter, item.fragment)
        else if (item.kind === "mark" || item.kind === "text")
            jumpMark(item.bookId, item.chapter, item.offset)
        else if (item.kind === "target")
            jumpTarget(item.target)
    }

    function moveSelection(delta) {
        if (results.length === 0)
            return
        selectedIndex = (selectedIndex + delta + results.length) % results.length
        list.currentIndex = selectedIndex
        list.positionViewAtIndex(selectedIndex, ListView.Contain)
    }

    Timer {
        id: searchDebounce
        interval: 45
        onTriggered: { cmdPalette.rebuild(); meaningDebounce.restart() }
    }
    // Search by meaning waits briefly for a pause in typing.
    property string meaningQuery: ""
    Timer {
        id: meaningDebounce
        interval: 450
        onTriggered: {
            const q = input.text.trim()
            if (q.length >= 3 && !/^[>#@"]/.test(q) && bookSearch.canSearchByMeaning && bookSearch.booksPrepared > 0) {
                cmdPalette.meaningQuery = q
                bookSearch.searchLibrary(q)
            }
        }
    }
    Connections {
        target: bookSearch
        function onLibraryResultsChanged() { if (cmdPalette.open) cmdPalette.rebuild() }
    }

    visible: opacity > 0
    onVisibleChanged: {
        if (!visible && !open)
            results = []
    }
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }

    // Claim Escape / arrows / Return / Tab / Ctrl+K ahead of ApplicationShortcuts.
    Keys.onShortcutOverride: (event) => {
        if (!cmdPalette.open)
            return
        if (event.key === Qt.Key_Escape || event.key === Qt.Key_Up || event.key === Qt.Key_Down ||
            event.key === Qt.Key_Return || event.key === Qt.Key_Enter || event.key === Qt.Key_Tab ||
            event.key === Qt.Key_Backtab ||
            ((event.modifiers & Qt.ControlModifier) && (event.key === Qt.Key_K || event.key === Qt.Key_N || event.key === Qt.Key_P))) {
            event.accepted = true
        }
    }
    Keys.onPressed: (event) => {
        if (!cmdPalette.open)
            return
        if (event.key === Qt.Key_Escape || ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_K)) {
            cmdPalette.close()
            event.accepted = true
        } else if (event.key === Qt.Key_Tab || event.key === Qt.Key_Backtab) {
            if (cmdPalette.canScopeBook)
                cmdPalette.toggleScope()
            else
                cmdPalette.moveSelection(event.key === Qt.Key_Tab ? 1 : -1)
            event.accepted = true
        } else if (event.key === Qt.Key_Down || ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_N)) {
            cmdPalette.moveSelection(1)
            event.accepted = true
        } else if (event.key === Qt.Key_Up || ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_P)) {
            cmdPalette.moveSelection(-1)
            event.accepted = true
        } else if (event.key === Qt.Key_Return || event.key === Qt.Key_Enter) {
            cmdPalette.activate(cmdPalette.selectedIndex)
            event.accepted = true
        }
    }

    // Scrim
    Rectangle {
        anchors.fill: parent
        color: theme.scrim
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onPressed: cmdPalette.close()
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    // 680 px Spotlight dialog card with soft elevation shadow.
    Item {
        id: cardFrame
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.max(44, Math.min(112, parent.height * 0.14))
        width: Math.min(parent.width - 48, 680)
        height: Math.min(parent.height - y - 40, header.height + 1 + bodyHeight + footer.height + 1)
        readonly property real bodyHeight: cmdPalette.results.length > 0
            ? Math.min(Math.max(list.contentHeight, 60), Math.max(220, cmdPalette.height * 0.54))
            : 84
        scale: cmdPalette.open || theme.reducedMotion ? 1.0 : 0.98
        Behavior on scale {
            enabled: !theme.reducedMotion
            NumberAnimation { duration: 160; easing.type: Easing.OutQuart }
        }

        // Deep floating shadow (0 30px 90px rgba(0,0,0,0.6)).
        Rectangle {
            anchors.fill: parent
            anchors.topMargin: 18
            anchors.margins: -8
            radius: card.radius > 0 ? card.radius + 8 : 0
            color: theme.shadow
            opacity: 0.55
        }

        Rectangle {
            id: card
            anchors.fill: parent
            radius: Ui.radius(14, theme.radius)
            color: theme.raised
            border.width: 1
            border.color: Ui.tint(theme.ink1, 0.10)
            clip: true

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                onPressed: input.forceActiveFocus()
                onWheel: (wheel) => wheel.accepted = true
            }

            // Top 56 px search input row with Everything | This book scope tabs.
            RowLayout {
                id: header
                anchors.top: parent.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 56
                spacing: 12

                Glyph {
                    Layout.leftMargin: 18
                    Layout.preferredWidth: 18
                    Layout.preferredHeight: 18
                    stroke: 2
                    path: Icons.search
                    color: theme.ink3
                }

                TextInput {
                    id: input
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    color: theme.ink1
                    selectionColor: Ui.tint(theme.accent, 0.35)
                    selectedTextColor: theme.ink1
                    font.pixelSize: 17
                    clip: true
                    selectByMouse: true
                    Accessible.role: Accessible.EditableText
                    Accessible.name: qsTr("Search everything")
                    onTextEdited: searchDebounce.restart()

                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        width: parent.width
                        visible: input.text === ""
                        text: cmdPalette.reading
                            ? qsTr("Jump to book, passage, chapter, or action…")
                            : qsTr("Search books, passages, notes, or actions…")
                        color: theme.ink4
                        font.pixelSize: 17
                        elide: Text.ElideRight
                    }
                }

                IconButton {
                    visible: input.text !== ""
                    Layout.preferredWidth: 26
                    Layout.preferredHeight: 26
                    tabFocusable: false
                    glyph: Icons.close
                    tone: theme.ink3
                    tip: qsTr("Clear")
                    onClicked: {
                        input.text = ""
                        cmdPalette.rebuild()
                        input.forceActiveFocus()
                    }
                }

                Segmented {
                    visible: cmdPalette.canScopeBook
                    Layout.rightMargin: 16
                    compact: true
                    model: [
                        { value: "all", label: qsTr("Everything") },
                        { value: "book", label: qsTr("This book") }
                    ]
                    current: cmdPalette.scope
                    onActivated: (v) => {
                        cmdPalette.scope = v
                        cmdPalette.rebuild()
                        input.forceActiveFocus()
                    }
                }
            }

            Rectangle {
                id: topDivider
                anchors.top: header.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: Ui.tint(theme.ink1, 0.07)
            }

            // Empty query-no-match state
            Label {
                anchors.top: topDivider.bottom
                anchors.bottom: bottomDivider.top
                anchors.left: parent.left
                anchors.right: parent.right
                horizontalAlignment: Text.AlignHCenter
                verticalAlignment: Text.AlignVCenter
                visible: cmdPalette.results.length === 0
                text: qsTr("No matching books, passages, or actions")
                color: theme.ink3
                font.pixelSize: 13
            }

            // Results list
            ListView {
                id: list
                anchors.top: topDivider.bottom
                anchors.bottom: bottomDivider.top
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.leftMargin: 8
                anchors.rightMargin: 8
                topMargin: 4
                bottomMargin: 8
                clip: true
                visible: cmdPalette.results.length > 0
                boundsBehavior: Flickable.StopAtBounds
                model: cmdPalette.results
                currentIndex: cmdPalette.selectedIndex
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Column {
                    id: rowItem
                    required property var modelData
                    required property int index
                    readonly property bool showSection: index === 0 || cmdPalette.results[index - 1].section !== modelData.section
                    readonly property bool selected: index === cmdPalette.selectedIndex
                    readonly property bool isBookOrPassage: modelData.kind === "book" || modelData.kind === "text"
                    readonly property bool isHighlight: modelData.kind === "mark" && modelData.markKind === "highlight"
                    readonly property string badgeText: selected ? qsTr("Enter") : (modelData.hint || "")
                    width: list.width

                    // Sentence-case 11/600 section heading (07_command_palette).
                    Label {
                        visible: rowItem.showSection
                        width: parent.width
                        leftPadding: 10
                        rightPadding: 10
                        topPadding: rowItem.index === 0 ? 6 : 12
                        bottomPadding: 4
                        text: rowItem.modelData.section
                        color: theme.ink4
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        font.letterSpacing: 0.22
                    }

                    Rectangle {
                        id: rowBg
                        width: parent.width
                        height: (rowItem.modelData.subtitle || "") !== "" ? 46 : 38
                        radius: Ui.radius(8, theme.radius)
                        color: rowItem.selected ? Ui.tint(theme.accent, theme.light ? 0.14 : 0.16)
                             : rowHover.hovered ? Ui.tint(theme.ink1, 0.05) : "transparent"

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            spacing: 12

                            // Leading 26×26 rounded tile: cover, highlight swatch, or tinted action glyph.
                            Item {
                                Layout.preferredWidth: 26
                                Layout.preferredHeight: 26
                                Layout.alignment: Qt.AlignVCenter

                                CoverImage {
                                    anchors.fill: parent
                                    visible: rowItem.isBookOrPassage
                                    radius: Ui.radius(6, theme.radius)
                                    source: rowItem.modelData.cover || ""
                                    title: rowItem.modelData.bookTitle || rowItem.modelData.title || ""
                                    author: rowItem.modelData.author || ""
                                }

                                Rectangle {
                                    anchors.fill: parent
                                    visible: !rowItem.isBookOrPassage
                                    radius: Ui.radius(6, theme.radius)
                                    color: Ui.tint(theme.ink1, 0.08)

                                    Rectangle {
                                        anchors.centerIn: parent
                                        visible: rowItem.isHighlight
                                        width: 4
                                        height: 16
                                        radius: 2
                                        color: cmdPalette.swatches[rowItem.modelData.color] || cmdPalette.swatches.yellow
                                    }

                                    Glyph {
                                        anchors.centerIn: parent
                                        visible: !rowItem.isHighlight
                                        width: 14
                                        height: 14
                                        path: rowItem.modelData.glyph || Icons.book
                                        color: (rowItem.modelData.kind === "action" || rowItem.modelData.kind === "ask" || rowItem.selected)
                                               ? theme.accentInk : theme.ink2
                                    }
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.alignment: Qt.AlignVCenter
                                spacing: 2

                                Label {
                                    id: rowTitle
                                    Layout.fillWidth: true
                                    text: rowItem.modelData.title || ""
                                    textFormat: Text.PlainText
                                    color: theme.ink1
                                    font.pixelSize: 13
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: (rowItem.modelData.subtitle || "") !== ""
                                    text: rowItem.modelData.subtitle || ""
                                    textFormat: Text.PlainText
                                    color: theme.ink3
                                    font.family: rowItem.modelData.quote ? "Literata" : rowTitle.font.family
                                    font.italic: !!rowItem.modelData.quote
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }
                            }

                            KeyHint {
                                visible: rowItem.badgeText !== ""
                                Layout.alignment: Qt.AlignVCenter
                                text: rowItem.badgeText
                            }
                        }

                        HoverHandler {
                            id: rowHover
                            cursorShape: Qt.PointingHandCursor
                            onHoveredChanged: if (hovered) cmdPalette.selectedIndex = rowItem.index
                        }
                        TapHandler {
                            onTapped: cmdPalette.activate(rowItem.index)
                        }
                    }
                }
            }

            Rectangle {
                id: bottomDivider
                anchors.bottom: footer.top
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: Ui.tint(theme.ink1, 0.07)
            }

            // 38 px footer with bold key hints and calm scope tagline.
            RowLayout {
                id: footer
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: 38
                spacing: 16

                Row {
                    Layout.leftMargin: 18
                    spacing: 16

                    Row {
                        spacing: 4
                        Label { text: "↑ ↓"; color: theme.ink2; font.pixelSize: 11; font.weight: Font.DemiBold }
                        Label { text: qsTr("move"); color: theme.ink4; font.pixelSize: 11 }
                    }
                    Row {
                        spacing: 4
                        Label { text: qsTr("Enter"); color: theme.ink2; font.pixelSize: 11; font.weight: Font.DemiBold }
                        Label { text: qsTr("open"); color: theme.ink4; font.pixelSize: 11 }
                    }
                    Row {
                        visible: cmdPalette.canScopeBook
                        spacing: 4
                        Label { text: qsTr("Tab"); color: theme.ink2; font.pixelSize: 11; font.weight: Font.DemiBold }
                        Label {
                            text: cmdPalette.scope === "book" ? qsTr("everything") : qsTr("this book")
                            color: theme.ink4
                            font.pixelSize: 11
                        }
                    }
                }

                Item { Layout.fillWidth: true }

                Label {
                    Layout.rightMargin: 18
                    text: qsTr("Books, passages, your notes, and anything Clothbound can do")
                    color: theme.ink4
                    font.pixelSize: 11
                    elide: Text.ElideRight
                    Layout.maximumWidth: card.width * 0.52
                }
            }
        }
    }
}
