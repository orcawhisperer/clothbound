import QtCore
import QtQuick
import QtQuick.Controls
import "components"
import "library"
import "reader"
import "settings"

import "components/Icons.js" as Icons
import "components/Themes.js" as Themes
import "components/Ui.js" as Ui

ApplicationWindow {
    id: win
    width: Math.max(minimumWidth, windowSettings.width)
    height: Math.max(minimumHeight, windowSettings.height)
    minimumWidth: 720
    minimumHeight: 480
    visible: true
    title: readerOpen ? reader.title + " — Clothbound" : "Clothbound"
    color: theme.ground

    // The Clothbound style reads these roles (see src/style/Clothbound/README.md);
    // every control and popup in the window inherits them.
    palette.window: theme.ground
    palette.windowText: theme.ink1
    palette.base: theme.fill
    palette.alternateBase: theme.fillHover
    palette.text: theme.ink1
    palette.placeholderText: theme.ink4
    palette.button: theme.fill
    palette.buttonText: theme.ink1
    palette.mid: theme.ink3
    palette.midlight: theme.hairline
    palette.dark: theme.separator
    palette.light: theme.overlay
    palette.shadow: theme.shadow
    palette.highlight: theme.accent
    palette.highlightedText: theme.accentForeground
    palette.link: theme.accentInk
    palette.linkVisited: theme.accentSoft
    palette.toolTipBase: theme.overlay
    palette.toolTipText: theme.ink1

    property bool geometryReady: false
    property var pendingJump: null
    property bool pendingListen: false
    property string pendingCompanionQuestion: ""

    function saveWindowGeometry() {
        if (!geometryReady)
            return
        if (visibility === Window.Maximized) {
            windowSettings.maximized = true
        } else if (visibility === Window.Windowed) {
            windowSettings.maximized = false
            if (width >= minimumWidth)
                windowSettings.width = width
            if (height >= minimumHeight)
                windowSettings.height = height
        }
    }
    onWidthChanged: if (geometryReady && visibility === Window.Windowed) geometryTimer.restart()
    onHeightChanged: if (geometryReady && visibility === Window.Windowed) geometryTimer.restart()
    onVisibilityChanged: if (geometryReady) saveWindowGeometry()
    Timer {
        id: geometryTimer
        interval: 250
        onTriggered: win.saveWindowGeometry()
    }

    Component.onCompleted: {
        // Restore persisted library preferences.
        const validSorts = ["recent", "title", "author", "series", "added", "progress"]
        if (validSorts.indexOf(librarySettings.sortOrder) >= 0)
            library.works.sortOrder = librarySettings.sortOrder
        library.works.grouped = librarySettings.grouped
        if (librarySettings.viewMode === "list" || librarySettings.viewMode === "grid")
            libraryView.viewMode = librarySettings.viewMode
        libraryView.sidebarCollapsed = librarySettings.sidebarCollapsed
        if (windowSettings.maximized)
            win.visibility = Window.Maximized
        geometryReady = true
    }

    function quitApp() {
        win.saveWindowGeometry()
        if (narration.active)
            narration.stop()
        Qt.quit()
    }

    // Closing the window always stops audio and quits Clothbound cleanly.
    // To keep listening in the background intentionally, use the tray button
    // in the listening bar.
    onClosing: (close) => {
        close.accepted = true
        win.quitApp()
    }
    property bool backgroundTold: false
    function listenInBackground() {
        win.hide()
        if (!backgroundTold) {
            backgroundTold = true
            desktop.notify(qsTr("Clothbound keeps reading"),
                           qsTr("Click here, the tray icon, or open Clothbound again to bring the book back. Media keys still control the voice."),
                           true)
        }
    }
    function showAgain() {
        instance.bringBack()
    }
    Connections {
        target: desktop
        function onClicked(token) { instance.bringBack(token) }
    }
    property bool wasNarrationActive: false
    function applyDefaultSleep() {
        const pref = listenSettings.defaultSleep
        if (pref === "chapter")
            narration.sleepAtEndOfChapter()
        else if (pref === "15" || pref === "30" || pref === "60")
            narration.sleepIn(parseInt(pref, 10))
    }
    Connections {
        target: narration
        function onStateChanged() {
            const active = narration.active
            if (active && !win.wasNarrationActive && listenSettings.defaultSleep !== "off"
                && narration.sleepSecondsLeft <= 0 && !narration.sleepAtChapterEnd) {
                win.applyDefaultSleep()
            }
            win.wasNarrationActive = active
            if (!active && !win.visible)
                Qt.quit()
        }
    }
    Binding {
        target: trayIcon
        property: "visible"
        value: !win.visible && narration.active
    }
    Binding {
        target: trayIcon
        property: "playing"
        value: narration.playing
    }
    Binding {
        target: trayIcon
        property: "tooltip"
        value: narration.active ? qsTr("Clothbound is reading %1").arg(reader.title) : "Clothbound"
    }
    Connections {
        target: trayIcon
        function onActivated() { win.showAgain() }
        function onTogglePlayRequested() { narration.toggle() }
        function onNextRequested() { narration.nextParagraph() }
        function onPreviousRequested() { narration.previousParagraph() }
        function onQuitRequested() { win.quitApp() }
    }

    property bool helpVisible: false
    readonly property bool readerOpen: reader.open
    // Letters are text while any editable field or command palette has focus, not global shortcuts.
    readonly property bool typing: libraryView.typing || commandPalette.open
                                   || (activeFocusItem !== null && activeFocusItem.cursorPosition !== undefined && !activeFocusItem.readOnly)
    readonly property bool overlayOpen: helpVisible || libraryView.menuOpen || settingsView.open || welcome.open
                                        || commandPalette.open || libraryView.foldersOpen || libraryView.statsOpen
                                        || libraryView.collectionDialogOpen || libraryView.addUrlOpen
                                        || libraryView.catalogsOpen
    readonly property bool libraryKeys: !readerOpen && !typing && !overlayOpen && !libraryView.detailsOpen

    function openBook(bookId) {
        const book = library.book(bookId)
        if (!book.id)
            return
        if (book.problem !== "") {
            toast.show(qsTr("“%1” can't be opened: %2").arg(book.title).arg(book.problem))
            return
        }
        if (pendingJump && pendingJump.bookId !== bookId)
            pendingJump = null
        if (!reader.openBook(bookId)) {
            toast.show(reader.errorString())
            return
        }
        // The reading surface starts loading with the first book and then
        // stays, so later books open at once.
        readerLoader.active = true
    }
    function closeBook() {
        pendingJump = null
        pendingListen = false
        pendingCompanionQuestion = ""
        const id = reader.bookId
        if (readerLoader.item)
            readerLoader.item.closeBook()
        reader.close()
        libraryView.select(id)
    }
    function startReaderListening() {
        if (!readerLoader.item)
            return
        if (typeof readerLoader.item.startListening === "function")
            readerLoader.item.startListening()
        else if (typeof readerLoader.item.listen === "function")
            readerLoader.item.listen()
    }
    function openBookAndListen(bookId) {
        if (readerOpen && reader.bookId === bookId && readerLoader.item) {
            startReaderListening()
            return
        }
        pendingListen = true
        openBook(bookId)
        if (!reader.open || reader.bookId !== bookId) {
            pendingListen = false
            return
        }
        if (reader.format === "pdf")
            Qt.callLater(flushPendingReaderActions)
    }
    function askCompanion(question) {
        const q = (question || "").trim()
        if (q === "")
            return
        const targetId = readerOpen ? reader.bookId : library.lastOpenedBook()
        if (targetId <= 0)
            return
        if (readerOpen && reader.bookId === targetId && readerLoader.item) {
            if (typeof readerLoader.item.askCompanion === "function") {
                readerLoader.item.askCompanion(q)
            } else {
                readerLoader.item.showCompanion("ask")
                companion.ask(q)
            }
            return
        }
        pendingCompanionQuestion = q
        openBook(targetId)
        if (!reader.open || reader.bookId !== targetId) {
            pendingCompanionQuestion = ""
            return
        }
        if (reader.format === "pdf")
            Qt.callLater(flushPendingReaderActions)
    }
    function flushPendingReaderActions() {
        if (!reader.open || !readerLoader.item)
            return
        if (pendingListen) {
            pendingListen = false
            startReaderListening()
        }
        if (pendingCompanionQuestion !== "") {
            const q = pendingCompanionQuestion
            pendingCompanionQuestion = ""
            if (typeof readerLoader.item.askCompanion === "function") {
                readerLoader.item.askCompanion(q)
            } else {
                readerLoader.item.showCompanion("ask")
                companion.ask(q)
            }
        }
    }
    function toggleFullScreen() {
        visibility = visibility === Window.FullScreen ? Window.Windowed : Window.FullScreen
    }
    function jumpToMark(bookId, chapter, offset) {
        if (readerOpen && reader.bookId === bookId && readerLoader.item) {
            readerLoader.item.goTo({ chapter: chapter, offset: offset, instant: true })
            return
        }
        pendingJump = { bookId: bookId, chapter: chapter, offset: offset }
        openBook(bookId)
        if (!reader.open || reader.bookId !== bookId) {
            pendingJump = null
            return
        }
        if (reader.format === "pdf") {
            Qt.callLater(flushPendingJump)
        }
    }
    function openBookPassage(bookId, chapter, quote) {
        if (!bookId || bookId <= 0)
            return
        const targetChapter = Math.max(0, chapter || 0)
        const cleanQuote = (quote || "").replace(/\u2026+$/, "").replace(/\s+/g, " ").trim()
        if (readerOpen && reader.bookId === bookId && readerLoader.item) {
            applyJump({ bookId: bookId, chapter: targetChapter, offset: 0, quote: cleanQuote })
            return
        }
        pendingJump = { bookId: bookId, chapter: targetChapter, offset: 0, quote: cleanQuote }
        openBook(bookId)
        if (!reader.open || reader.bookId !== bookId) {
            pendingJump = null
            return
        }
        if (reader.format === "pdf") {
            Qt.callLater(flushPendingJump)
        }
    }
    function applyJump(target) {
        if (!target || !reader.open || !readerLoader.item)
            return
        const chapter = Math.max(0, target.chapter || 0)
        const offset = Math.max(0, target.offset || 0)
        const quote = (target.quote || "").trim()
        const place = { chapter: chapter, offset: offset, instant: true }
        if (quote !== "") {
            place.snippet = quote.slice(0, 64)
            if (reader.format === "pdf") {
                const pageText = reader.pdfPageText(chapter)
                const findQuoteSpan = (needle) => {
                    if (!needle || needle.length < 4 || !pageText)
                        return null
                    const escaped = needle.replace(/[.*+?^${}()|[\]\\]/g, "\\$&").replace(/\s+/g, "\\s+")
                    const match = new RegExp(escaped, "i").exec(pageText)
                    return match ? { chapter: chapter, start: match.index, end: match.index + match[0].length } : null
                }
                const hit = findQuoteSpan(quote.slice(0, 64)) || findQuoteSpan(quote.slice(0, 28))
                readerLoader.item.goTo(place)
                if (hit) {
                    readerLoader.item.js("clothbound.found(" + JSON.stringify([hit]) + "," + JSON.stringify(hit) + ")")
                    readerLoader.item.foundChapter = chapter
                }
                return
            }
        }
        readerLoader.item.goTo(place)
    }
    function flushPendingJump() {
        if (!pendingJump || !reader.open || reader.bookId !== pendingJump.bookId || !readerLoader.item)
            return
        const target = pendingJump
        pendingJump = null
        applyJump(target)
    }
    function runPaletteAction(actionId) {
        if (actionId === "add-folder") {
            picker.chooseFolders()
        } else if (actionId === "add-books") {
            picker.chooseBooks()
        } else if (actionId === "add-url") {
            if (readerOpen)
                closeBook()
            libraryView.openAddUrl()
        } else if (actionId === "browse-catalogs") {
            if (readerOpen)
                closeBook()
            libraryView.openCatalogs()
        } else if (actionId === "manage-folders") {
            if (readerOpen)
                closeBook()
            libraryView.openFolders()
        } else if (actionId === "rescan") {
            library.rescan()
            toast.show(qsTr("Scanning library…"))
        } else if (actionId === "stats") {
            if (readerOpen)
                closeBook()
            libraryView.openStats()
        } else if (actionId === "toggle-view") {
            if (!readerOpen)
                libraryView.toggleViewMode()
        } else if (actionId === "cycle-sort") {
            if (!readerOpen)
                libraryView.cycleSort()
        } else if (actionId === "toggle-grouping") {
            if (!readerOpen)
                libraryView.toggleGrouping()
        } else if (actionId === "continue-reading") {
            const last = library.lastOpenedBook()
            if (last > 0)
                openBook(last)
        } else if (actionId === "listen") {
            if (readerOpen && readerLoader.item) {
                readerLoader.item.listen()
            } else {
                const last = library.lastOpenedBook()
                if (last > 0)
                    openBookAndListen(last)
            }
        } else if (actionId === "toggle-theme") {
            if (readerOpen && readerLoader.item) {
                readerLoader.item.cycleTheme()
            } else {
                const order = Themes.order
                readingSettings.theme = order[(order.indexOf(readingSettings.theme) + 1) % order.length]
                toast.show(qsTr("%1 theme").arg(Themes.name(readingSettings.theme)))
            }
        } else if (actionId === "settings") {
            settingsView.open ? settingsView.close() : settingsView.show()
        } else if (actionId === "ai-settings") {
            settingsView.show("intelligence")
        } else if (actionId === "fullscreen") {
            toggleFullScreen()
        } else if (actionId === "help") {
            helpVisible = !helpVisible
        } else if (actionId === "quit") {
            win.quitApp()
        } else if (readerOpen && readerLoader.item) {
            if (actionId === "search-book") {
                readerLoader.item.searchBar.show()
            } else if (actionId === "go-to") {
                Qt.callLater(() => { if (readerLoader.item) readerLoader.item.pageKey("g", 0) })
            } else if (actionId === "contents") {
                readerLoader.item.showContents()
            } else if (actionId === "appearance") {
                readerLoader.item.showAppearance()
            } else if (actionId === "pdf-fit-page") {
                readerLoader.item.setPdfFit("page", true)
            } else if (actionId === "pdf-fit-width") {
                readerLoader.item.setPdfFit("width", true)
            } else if (actionId === "pdf-fit-actual") {
                readerLoader.item.setPdfFit("actual", true)
            } else if (actionId === "marks") {
                readerLoader.item.showMarks()
            } else if (actionId === "bookmark") {
                readerLoader.item.toggleBookmark()
            } else if (actionId === "companion-ask") {
                readerLoader.item.showCompanion("ask", true)
            } else if (actionId === "companion-prev") {
                readerLoader.item.showCompanion("recap")
                if (companion.feature !== "recap" || companion.problem !== "")
                    companion.recap()
            } else if (actionId === "companion-people") {
                readerLoader.item.showCompanion("whoswho")
                companion.whosWho()
            } else if (actionId === "voices") {
                readerLoader.item.showVoices()
            } else if (actionId === "export-notes-file") {
                const saved = marks.exportMarkdownToFile("", reader.title, reader.author, "obsidian")
                if (saved !== "")
                    toast.show(qsTr("Saved notes to %1").arg(saved))
                else
                    toast.show(qsTr("Could not save notes"))
            } else if (actionId === "copy-notes") {
                reader.copyText(marks.markdown(reader.title, reader.author))
                toast.show(qsTr("Your marks are copied"))
            } else if (actionId === "close-book") {
                closeBook()
            }
        }
    }

    Connections {
        target: reader
        function onLocationChanged() {
            if (win.pendingJump && reader.relocationSeq > 0)
                win.flushPendingJump()
            if ((win.pendingListen || win.pendingCompanionQuestion !== "") && reader.relocationSeq > 0)
                win.flushPendingReaderActions()
        }
    }
    Connections {
        target: companion
        function onOpenBookPassage(bookId, chapter, quote) {
            win.openBookPassage(bookId, chapter, quote)
        }
    }

    // --- Everywhere ------------------------------------------------------
    Shortcut {
        sequence: "Ctrl+K"
        context: Qt.ApplicationShortcut
        onActivated: commandPalette.open ? commandPalette.close()
                   : !win.readerOpen && libraryView.searchText !== "" ? commandPalette.search(libraryView.searchText)
                   : commandPalette.show()
    }
    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.quitApp() }
    Shortcut { sequence: "Ctrl+Q"; context: Qt.ApplicationShortcut; onActivated: win.quitApp() }
    Shortcut { sequence: "Ctrl+W"; context: Qt.ApplicationShortcut; onActivated: win.close() }
    Shortcut { sequences: ["F", "F11"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.toggleFullScreen() }
    Shortcut {
        sequence: "Esc"
        context: Qt.ApplicationShortcut
        enabled: win.helpVisible || welcome.open || settingsView.open || libraryView.detailsOpen
                 || libraryView.foldersOpen || libraryView.statsOpen || libraryView.collectionDialogOpen
                 || libraryView.addUrlOpen || libraryView.catalogsOpen || commandPalette.open
        onActivated: {
            if (win.helpVisible)
                win.helpVisible = false
            else if (commandPalette.open)
                commandPalette.close()
            else if (welcome.open)
                welcome.close()
            else if (settingsView.open)
                settingsView.close()
            else if (libraryView.catalogsOpen)
                libraryView.closeCatalogs()
            else if (libraryView.addUrlOpen)
                libraryView.closeAddUrl()
            else if (libraryView.collectionDialogOpen)
                libraryView.closeCollectionDialog()
            else if (libraryView.statsOpen)
                libraryView.closeStats()
            else if (libraryView.foldersOpen)
                libraryView.closeFolders()
            else
                libraryView.closeDetails()
        }
    }
    Shortcut {
        sequence: "Ctrl+Shift+U"
        context: Qt.ApplicationShortcut
        enabled: !commandPalette.open && !settingsView.open && !welcome.open
        onActivated: {
            if (libraryView.addUrlOpen) {
                libraryView.closeAddUrl()
            } else {
                if (win.readerOpen)
                    win.closeBook()
                libraryView.openAddUrl()
            }
        }
    }
    Shortcut {
        sequence: "Ctrl+Shift+O"
        context: Qt.ApplicationShortcut
        enabled: !commandPalette.open && !settingsView.open && !welcome.open
        onActivated: {
            if (libraryView.catalogsOpen) {
                libraryView.closeCatalogs()
            } else {
                if (win.readerOpen)
                    win.closeBook()
                libraryView.openCatalogs()
            }
        }
    }
    // Ctrl+Z undoes what a toast offers to; otherwise, while reading, the
    // last change to the book's marks.
    Shortcut {
        sequences: [StandardKey.Undo]
        context: Qt.ApplicationShortcut
        enabled: !win.typing
        onActivated: {
            if (toast.runAction())
                return
            if (win.readerOpen && readerLoader.item && marks.canUndo) {
                const what = marks.undoText
                marks.undo()
                toast.show(qsTr("Undid: %1").arg(what))
            }
        }
    }
    Shortcut { sequences: ["Ctrl+,"]; context: Qt.ApplicationShortcut; onActivated: settingsView.open ? settingsView.close() : settingsView.show() }
    function showSettings() { settingsView.show() }
    function showAiSettings() { settingsView.show("intelligence") }

    // --- Library ---------------------------------------------------------
    Shortcut { sequence: "/"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys; onActivated: libraryView.focusSearch() }
    Shortcut { sequence: "Ctrl+F"; context: Qt.ApplicationShortcut; enabled: !win.readerOpen && !commandPalette.open; onActivated: libraryView.focusSearch() }
    Shortcut { sequence: "A"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys; onActivated: picker.chooseFolders() }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys && library.bookCount > 0; onActivated: libraryView.cycleSort() }
    Shortcut { sequence: "G"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys && library.bookCount > 0; onActivated: libraryView.toggleGrouping() }
    Shortcut { sequence: "V"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys && library.bookCount > 0; onActivated: libraryView.toggleViewMode() }
    Shortcut { sequence: "Ctrl+B"; context: Qt.ApplicationShortcut; enabled: !win.readerOpen && !win.overlayOpen; onActivated: libraryView.toggleSidebar() }
    Shortcut { sequence: "I"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys; onActivated: libraryView.showDetails(libraryView.currentRow) }
    Shortcut { sequence: "H"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys; onActivated: libraryView.moveSelection("left") }
    Shortcut { sequence: "L"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys; onActivated: libraryView.moveSelection("right") }
    Shortcut { sequence: "K"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys; onActivated: libraryView.moveSelection("up") }
    Shortcut { sequence: "J"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys; onActivated: libraryView.moveSelection("down") }
    Shortcut {
        sequence: "Space"
        context: Qt.ApplicationShortcut
        enabled: win.libraryKeys
        onActivated: {
            const last = library.lastOpenedBook()
            if (last > 0)
                win.openBook(last)
            else
                toast.show(qsTr("Nothing to continue yet: pick a book and press ↵"))
        }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        id: root
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        LibraryView {
            id: libraryView
            anchors.fill: parent
            property real viewReveal: win.readerOpen ? 0 : 1
            Behavior on viewReveal {
                NumberAnimation {
                    duration: theme.reducedMotion ? 90 : (win.readerOpen ? 180 : 220)
                    easing.type: Easing.OutCubic
                }
            }
            opacity: viewReveal
            scale: theme.reducedMotion ? 1.0 : (0.985 + 0.015 * viewReveal)
            visible: viewReveal > 0.01
            enabled: !win.readerOpen
            focus: !win.readerOpen
            onOpenBook: (bookId) => win.openBook(bookId)
            onListenBook: (bookId) => win.openBookAndListen(bookId)
            onPaletteRequested: (text) => text !== "" ? commandPalette.search(text) : commandPalette.show()
        }

        Loader {
            id: readerLoader
            anchors.fill: parent
            active: false
            property real readerReveal: reader.open ? 1 : 0
            Behavior on readerReveal {
                NumberAnimation {
                    duration: theme.reducedMotion ? 90 : (reader.open ? 210 : 170)
                    easing.type: reader.open ? Easing.OutCubic : Easing.OutQuad
                }
            }
            opacity: readerReveal
            visible: readerReveal > 0.01
            enabled: reader.open
            sourceComponent: EpubReader {
                settings: readingSettings
                onCloseRequested: win.closeBook()
            }
        }

        // Books and folders dropped anywhere join the library.
        DropArea {
            id: drop
            anchors.fill: parent
            keys: ["text/uri-list"]
            onDropped: (event) => {
                if (event.hasUrls) {
                    library.addUrls(event.urls)
                    event.acceptProposedAction()
                }
            }
        }
        Rectangle {
            anchors.fill: parent
            opacity: drop.containsDrag ? 1 : 0
            visible: opacity > 0.01
            Behavior on opacity {
                NumberAnimation { duration: theme.reducedMotion ? 80 : 150; easing.type: Easing.OutCubic }
            }
            color: Ui.tint(theme.ground, 0.88)
            border.width: 3
            border.color: theme.accent
            z: 40
            Column {
                anchors.centerIn: parent
                spacing: 12
                scale: theme.reducedMotion ? 1.0 : (0.96 + 0.04 * parent.opacity)
                Glyph { anchors.horizontalCenter: parent.horizontalCenter; width: 48; height: 48; path: Icons.plus; color: theme.accentInk; stroke: 1.6 }
                Label {
                    text: qsTr("Drop books or folders to add them")
                    color: theme.ink1
                    font.pixelSize: 18
                }
            }
        }

        // Above everything, settings included. Lift cleanly above the reader's
        // floating listening pill or bottom J3 first-book keybar when visible.
        Toast {
            id: toast
            z: 70
            readonly property bool readerSurfaceVisible: win.readerOpen && !settingsView.open && !welcome.open
                                                         && !commandPalette.open && !win.helpVisible
            bottomClearance: !readerSurfaceVisible ? 28
                           : narration.active
                             ? ((readerLoader.item && readerLoader.item.listenBar)
                                ? Math.round(readerLoader.item.listenBar.height) + 28
                                : 76)
                           : (readerLoader.item && readerLoader.item.firstBookTipsActive ? 116 : 28)
        }

        Loader {
            id: settingsView
            anchors.fill: parent
            z: 55
            active: false
            readonly property var loaded: item
            readonly property bool open: loaded ? !!loaded.open : false
            function show(which) {
                active = true
                if (loaded)
                    loaded.show(which)
            }
            function close() {
                if (loaded)
                    loaded.close()
            }
            sourceComponent: SettingsView {
                anchors.fill: parent
                onClosed: {
                    if (win.readerOpen && readerLoader.item)
                        readerLoader.item.focusPage()
                    else
                        libraryView.focusGrid()
                }
            }
        }

        Loader {
            id: welcome
            anchors.fill: parent
            z: 58
            active: false
            readonly property var loaded: item
            readonly property bool open: loaded ? !!loaded.open : false
            function show(mode) {
                active = true
                if (loaded)
                    loaded.show(mode)
            }
            function close() {
                if (loaded)
                    loaded.close()
            }
            sourceComponent: Welcome {
                anchors.fill: parent
                onAddBooks: picker.chooseFolders()
                onShowIntelligence: win.showAiSettings()
                onClosed: {
                    if (win.readerOpen && readerLoader.item)
                        readerLoader.item.focusPage()
                    else
                        libraryView.focusGrid()
                }
            }
        }

        Loader {
            id: commandPalette
            anchors.fill: parent
            z: 59
            active: false
            readonly property var loaded: item
            readonly property bool open: loaded ? !!loaded.open : false
            function show() {
                active = true
                if (loaded)
                    loaded.show()
            }
            function search(text) {
                active = true
                if (loaded)
                    loaded.search(text)
            }
            function close() {
                if (loaded)
                    loaded.close()
            }
            sourceComponent: CommandPalette {
                anchors.fill: parent
                reading: win.readerOpen
                currentTheme: readingSettings.theme
                viewMode: libraryView.viewMode
                onOpenBook: (bookId) => win.openBook(bookId)
                onJumpChapter: (chapter, fragment) => {
                    if (win.readerOpen && readerLoader.item)
                        readerLoader.item.goTo({ chapter: chapter, fragment: fragment, instant: true })
                }
                onJumpMark: (bookId, chapter, offset) => win.jumpToMark(bookId, chapter, offset)
                onJumpTarget: (target) => {
                    if (win.readerOpen && readerLoader.item)
                        readerLoader.item.goTo(target)
                }
                onTriggerAction: (actionId) => win.runPaletteAction(actionId)
                onAskCompanion: (question) => win.askCompanion(question)
                onClosed: {
                    if (win.readerOpen && readerLoader.item)
                        readerLoader.item.focusPage()
                    else
                        libraryView.focusGrid()
                }
            }
        }

        Loader {
            id: helpOverlayLoader
            anchors.fill: parent
            z: 60
            active: win.helpVisible || (item !== null && item.opacity > 0.01)
            sourceComponent: HelpOverlay {
                anchors.fill: parent
                opacity: win.helpVisible ? 1 : 0
                visible: opacity > 0.01
                enabled: win.helpVisible
                Behavior on opacity {
                    NumberAnimation {
                        duration: theme.reducedMotion ? 90 : (win.helpVisible ? 190 : 145)
                        easing.type: win.helpVisible ? Easing.OutCubic : Easing.OutQuad
                    }
                }
                reading: win.readerOpen
                onDismissed: win.helpVisible = false
            }
        }
    }

    // Window geometry persisted across sessions.
    Settings {
        id: windowSettings
        location: settingsUrl
        category: "window"
        property int width: 1280
        property int height: 840
        property bool maximized: false
    }

    // Library view preferences persisted across sessions.
    Settings {
        id: librarySettings
        location: settingsUrl
        category: "library"
        property string sortOrder: "recent"
        property bool grouped: true
        property string viewMode: "grid"
        property bool sidebarCollapsed: false
    }
    Connections {
        target: library.works
        function onSortOrderChanged() { if (win.geometryReady) librarySettings.sortOrder = library.works.sortOrder }
        function onGroupedChanged() { if (win.geometryReady) librarySettings.grouped = library.works.grouped }
    }
    Connections {
        target: libraryView
        function onViewModeChanged() { if (win.geometryReady) librarySettings.viewMode = libraryView.viewMode }
        function onSidebarCollapsedChanged() { if (win.geometryReady) librarySettings.sidebarCollapsed = libraryView.sidebarCollapsed }
    }

    // What the reader has been shown: the welcome, the version they last
    // saw (for What's new), and the tips already given, once each.
    Settings {
        id: onboarding
        location: settingsUrl
        category: "onboarding"
        property bool welcomed: false
        property string version: ""
        property var tips: []
    }
    // A tip, the first time something happens, and never again.
    function tip(id, text, hint) {
        if (onboarding.tips.indexOf(id) >= 0 || welcome.open)
            return
        onboarding.tips = onboarding.tips.concat([id])
        if (hint && hint !== "")
            toast.show(text, "", hint)
        else
            toast.show(text, qsTr("Got it"), () => {})
    }
    // A first launch is welcomed; a new version (for someone who has read
    // with Clothbound before) gets What's new. Never pop over a CLI-opened book.
    function greet() {
        if (win.readerOpen)
            return
        const version = Qt.application.version
        if (!onboarding.welcomed && library.bookCount === 0)
            welcome.show("welcome")
        else if (onboarding.version !== version)
            welcome.show("whatsnew")
        onboarding.welcomed = true
        onboarding.version = version
    }
    Timer {
        running: !debugMode || Qt.application.arguments.indexOf("--greet") >= 0
        interval: 700
        onTriggered: win.greet()
    }

    // Default listening preferences kept alive from startup so sleep timers arm
    // even before Settings › Voices is ever opened.
    Settings {
        id: listenSettings
        location: settingsUrl
        category: "listen"
        property string defaultSleep: "off"
    }

    // How pages look, kept between sessions in ~/.config/clothbound/settings.ini.
    Settings {
        id: readingSettings
        location: settingsUrl
        category: "reading"
        property string theme: "paper"
        property string font: "Literata"
        property real fontScale: 1.15
        property real lineHeight: 1.62
        property string margins: "normal"
        property bool justify: true
        property bool hyphenate: true
        property bool dropCap: true
        property string layout: "auto"
        property bool animate: true
        property bool pageTurnSound: true
        property bool publisher: false
        property bool pdfCrop: true
        property bool pdfOriginalColors: false
        property string pdfFit: "page"
    }

    Connections {
        target: library
        function onProblem(message) { toast.show(message) }
        function onBooksChanged() {
            if (library.scanning && library.bookCount > 0 && !win.readerOpen)
                win.tip("books-arriving", qsTr("Open any book while the rest arrive."), qsTr("Enter"))
        }
        function onScanFinished(added, updated) {
            if (added > 0)
                toast.show(qsTr("Added %n books", "", added))
        }
    }
    Connections {
        target: picker
        function onChosen(urls) { library.addUrls(urls) }
        function onFailed(message) { toast.show(message) }
    }
}
