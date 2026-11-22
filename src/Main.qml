import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
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
    title: readerOpen ? readerTitle + " — Dogear" : "Dogear"
    color: theme.background

    Material.theme: theme.light ? Material.Light : Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    property bool readerOpen: false
    property string readerTitle
    // Letters are text while a field has the keys, not shortcuts.
    readonly property bool typing: libraryView.typing
    readonly property bool overlayOpen: helpVisible || libraryView.menuOpen
    readonly property bool libraryKeys: !readerOpen && !typing && !overlayOpen && !libraryView.detailsOpen

    function openBook(bookId) {
        const book = library.book(bookId)
        if (!book.id)
            return
        if (book.problem !== "") {
            toast.show(qsTr("“%1” can't be opened: %2").arg(book.title).arg(book.problem))
            return
        }
        library.markOpened(bookId)
        toast.show(qsTr("Opening “%1”").arg(book.title))
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
        enabled: win.helpVisible || libraryView.detailsOpen
        onActivated: {
            if (win.helpVisible)
                win.helpVisible = false
            else
                libraryView.closeDetails()
        }
    }
    Shortcut { sequences: [StandardKey.Undo]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: toast.runAction() }

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

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
            reading: win.readerOpen
            z: 60
            onDismissed: win.helpVisible = false
        }
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
