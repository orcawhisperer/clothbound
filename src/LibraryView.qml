import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Icons.js" as Icons
import "Ui.js" as Ui

// The library: books under way on top, then every work as a cover, found
// by typing and ordered as asked. Keys move through the covers.
FocusScope {
    id: view

    readonly property bool typing: search.activeFocus
    readonly property bool detailsOpen: details.open
    readonly property bool menuOpen: sortMenu.opened
    readonly property alias currentRow: grid.currentIndex

    signal openBook(int bookId)

    // A work with several editions asks which one; a single one opens.
    function activate(row) {
        const work = library.works.get(row)
        if (!work.key)
            return
        if (work.editions > 1)
            details.show(work.bookId, work.key)
        else if (work.problem !== "")
            details.show(work.bookId, work.key)
        else
            openBook(work.bookId)
    }
    function showDetails(row) {
        const work = library.works.get(row)
        if (work.key)
            details.show(work.bookId, work.key)
    }
    function focusSearch() { search.forceActiveFocus() }
    function focusGrid() { grid.forceActiveFocus() }
    function cycleSort() {
        const orders = ["recent", "title", "author", "added", "progress"]
        library.works.sortOrder = orders[(orders.indexOf(library.works.sortOrder) + 1) % orders.length]
        toast.show(qsTr("Sorted by %1").arg(sortLabel(library.works.sortOrder).toLowerCase()))
    }
    function toggleGrouping() {
        library.works.grouped = !library.works.grouped
        toast.show(library.works.grouped ? qsTr("Editions of a work are shown together")
                                         : qsTr("Every edition is shown on its own"))
    }
    function sortLabel(order) {
        return order === "title" ? qsTr("Title")
             : order === "author" ? qsTr("Author")
             : order === "added" ? qsTr("Date added")
             : order === "progress" ? qsTr("Progress") : qsTr("Recent")
    }
    function moveSelection(key) {
        if (grid.count === 0)
            return
        if (grid.currentIndex < 0)
            grid.currentIndex = 0
        else if (key === "left") grid.moveCurrentIndexLeft()
        else if (key === "right") grid.moveCurrentIndexRight()
        else if (key === "up") grid.moveCurrentIndexUp()
        else if (key === "down") grid.moveCurrentIndexDown()
        grid.forceActiveFocus()
    }
    function closeDetails() { details.close() }
    // After reading, the book just closed is the one selected.
    function select(bookId) {
        const row = library.works.rowOfBook(bookId)
        if (row >= 0) {
            grid.currentIndex = row
            // The first row keeps the continue-reading shelf above it in view.
            if (row < grid.columns)
                grid.positionViewAtBeginning()
            else
                grid.positionViewAtIndex(row, GridView.Contain)
        }
        grid.forceActiveFocus()
    }

    Component.onCompleted: grid.forceActiveFocus()

    // --- Top bar ---------------------------------------------------------
    Item {
        id: bar
        anchors.left: parent.left
        anchors.right: parent.right
        height: 72

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 32
            anchors.rightMargin: 24
            spacing: 10

            Label {
                text: qsTr("Library")
                color: theme.text
                font.family: "Literata"
                font.pixelSize: 24
                font.weight: Font.DemiBold
            }
            Label {
                Layout.leftMargin: 6
                Layout.alignment: Qt.AlignBaseline
                visible: library.bookCount > 0
                text: qsTr("%n books", "", library.bookCount)
                color: theme.textTertiary
                font.pixelSize: 13
            }
            Item { Layout.fillWidth: true }

            SearchBox {
                id: search
                Layout.preferredWidth: Math.min(340, view.width * 0.32)
                visible: library.bookCount > 0
                placeholder: qsTr("Search title, author, editor…")
                onTextChanged: library.works.searchText = text
                onAccepted: {
                    if (grid.count > 0)
                        grid.currentIndex = 0
                    grid.forceActiveFocus()
                }
                onEscaped: grid.forceActiveFocus()
            }

            AbstractButton {
                id: sortButton
                visible: library.bookCount > 0
                Layout.preferredHeight: 36
                implicitWidth: sortRow.implicitWidth + 24
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                background: Rectangle {
                    radius: Math.min(8, theme.radius)
                    color: sortButton.hovered || sortMenu.opened ? Ui.tint(theme.text, 0.08) : "transparent"
                }
                contentItem: Item {
                    Row {
                        id: sortRow
                        anchors.centerIn: parent
                        spacing: 6
                        Glyph { anchors.verticalCenter: parent.verticalCenter; width: 18; height: 18; path: Icons.sort; color: theme.textSecondary }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: view.sortLabel(library.works.sortOrder)
                            color: theme.text
                            font.pixelSize: 13
                        }
                        Glyph { anchors.verticalCenter: parent.verticalCenter; width: 14; height: 14; path: Icons.chevronDown; color: theme.textTertiary }
                    }
                }
                onClicked: sortMenu.opened ? sortMenu.close() : sortMenu.open()
                ToolTip.visible: hovered && !sortMenu.opened
                ToolTip.text: qsTr("Order (S)")
                ToolTip.delay: 700

                Popup {
                    id: sortMenu
                    y: sortButton.height + 6
                    x: sortButton.width - width
                    width: 230
                    padding: 6
                    focus: true
                    onClosed: grid.forceActiveFocus()
                    background: Rectangle {
                        radius: Math.min(10, theme.radius)
                        color: theme.surface
                        border.color: Ui.tint(theme.text, 0.1)
                    }
                    contentItem: Column {
                        spacing: 2
                        Repeater {
                            model: ["recent", "title", "author", "added", "progress"]
                            delegate: MenuRow {
                                required property string modelData
                                text: view.sortLabel(modelData)
                                checked: library.works.sortOrder === modelData
                                onClicked: { library.works.sortOrder = modelData; sortMenu.close() }
                            }
                        }
                        Rectangle { width: parent.width; height: 1; color: Ui.tint(theme.text, 0.08) }
                        MenuRow {
                            text: qsTr("Group editions")
                            hint: "G"
                            checked: library.works.grouped
                            onClicked: { library.works.grouped = !library.works.grouped; sortMenu.close() }
                        }
                    }
                }
            }

            IconButton {
                glyph: Icons.sparkle
                tip: ai.hasKey ? qsTr("Gemini (Ctrl+,)") : qsTr("Listen and ask with Gemini (Ctrl+,)")
                active: ai.connected
                onClicked: win.showAiSettings()
            }
            IconButton {
                glyph: Icons.plus
                tip: qsTr("Add a folder of books (A)")
                onClicked: picker.chooseFolders()
            }
        }

        // Reading new books in: a thin line along the bar.
        Rectangle {
            anchors.bottom: parent.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            height: 1
            color: Ui.tint(theme.text, grid.contentY > grid.originY + 4 ? 0.08 : 0)
        }
        Rectangle {
            visible: library.scanning && library.scanTotal > 0
            anchors.bottom: parent.bottom
            height: 2
            width: parent.width * (library.scanTotal > 0 ? library.scanDone / library.scanTotal : 0)
            color: theme.accent
            Behavior on width { NumberAnimation { duration: 200 } }
        }
    }

    Label {
        visible: library.scanning
        anchors.top: bar.bottom
        anchors.topMargin: 6
        anchors.right: parent.right
        anchors.rightMargin: 32
        z: 2
        text: library.scanTotal > 0 ? qsTr("Reading %1 of %2 books…").arg(library.scanDone).arg(library.scanTotal)
                                    : qsTr("Looking for books…")
        color: theme.textTertiary
        font.pixelSize: 12
    }

    // --- The books -------------------------------------------------------
    GridView {
        id: grid
        anchors.top: bar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 18
        anchors.rightMargin: 18
        visible: library.bookCount > 0
        clip: true
        focus: true
        keyNavigationEnabled: true
        keyNavigationWraps: false
        boundsBehavior: Flickable.StopAtBounds
        cacheBuffer: 1200
        currentIndex: -1

        readonly property int columns: Math.max(2, Math.floor(width / 190))
        cellWidth: Math.floor(width / columns)
        cellHeight: Math.round((cellWidth - 28) * 1.5) + 14 + 64

        model: library.works

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        header: Column {
            width: grid.width
            topPadding: 12
            spacing: 0

            ContinueShelf {
                x: 14
                width: grid.width - 28
                visible: library.continueReading.length > 0 && library.works.searchText === ""
                height: visible ? implicitHeight : 0
                books: library.continueReading
                onOpen: (bookId) => view.openBook(bookId)
            }
            Item { width: 1; height: library.continueReading.length > 0 && library.works.searchText === "" ? 26 : 0 }
            Label {
                x: 14
                text: library.works.searchText !== ""
                      ? qsTr("%n matches", "", library.works.count)
                      : library.works.grouped ? qsTr("All books · %n works", "", library.works.count) : qsTr("All books")
                color: theme.text
                font.pixelSize: 15
                font.weight: Font.DemiBold
                bottomPadding: 4
            }
        }

        delegate: BookCard {
            onActivated: view.activate(index)
            onDetailsRequested: view.showDetails(index)
        }

        Keys.onReturnPressed: view.activate(currentIndex)
        Keys.onEnterPressed: view.activate(currentIndex)
        onActiveFocusChanged: if (activeFocus && currentIndex < 0 && count > 0) currentIndex = 0

        Label {
            anchors.centerIn: parent
            visible: grid.count === 0 && library.works.searchText !== ""
            text: qsTr("No books match “%1”").arg(library.works.searchText)
            color: theme.textSecondary
            font.pixelSize: 15
        }
    }

    EmptyLibrary {
        anchors.centerIn: parent
        visible: library.bookCount === 0 && !library.scanning
        onAddFolders: picker.chooseFolders()
        onAddBooks: picker.chooseBooks()
    }

    BusyIndicator {
        anchors.centerIn: parent
        visible: library.bookCount === 0 && library.scanning
        running: visible
    }

    BookDetails {
        id: details
        anchors.fill: parent
        z: 10
        onRead: (bookId) => { details.close(); view.openBook(bookId) }
        onClosed: grid.forceActiveFocus()
    }
}
