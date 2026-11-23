import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import Qt.labs.platform as Platform
import QtQuick.Layouts

import "Icons.js" as Icons
import "Themes.js" as Themes
import "Ui.js" as Ui

ApplicationWindow {
    id: win
    width: Math.max(minimumWidth, windowSettings.width)
    height: Math.max(minimumHeight, windowSettings.height)
    minimumWidth: 720
    minimumHeight: 480
    visible: true
    title: readerOpen ? reader.title + " — Dogear" : "Dogear"
    color: theme.background

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool geometryReady: false
    property var pendingJump: null

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
        if (windowSettings.maximized)
            win.visibility = Window.Maximized
        geometryReady = true
    }

    // Closing the window while a book is read aloud leaves the voice reading:
    // Dogear waits in the bar's tray until it's shown again, or the reading
    // ends. Closed otherwise, it quits.
    onClosing: (close) => {
        win.saveWindowGeometry()
        if (narration.active) {
            close.accepted = false
            win.listenInBackground()
        } else {
            Qt.quit()
        }
    }
    property bool backgroundTold: false
    function listenInBackground() {
        win.hide()
        if (!backgroundTold) {
            backgroundTold = true
            desktop.notify(qsTr("Dogear keeps reading"),
                           qsTr("Click here, or open Dogear again, to bring the book back. The media keys and the bar's media panel still control the voice."),
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
    Connections {
        target: narration
        function onStateChanged() {
            if (!narration.active && !win.visible)
                Qt.quit()
        }
    }
    Platform.SystemTrayIcon {
        visible: !win.visible && narration.active
        icon.source: "qrc:/dogear.svg"
        tooltip: narration.active ? qsTr("Dogear is reading %1").arg(reader.title) : "Dogear"
        onActivated: win.showAgain()
        menu: Platform.Menu {
            Platform.MenuItem {
                text: narration.playing ? qsTr("Pause") : qsTr("Play")
                onTriggered: narration.toggle()
            }
            Platform.MenuItem {
                text: qsTr("Next paragraph")
                onTriggered: narration.nextParagraph()
            }
            Platform.MenuItem {
                text: qsTr("Previous paragraph")
                onTriggered: narration.previousParagraph()
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: qsTr("Show Dogear")
                onTriggered: win.showAgain()
            }
            Platform.MenuItem {
                text: qsTr("Stop and quit")
                onTriggered: Qt.quit()
            }
        }
    }

    property bool helpVisible: false
    readonly property bool readerOpen: reader.open
    // Letters are text while any editable field or command palette has focus, not global shortcuts.
    readonly property bool typing: libraryView.typing || commandPalette.open
                                   || (activeFocusItem !== null && activeFocusItem.cursorPosition !== undefined && !activeFocusItem.readOnly)
    readonly property bool overlayOpen: helpVisible || libraryView.menuOpen || aiSettings.open || welcome.open
                                        || commandPalette.open || libraryView.foldersOpen || libraryView.statsOpen
                                        || libraryView.collectionDialogOpen
    readonly property bool libraryKeys: !readerOpen && !typing && !overlayOpen && !libraryView.detailsOpen

    function openBook(bookId) {
        const book = library.book(bookId)
        if (!book.id)
            return
        if (book.problem !== "") {
            toast.show(qsTr("“%1” can't be opened: %2").arg(book.title).arg(book.problem))
            return
        }
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
        const id = reader.bookId
        if (readerLoader.item)
            readerLoader.item.closeBook()
        reader.close()
        libraryView.select(id)
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
        if (reader.open && reader.bookId === bookId && reader.format === "pdf") {
            Qt.callLater(flushPendingJump)
        }
    }
    function flushPendingJump() {
        if (!pendingJump || !reader.open || reader.bookId !== pendingJump.bookId || !readerLoader.item)
            return
        const target = pendingJump
        pendingJump = null
        readerLoader.item.goTo({ chapter: target.chapter, offset: target.offset, instant: true })
    }
    function runPaletteAction(actionId) {
        if (actionId === "add-folder") {
            picker.chooseFolders()
        } else if (actionId === "add-books") {
            picker.chooseBooks()
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
        } else if (actionId === "toggle-theme") {
            if (readerOpen && readerLoader.item) {
                readerLoader.item.cycleTheme()
            } else {
                const order = Themes.order
                readingSettings.theme = order[(order.indexOf(readingSettings.theme) + 1) % order.length]
                toast.show(qsTr("%1 theme").arg(Themes.name(readingSettings.theme)))
            }
        } else if (actionId === "ai-settings") {
            aiSettings.open ? aiSettings.close() : aiSettings.show()
        } else if (actionId === "fullscreen") {
            toggleFullScreen()
        } else if (actionId === "help") {
            helpVisible = !helpVisible
        } else if (actionId === "quit") {
            saveWindowGeometry()
            Qt.quit()
        } else if (readerOpen && readerLoader.item) {
            if (actionId === "search-book") {
                readerLoader.item.searchBar.show()
            } else if (actionId === "go-to") {
                Qt.callLater(() => { if (readerLoader.item) readerLoader.item.pageKey("g", 0) })
            } else if (actionId === "contents") {
                readerLoader.item.showContents()
            } else if (actionId === "appearance") {
                readerLoader.item.showAppearance()
            } else if (actionId === "marks") {
                readerLoader.item.showMarks()
            } else if (actionId === "bookmark") {
                readerLoader.item.toggleBookmark()
            } else if (actionId === "companion-ask") {
                readerLoader.item.showCompanion("ask")
            } else if (actionId === "companion-prev") {
                readerLoader.item.showCompanion("recap")
                if (companion.feature !== "recap" || companion.problem !== "")
                    companion.recap()
            } else if (actionId === "companion-people") {
                readerLoader.item.showCompanion("whoswho")
                companion.whosWho()
            } else if (actionId === "voices") {
                readerLoader.item.showVoices()
            } else if (actionId === "listen") {
                readerLoader.item.listen()
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
        }
    }

    // --- Everywhere ------------------------------------------------------
    Shortcut { sequence: "Ctrl+K"; context: Qt.ApplicationShortcut; onActivated: commandPalette.open ? commandPalette.close() : commandPalette.show() }
    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: { win.saveWindowGeometry(); Qt.quit() } }
    Shortcut { sequences: ["F", "F11"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.toggleFullScreen() }
    Shortcut {
        sequence: "Esc"
        context: Qt.ApplicationShortcut
        enabled: win.helpVisible || welcome.open || aiSettings.open || libraryView.detailsOpen
                 || libraryView.foldersOpen || libraryView.statsOpen || libraryView.collectionDialogOpen
                 || commandPalette.open
        onActivated: {
            if (win.helpVisible)
                win.helpVisible = false
            else if (commandPalette.open)
                commandPalette.close()
            else if (welcome.open)
                welcome.close()
            else if (aiSettings.open)
                aiSettings.close()
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
    // Ctrl+Z undoes what a toast offers to; otherwise, while reading, the
    // last change to the book's marks.
    Shortcut {
        sequences: [StandardKey.Undo]
        context: Qt.ApplicationShortcut
        enabled: !win.typing
        onActivated: {
            if (toast.runAction())
                return
            if (readerLoader.item && readerLoader.item.visible && marks.canUndo) {
                const what = marks.undoText
                marks.undo()
                toast.show(qsTr("Undid: %1").arg(what))
            }
        }
    }
    Shortcut { sequences: ["Ctrl+,"]; context: Qt.ApplicationShortcut; onActivated: aiSettings.open ? aiSettings.close() : aiSettings.show() }
    function showAiSettings() { aiSettings.show() }

    // --- Library ---------------------------------------------------------
    Shortcut { sequence: "/"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys; onActivated: libraryView.focusSearch() }
    Shortcut { sequence: "Ctrl+F"; context: Qt.ApplicationShortcut; enabled: !win.readerOpen && !commandPalette.open; onActivated: libraryView.focusSearch() }
    Shortcut { sequence: "A"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys; onActivated: picker.chooseFolders() }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys && library.bookCount > 0; onActivated: libraryView.cycleSort() }
    Shortcut { sequence: "G"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys && library.bookCount > 0; onActivated: libraryView.toggleGrouping() }
    Shortcut { sequence: "V"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys && library.bookCount > 0; onActivated: libraryView.toggleViewMode() }
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
            visible: !win.readerOpen
            focus: !win.readerOpen
            onOpenBook: (bookId) => win.openBook(bookId)
        }

        Loader {
            id: readerLoader
            anchors.fill: parent
            active: false
            visible: reader.open
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
            visible: drop.containsDrag
            color: Ui.tint(theme.background, 0.88)
            border.width: 3
            border.color: theme.accent
            z: 40
            Column {
                anchors.centerIn: parent
                spacing: 12
                Glyph { anchors.horizontalCenter: parent.horizontalCenter; width: 48; height: 48; path: Icons.plus; color: theme.accent; stroke: 1.6 }
                Label {
                    text: qsTr("Drop books or folders to add them")
                    color: theme.text
                    font.pixelSize: 18
                }
            }
        }

        Toast { id: toast }

        AiSettings {
            id: aiSettings
            anchors.fill: parent
            z: 55
            onClosed: {
                if (win.readerOpen && readerLoader.item)
                    readerLoader.item.focusPage()
                else
                    libraryView.focusGrid()
            }
        }

        Welcome {
            id: welcome
            anchors.fill: parent
            z: 58
            onAddBooks: picker.chooseFolders()
            onShowIntelligence: win.showAiSettings()
            onClosed: {
                if (win.readerOpen && readerLoader.item)
                    readerLoader.item.focusPage()
                else
                    libraryView.focusGrid()
            }
        }

        CommandPalette {
            id: commandPalette
            anchors.fill: parent
            z: 59
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
            onClosed: {
                if (win.readerOpen && readerLoader.item)
                    readerLoader.item.focusPage()
                else
                    libraryView.focusGrid()
            }
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            reading: win.readerOpen
            z: 60
            onDismissed: win.helpVisible = false
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
    }
    Connections {
        target: library.works
        function onSortOrderChanged() { if (win.geometryReady) librarySettings.sortOrder = library.works.sortOrder }
        function onGroupedChanged() { if (win.geometryReady) librarySettings.grouped = library.works.grouped }
    }
    Connections {
        target: libraryView
        function onViewModeChanged() { if (win.geometryReady) librarySettings.viewMode = libraryView.viewMode }
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
    function tip(id, text) {
        if (onboarding.tips.indexOf(id) >= 0 || welcome.open)
            return
        onboarding.tips = onboarding.tips.concat([id])
        toast.show(text, qsTr("Got it"), () => {})
    }
    // A first launch is welcomed; a new version (for someone who has read
    // with Dogear before) gets What's new. Never pop over a CLI-opened book.
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

    // How pages look, kept between sessions in ~/.config/dogear/settings.ini.
    Settings {
        id: readingSettings
        location: settingsUrl
        category: "reading"
        property string theme: "omarchy"
        property string font: "Literata"
        property real fontScale: 1.15
        property real lineHeight: 1.55
        property string margins: "normal"
        property bool justify: true
        property bool hyphenate: true
        property string layout: "auto"
        property bool animate: true
        property bool publisher: false
        property bool pdfCrop: true
        property bool pdfOriginalColors: false
    }

    Connections {
        target: library
        function onProblem(message) { toast.show(message) }
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
