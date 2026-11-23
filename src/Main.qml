import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import Qt.labs.platform as Platform
import QtQuick.Layouts

import "Icons.js" as Icons
import "Ui.js" as Ui

ApplicationWindow {
    id: win
    width: 1280
    height: 840
    minimumWidth: 720
    minimumHeight: 480
    visible: true
    title: readerOpen ? reader.title + " — Dogear" : "Dogear"
    color: theme.background

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    // Closing the window while a book is read aloud leaves the voice reading:
    // Dogear waits in the bar's tray until it's shown again, or the reading
    // ends. Closed otherwise, it quits.
    onClosing: (close) => {
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
            desktop.notify(qsTr("Dogear reads on"),
                           qsTr("Its controls are in the bar's tray and media panel, and the media keys work."))
        }
    }
    function showAgain() {
        win.show()
        win.raise()
        win.requestActivate()
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
    // Letters are text while a field has the keys, not shortcuts.
    readonly property bool typing: libraryView.typing
    readonly property bool overlayOpen: helpVisible || libraryView.menuOpen || aiSettings.open
    readonly property bool libraryKeys: !readerOpen && !typing && !overlayOpen && !libraryView.detailsOpen

    function openBook(bookId) {
        const book = library.book(bookId)
        if (!book.id)
            return
        if (book.problem !== "") {
            toast.show(qsTr("“%1” can't be opened: %2").arg(book.title).arg(book.problem))
            return
        }
        // The reading surface starts loading with the first book and then
        // stays, so later books open at once.
        readerLoader.active = true
        if (!reader.openBook(bookId))
            toast.show(reader.errorString())
    }
    function closeBook() {
        const id = reader.bookId
        reader.close()
        libraryView.select(id)
    }
    function toggleFullScreen() {
        visibility = visibility === Window.FullScreen ? Window.Windowed : Window.FullScreen
    }

    // --- Everywhere ------------------------------------------------------
    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: Qt.quit() }
    Shortcut { sequences: ["F", "F11"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.toggleFullScreen() }
    Shortcut {
        sequence: "Esc"
        context: Qt.ApplicationShortcut
        enabled: win.helpVisible || libraryView.detailsOpen || aiSettings.open
        onActivated: {
            if (win.helpVisible)
                win.helpVisible = false
            else if (aiSettings.open)
                aiSettings.close()
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
    Shortcut { sequence: "Ctrl+F"; context: Qt.ApplicationShortcut; enabled: !win.readerOpen; onActivated: libraryView.focusSearch() }
    Shortcut { sequence: "A"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys; onActivated: picker.chooseFolders() }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys && library.bookCount > 0; onActivated: libraryView.cycleSort() }
    Shortcut { sequence: "G"; context: Qt.ApplicationShortcut; enabled: win.libraryKeys && library.bookCount > 0; onActivated: libraryView.toggleGrouping() }
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
            onClosed: if (!win.readerOpen) libraryView.focusGrid()
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            reading: win.readerOpen
            z: 60
            onDismissed: win.helpVisible = false
        }
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
