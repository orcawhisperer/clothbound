import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Format.js" as Format
import "Icons.js" as Icons
import "Themes.js" as Themes
import "Ui.js" as Ui

// Spotlight-grade Ctrl+K Command Palette: fuzzy search across all books,
// current book chapters, cross-book highlights & notes, and quick actions.
FocusScope {
    id: cmdPalette

    property bool open: false
    property bool reading: false
    property string currentTheme: "omarchy"
    property string viewMode: "grid"

    signal openBook(int bookId)
    signal jumpChapter(int chapter, string fragment)
    signal jumpMark(int bookId, int chapter, var offset)
    signal jumpTarget(var target)
    signal triggerAction(string actionId)
    signal closed()

    property var results: []
    property var cachedBooks: []
    property var cachedMarks: []
    property int selectedIndex: 0

    readonly property string statsIcon: "M5 19.5 V13 M10 19.5 V8.5 M15 19.5 V11 M20 19.5 V5.5"
    readonly property string listIcon: "M8 6.5 H20 M8 12 H20 M8 17.5 H20 M4 6.5 H5 M4 12 H5 M4 17.5 H5"
    readonly property string gridIcon: "M4.5 4.5 H10.5 V10.5 H4.5 Z M13.5 4.5 H19.5 V10.5 H13.5 Z M4.5 13.5 H10.5 V19.5 H4.5 Z M13.5 13.5 H19.5 V19.5 H13.5 Z"
    readonly property string refreshIcon: "M19 12 A7 7 0 1 1 16.95 7.05 M19 4.5 V8.5 H15"
    readonly property var swatches: Themes.swatches

    function show() {
        cachedBooks = library.allBooks()
        cachedMarks = marks.searchAll("", 30)
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
    }

    function close() {
        if (!open)
            return
        open = false
        searchDebounce.stop()
        closed()
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

        // Exact prefix / word-start / substring on primary.
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

        // Multi-token AND match across primary + secondary.
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

        // Subsequence fuzzy match (e.g. "tm" -> "The Time Machine").
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
            list.push({ id: "listen", title: narration.active ? qsTr("Stop listening") : qsTr("Listen from this page"), subtitle: qsTr("Read aloud"), hint: "P", glyph: Icons.play })
            list.push({ id: "appearance", title: qsTr("Typography & appearance"), subtitle: qsTr("Typeface, spacing, margins, layout"), hint: "A", glyph: Icons.book })
            list.push({ id: "toggle-theme", title: qsTr("Cycle reading theme"), subtitle: Themes.name(currentTheme), hint: "D", glyph: Icons.sun })
            if (marks.count > 0) {
                list.push({ id: "export-notes-file", title: qsTr("Export notes to Markdown file"), subtitle: qsTr("Obsidian & Readwise format"), hint: "", glyph: Icons.download })
                list.push({ id: "copy-notes", title: qsTr("Copy notes as Markdown"), subtitle: qsTr("Copy all marks to clipboard"), hint: "", glyph: Icons.copy })
            }
            list.push({ id: "close-book", title: qsTr("Back to library"), subtitle: qsTr("Close current book"), hint: "Esc", glyph: Icons.back })
        } else {
            if (library.lastOpenedBook() > 0)
                list.push({ id: "continue-reading", title: qsTr("Continue reading"), subtitle: qsTr("Resume your last opened book"), hint: "Space", glyph: Icons.book })
            list.push({ id: "add-folder", title: qsTr("Add folder of books…"), subtitle: qsTr("Watch a folder for EPUB & PDF books"), hint: "A", glyph: Icons.folder })
            list.push({ id: "add-books", title: qsTr("Add individual books…"), subtitle: qsTr("Pick EPUB or PDF files"), hint: "", glyph: Icons.plus })
            list.push({ id: "manage-folders", title: qsTr("Manage watched folders…"), subtitle: qsTr("%1 watched").arg(library.folders.length), hint: "", glyph: Icons.folder })
            list.push({ id: "rescan", title: qsTr("Rescan library"), subtitle: qsTr("Check watched folders for new or moved books"), hint: "", glyph: refreshIcon })
            list.push({ id: "toggle-view", title: viewMode === "grid" ? qsTr("Switch to list view") : qsTr("Switch to cover grid"), subtitle: qsTr("Library layout"), hint: "V", glyph: viewMode === "grid" ? listIcon : gridIcon })
            list.push({ id: "cycle-sort", title: qsTr("Cycle sort order"), subtitle: qsTr("Recent, title, author, series, added, progress"), hint: "S", glyph: Icons.sort })
            list.push({ id: "toggle-grouping", title: library.works.grouped ? qsTr("Show every edition separately") : qsTr("Group editions together"), subtitle: qsTr("Edition grouping"), hint: "G", glyph: Icons.books })
            list.push({ id: "toggle-theme", title: qsTr("Cycle reading theme"), subtitle: Themes.name(currentTheme), hint: "", glyph: Icons.sun })
        }
        list.push({ id: "stats", title: qsTr("Reading stats & insights"), subtitle: qsTr("Reading time, streak, and finished books"), hint: "", glyph: statsIcon })
        list.push({ id: "ai-settings", title: qsTr("Intelligence settings"), subtitle: qsTr("Gemini, local models, budget"), hint: "Ctrl+,", glyph: Icons.sparkle })
        list.push({ id: "fullscreen", title: qsTr("Toggle full screen"), subtitle: "", hint: "F", glyph: Icons.fullscreen })
        list.push({ id: "help", title: qsTr("Keyboard shortcuts"), subtitle: qsTr("View all keys"), hint: "?", glyph: Icons.keyboard })
        list.push({ id: "quit", title: qsTr("Quit Dogear"), subtitle: "", hint: "Q", glyph: Icons.close })
        return list
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

        const out = []

        // Instant direct jump when reading and user types a percentage or page label.
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
                        hint: "↵",
                        glyph: Icons.page,
                        target: target,
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
                        hint: "↵",
                        glyph: Icons.page,
                        target: target,
                    })
                }
            }
        }

        // 1. Actions
        if (mode === "all" || mode === "actions") {
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
            const actionCap = mode === "actions" ? 25 : (q === "" ? 7 : 6)
            for (let i = 0; i < Math.min(actionCap, matchedActions.length); ++i) {
                const a = matchedActions[i].item
                out.push({
                    kind: "action",
                    section: qsTr("Do"),
                    actionId: a.id,
                    title: a.title,
                    subtitle: a.subtitle,
                    hint: a.hint,
                    glyph: a.glyph,
                })
            }
        }

        // 2. Current book TOC chapters when reading
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
            const chapCap = mode === "chapters" ? 40 : (q === "" ? 5 : 8)
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
                })
            }
        }

        // 3. All books in library
        if (mode === "all") {
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
            const bookCap = q === "" ? 6 : 10
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
                })
            }
        }

        // The words of every book, from the library index.
        if (mode === "all" && q.length >= 3 && libraryIndex.available) {
            const found = libraryIndex.search(q, 6)
            for (const hit of found) {
                const book = library.book(hit.bookId)
                if (!book.id)
                    continue
                out.push({
                    kind: "text",
                    section: qsTr("In your books"),
                    bookId: hit.bookId,
                    chapter: hit.chapter,
                    offset: hit.offset,
                    title: hit.snippet.replace(/[‹›]/g, ""),
                    subtitle: book.title,
                    hint: hit.bookId === reader.bookId && reading ? qsTr("Here") : "",
                    glyph: Icons.search,
                })
            }
        }

        // Passages close in meaning, from every book prepared for it.
        if (mode === "all" && q.length >= 3 && q === meaningQuery) {
            const shown = out.filter(r => r.kind === "text").map(r => r.bookId + ":" + r.chapter)
            for (const hit of bookSearch.libraryResults) {
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
                    title: text + "…",
                    subtitle: book.title,
                    hint: "",
                    glyph: Icons.sparkle,
                })
            }
        }

        // 4. Cross-book & current-book highlights and notes
        if (mode === "all" || mode === "marks") {
            const hits = q === "" ? cachedMarks : marks.searchAll(q, mode === "marks" ? 30 : 8)
            const markCap = mode === "marks" ? 30 : (q === "" ? 4 : 8)
            for (let i = 0; i < Math.min(markCap, hits.length); ++i) {
                const m = hits[i]
                const primaryText = m.note && m.note !== "" ? m.note : m.text
                const subParts = []
                if (m.bookTitle)
                    subParts.push(m.bookTitle)
                if (m.label)
                    subParts.push(m.label)
                if (m.note && m.note !== "" && m.text)
                    subParts.push("“" + m.text.slice(0, 60) + (m.text.length > 60 ? "…" : "") + "”")
                out.push({
                    kind: "mark",
                    section: qsTr("Highlights & Notes"),
                    bookId: m.bookId,
                    chapter: m.chapter,
                    offset: m.start,
                    markKind: m.kind,
                    color: m.color || "yellow",
                    title: m.kind === "bookmark" ? m.text : (m.note !== "" ? m.note : "“" + m.text + "”"),
                    subtitle: subParts.join(" · "),
                    hint: m.bookId === reader.bookId && reading ? qsTr("Here") : "",
                    glyph: m.kind === "bookmark" ? Icons.bookmark : Icons.pen,
                })
            }
        }

        results = out
        selectedIndex = out.length > 0 ? 0 : -1
        list.currentIndex = selectedIndex
    }

    function activate(idx) {
        if (idx < 0 || idx >= results.length)
            return
        const item = results[idx]
        close()
        if (item.kind === "action")
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
    // Search by meaning across the library asks the embedder, so it waits
    // for a pause in typing.
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
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

    // Claim Escape / arrows / Return / Ctrl+K ahead of reader ApplicationShortcuts.
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
        } else if (event.key === Qt.Key_Down || event.key === Qt.Key_Tab || ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_N)) {
            cmdPalette.moveSelection(1)
            event.accepted = true
        } else if (event.key === Qt.Key_Up || event.key === Qt.Key_Backtab || ((event.modifiers & Qt.ControlModifier) && event.key === Qt.Key_P)) {
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
        color: Ui.tint("black", theme.light ? 0.25 : 0.56)
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onPressed: cmdPalette.close()
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    // Spotlight modal card
    Rectangle {
        id: card
        anchors.horizontalCenter: parent.horizontalCenter
        y: Math.max(44, Math.min(120, parent.height * 0.13))
        width: Math.min(parent.width - 48, 640)
        height: Math.min(parent.height - y - 40, header.height + 1 + bodyHeight + footer.height + 1)
        readonly property real bodyHeight: cmdPalette.results.length > 0
            ? Math.min(Math.max(list.contentHeight, 56), Math.max(200, cmdPalette.height * 0.52))
            : 84
        radius: Math.min(14, theme.radius + 2)
        color: theme.raised
        border.width: 1
        border.color: Ui.tint(theme.ink1, theme.light ? 0.14 : 0.16)
        clip: true
        scale: cmdPalette.open ? 1.0 : 0.97
        Behavior on scale { NumberAnimation { duration: 120; easing.type: Easing.OutCubic } }

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onPressed: input.forceActiveFocus()
            onWheel: (wheel) => wheel.accepted = true
        }

        // Top search input row
        RowLayout {
            id: header
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 54
            spacing: 10

            Glyph {
                Layout.leftMargin: 18
                Layout.preferredWidth: 19
                Layout.preferredHeight: 19
                path: Icons.search
                color: theme.accentInk
            }

            TextInput {
                id: input
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                color: theme.ink1
                selectionColor: Ui.tint(theme.accent, 0.35)
                selectedTextColor: theme.ink1
                font.pixelSize: 16
                clip: true
                selectByMouse: true
                onTextEdited: searchDebounce.restart()

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    width: parent.width
                    visible: input.text === ""
                    text: cmdPalette.reading
                        ? qsTr("Jump to book, chapter, note, or action…")
                        : qsTr("Search books, notes, or actions…")
                    color: theme.ink3
                    font.pixelSize: 16
                    elide: Text.ElideRight
                }
            }

            IconButton {
                visible: input.text !== ""
                Layout.preferredWidth: 30
                Layout.preferredHeight: 30
                glyph: Icons.close
                tone: theme.ink2
                onClicked: {
                    input.text = ""
                    cmdPalette.rebuild()
                    input.forceActiveFocus()
                }
            }

            Label {
                Layout.rightMargin: 16
                text: "Esc"
                color: theme.ink3
                font.pixelSize: 11
                font.family: "monospace"
                leftPadding: 6
                rightPadding: 6
                topPadding: 3
                bottomPadding: 3
                background: Rectangle {
                    radius: 4
                    color: Ui.tint(theme.ink1, 0.06)
                    border.width: 1
                    border.color: Ui.tint(theme.ink1, 0.1)
                }
            }
        }

        Rectangle {
            id: topDivider
            anchors.top: header.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: Ui.tint(theme.ink1, 0.08)
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
            text: qsTr("No matching books, notes, or commands")
            color: theme.ink2
            font.pixelSize: 14
        }

        // Results list
        ListView {
            id: list
            anchors.top: topDivider.bottom
            anchors.bottom: bottomDivider.top
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.leftMargin: 6
            anchors.rightMargin: 6
            topMargin: 6
            bottomMargin: 6
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
                width: list.width

                Label {
                    visible: rowItem.showSection
                    width: parent.width
                    leftPadding: 12
                    rightPadding: 12
                    topPadding: rowItem.index === 0 ? 4 : 10
                    bottomPadding: 4
                    text: rowItem.modelData.section
                    color: theme.ink3
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 0.7
                }

                Rectangle {
                    id: rowBg
                    width: parent.width
                    height: 46
                    radius: Math.min(8, theme.radius)
                    color: rowItem.selected ? Ui.tint(theme.accent, theme.light ? 0.16 : 0.22)
                         : rowHover.hovered ? Ui.tint(theme.ink1, 0.06) : "transparent"

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 12

                        // Leading visual: book cover, highlight color bar, or line glyph
                        Item {
                            Layout.preferredWidth: 26
                            Layout.preferredHeight: 32
                            Layout.alignment: Qt.AlignVCenter

                            CoverImage {
                                anchors.centerIn: parent
                                visible: rowItem.modelData.kind === "book"
                                width: 22
                                height: 32
                                source: rowItem.modelData.cover || ""
                                title: rowItem.modelData.title || ""
                                author: rowItem.modelData.author || ""
                            }

                            Rectangle {
                                anchors.centerIn: parent
                                visible: rowItem.modelData.kind === "mark" && rowItem.modelData.markKind === "highlight"
                                width: 4
                                height: 22
                                radius: 2
                                color: cmdPalette.swatches[rowItem.modelData.color] || cmdPalette.swatches.yellow
                            }

                            Glyph {
                                anchors.centerIn: parent
                                visible: rowItem.modelData.kind !== "book" &&
                                         !(rowItem.modelData.kind === "mark" && rowItem.modelData.markKind === "highlight")
                                width: 18
                                height: 18
                                path: rowItem.modelData.glyph || Icons.book
                                color: rowItem.selected ? theme.accentInk : theme.ink2
                            }
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 1

                            Label {
                                Layout.fillWidth: true
                                text: rowItem.modelData.title || ""
                                textFormat: Text.PlainText
                                color: theme.ink1
                                font.pixelSize: 14
                                font.weight: rowItem.selected ? Font.DemiBold : Font.Normal
                                elide: Text.ElideRight
                            }
                            Label {
                                Layout.fillWidth: true
                                visible: (rowItem.modelData.subtitle || "") !== ""
                                text: rowItem.modelData.subtitle || ""
                                textFormat: Text.PlainText
                                color: theme.ink2
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                        }

                        Label {
                            visible: (rowItem.modelData.hint || "") !== ""
                            Layout.alignment: Qt.AlignVCenter
                            text: rowItem.modelData.hint || ""
                            color: rowItem.selected ? theme.accentInk : theme.ink3
                            font.pixelSize: 11
                            font.family: "monospace"
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
            color: Ui.tint(theme.ink1, 0.08)
        }

        // Footer hints
        RowLayout {
            id: footer
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 32
            spacing: 12

            Label {
                Layout.leftMargin: 16
                Layout.fillWidth: true
                text: qsTr("↑ ↓ navigate · ↵ select · > actions · @ notes")
                color: theme.ink3
                font.pixelSize: 11
                elide: Text.ElideRight
            }
            Label {
                Layout.rightMargin: 16
                text: "Ctrl+K"
                color: theme.ink3
                font.pixelSize: 11
                font.family: "monospace"
            }
        }
    }
}
