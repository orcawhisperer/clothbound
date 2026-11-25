import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

import "../components/Format.js" as Format
import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui

// The library: 52 px calm toolbar, Continue shelf on Reading now & All books,
// 6-column cover grid or compact list view, J2 inline arriving skeletons and
// stoppable scan pill, watched-folder manager, and reading stats (01_library,
// 10_j1_empty_library, 11_j2_books_arriving).
FocusScope {
    id: view

    property string viewMode: "grid"
    property int statsRevision: 0
    property int pulsedBookId: 0
    property real nowEpoch: Math.floor(Date.now() / 1000)
    readonly property var continueBooks: library.continueReading

    readonly property bool typing: search.activeFocus || collectionDialog.open || details.addingShelf
                                   || addUrlOpen || catalogsOpen
    readonly property bool detailsOpen: details.open
    readonly property bool foldersOpen: foldersSheet.open
    readonly property bool statsOpen: statsSheet.open
    readonly property bool collectionDialogOpen: collectionDialog.open
    readonly property bool addUrlOpen: addUrlSheet.open
    readonly property bool catalogsOpen: catalogsSheet.open
    readonly property bool menuOpen: sortMenu.opened || seriesMenu.opened || filterMenu.opened || moreMenu.opened
    property bool sidebarCollapsed: false
    // What's typed in the search field; Ctrl+K takes it to the palette, which
    // also searches inside every book.
    readonly property string searchText: search.text
    function toggleSidebar() { sidebarCollapsed = !sidebarCollapsed }
    readonly property string activeCollectionName: {
        const cid = library.works.collectionId
        if (cid <= 0)
            return ""
        const col = library.collections.find(c => c.id === cid)
        return col ? col.name : qsTr("Collection")
    }
    // The toolbar names what the grid shows: a shelf, a collection, or a search.
    readonly property string sectionTitle: {
        const works = library.works
        if (works.searchText !== "")
            return qsTr("Search")
        if (works.collectionId !== 0)
            return view.activeCollectionName
        if (works.statusFilter === "" && works.seriesFilter === "") {
            if (works.formatFilter === "epub")
                return qsTr("Books")
            if (works.formatFilter === "pdf")
                return qsTr("Papers & PDFs")
        }
        return works.statusFilter === "reading" ? qsTr("Reading")
             : works.statusFilter === "recent" ? qsTr("Recently added")
             : works.statusFilter === "articles" ? qsTr("Articles")
             : works.statusFilter === "want" ? qsTr("Want to read")
             : works.statusFilter === "finished" ? qsTr("Finished")
             : works.statusFilter === "downloaded" ? qsTr("Downloaded")
             : works.statusFilter === "audiobooks" ? qsTr("Audiobooks")
             : works.statusFilter === "listening" ? qsTr("Listening")
             : works.statusFilter === "unread" ? qsTr("Not started")
             : qsTr("All books")
    }
    readonly property int currentRow: viewMode === "list" ? listView.currentIndex : grid.currentIndex
    readonly property string booksCountSummary: {
        const _lang = translator.language
        const works = library.works
        if (works.formatFilter === "pdf" && works.collectionId === 0 && works.statusFilter === "" && works.searchText === "")
            return qsTr("%n PDFs", "", works.count)
        const pdfs = library.shelfCounts.pdf || 0
        const showPdfs = !works.hasActiveFilters && works.searchText === "" && pdfs > 0
        const base = works.grouped && !works.hasActiveFilters && works.searchText === ""
                     ? qsTr("%n books", "", library.bookCount)
                     : qsTr("%n books", "", works.count)
        return showPdfs ? (base + " · " + qsTr("%n PDFs", "", pdfs)) : base
    }

    readonly property string statsIcon: Icons.stats
    readonly property string refreshIcon: "M19 12 A7 7 0 1 1 16.95 7.05 M19 4.5 V8.5 H15"

    signal openBook(int bookId)
    signal listenBook(int bookId)
    signal paletteRequested(string text)

    Timer {
        id: pulseTimer
        interval: 2200
        onTriggered: view.pulsedBookId = 0
    }
    Timer {
        interval: 60000
        repeat: true
        running: view.visible
        onTriggered: view.nowEpoch = Math.floor(Date.now() / 1000)
    }

    function startBookDrag(bookId, title, cover, sceneX, sceneY) {
        const local = view.mapFromItem(null, sceneX, sceneY)
        bookDragProxy.dragBookId = bookId
        bookDragProxy.dragBookTitle = title
        bookDragProxy.dragBookCover = cover
        bookDragProxy.x = local.x - bookDragProxy.width / 2
        bookDragProxy.y = local.y - bookDragProxy.height / 2
        bookDragProxy.dragging = true
    }
    function moveBookDrag(sceneX, sceneY) {
        if (!bookDragProxy.dragging)
            return
        const local = view.mapFromItem(null, sceneX, sceneY)
        bookDragProxy.x = local.x - bookDragProxy.width / 2
        bookDragProxy.y = local.y - bookDragProxy.height / 2
    }
    function finishBookDrag() {
        if (!bookDragProxy.dragging)
            return
        bookDragProxy.Drag.drop()
        bookDragProxy.dragging = false
        bookDragProxy.dragBookId = 0
    }

    Connections {
        target: library
        function onBooksChanged() {
            view.statsRevision++
            view.nowEpoch = Math.floor(Date.now() / 1000)
        }
        function onReadingStatsChanged() { view.statsRevision++ }
        function onBooksAdded(firstBookId, count, firstTitle, targetCollectionId) {
            view.nowEpoch = Math.floor(Date.now() / 1000)
            if (typeof win !== "undefined" && win.readerOpen)
                return
            const col = targetCollectionId > 0 ? library.collections.find(c => c.id === targetCollectionId) : null
            const title = firstTitle !== "" ? firstTitle : qsTr("book")
            if (count === 1) {
                toast.show(col ? qsTr("Added “%1” to %2").arg(title).arg(col.name)
                               : qsTr("Added “%1”").arg(title))
            } else if (count > 1) {
                toast.show(col ? qsTr("Added %1 books to %2").arg(count).arg(col.name)
                               : qsTr("Added %n books", "", count))
            }
            if (firstBookId > 0) {
                if (targetCollectionId > 0 && library.works.collectionId === targetCollectionId
                        && library.works.rowOfBook(firstBookId) < 0) {
                    if (search.text !== "")
                        search.text = ""
                    library.works.clearFilters()
                    library.works.collectionId = targetCollectionId
                }
                if (library.works.rowOfBook(firstBookId) >= 0) {
                    view.select(firstBookId)
                    view.pulsedBookId = firstBookId
                    pulseTimer.restart()
                }
            }
        }
    }

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
    function focusGrid() {
        if (viewMode === "list")
            listView.forceActiveFocus()
        else
            grid.forceActiveFocus()
    }
    function toggleViewMode() {
        const prev = currentRow
        viewMode = viewMode === "grid" ? "list" : "grid"
        if (prev >= 0) {
            if (viewMode === "list")
                listView.currentIndex = prev
            else
                grid.currentIndex = prev
        }
        focusGrid()
        toast.show(viewMode === "list" ? qsTr("List view") : qsTr("Cover grid"))
    }
    function cycleSort() {
        const orders = ["recent", "title", "author", "series", "added", "progress"]
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
             : order === "series" ? qsTr("Series")
             : order === "added" ? qsTr("Date added")
             : order === "progress" ? qsTr("Progress") : qsTr("Recent")
    }
    function moveSelection(key) {
        const activeView = viewMode === "list" ? listView : grid
        if (activeView.count === 0)
            return
        if (activeView.currentIndex < 0) {
            activeView.currentIndex = 0
        } else if (viewMode === "list") {
            if (key === "up" || key === "left")
                listView.decrementCurrentIndex()
            else if (key === "down" || key === "right")
                listView.incrementCurrentIndex()
        } else {
            if (key === "left") grid.moveCurrentIndexLeft()
            else if (key === "right") grid.moveCurrentIndexRight()
            else if (key === "up") grid.moveCurrentIndexUp()
            else if (key === "down") grid.moveCurrentIndexDown()
        }
        activeView.forceActiveFocus()
    }
    function closeDetails() { details.close() }
    function openFolders() {
        statsSheet.close()
        addUrlSheet.close()
        catalogsSheet.close()
        foldersSheet.show()
    }
    function closeFolders() { foldersSheet.close() }
    function openStats() {
        foldersSheet.close()
        addUrlSheet.close()
        catalogsSheet.close()
        statsRevision++
        statsSheet.show()
    }
    function closeStats() { statsSheet.close() }
    function openAddUrl(initialUrl) {
        foldersSheet.close()
        statsSheet.close()
        catalogsSheet.close()
        addUrlSheet.show(initialUrl || "")
    }
    function closeAddUrl() { addUrlSheet.close() }
    function openCatalogs() {
        foldersSheet.close()
        statsSheet.close()
        addUrlSheet.close()
        catalogsSheet.show()
    }
    function closeCatalogs() { catalogsSheet.close() }
    function closeCollectionDialog() { collectionDialog.close() }

    // After reading, the book just closed is the one selected.
    function select(bookId) {
        const row = library.works.rowOfBook(bookId)
        if (row >= 0) {
            if (viewMode === "list") {
                listView.currentIndex = row
                if (row === 0)
                    listView.positionViewAtBeginning()
                else
                    listView.positionViewAtIndex(row, ListView.Contain)
            } else {
                grid.currentIndex = row
                // The first row keeps the continue-reading shelf above it in view.
                if (row < grid.columns)
                    grid.positionViewAtBeginning()
                else
                    grid.positionViewAtIndex(row, GridView.Contain)
            }
        }
        focusGrid()
    }

    function formatDurationSeconds(seconds) {
        if (!seconds || seconds <= 0)
            return qsTr("0 min")
        const mins = Math.round(seconds / 60)
        if (mins < 1)
            return qsTr("Under 1 min")
        return Format.duration(mins)
    }

    function folderBasename(path) {
        if (!path)
            return ""
        const trimmed = path.endsWith("/") && path.length > 1 ? path.slice(0, -1) : path
        const idx = trimmed.lastIndexOf("/")
        return idx >= 0 ? trimmed.slice(idx + 1) : trimmed
    }

    Component.onCompleted: focusGrid()

    // --- Sidebar ---------------------------------------------------------
    LibrarySidebar {
        id: sidebar
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        collapsed: view.sidebarCollapsed
        onToggleRequested: view.toggleSidebar()
        onCreateCollection: collectionDialog.showCreate()
        onRenameCollection: (id, name) => collectionDialog.showRename(id, name)
        onAddBooksToCollection: (id) => {
            if (search.text !== "")
                search.text = ""
            library.works.clearFilters()
            library.works.collectionId = id
            picker.chooseBooks()
        }
        onBookDroppedOnCollection: (collectionId, collectionName) => {
            if (bookDragProxy.dragBookId > 0) {
                const bid = bookDragProxy.dragBookId
                const title = bookDragProxy.dragBookTitle !== "" ? bookDragProxy.dragBookTitle : qsTr("book")
                if (library.addToCollection(bid, collectionId))
                    toast.show(qsTr("Added “%1” to %2").arg(title).arg(collectionName))
            }
        }
        onFilterNavigated: {
            if (search.text !== "")
                search.text = ""
            grid.positionViewAtBeginning()
            listView.positionViewAtBeginning()
        }
        onSettingsRequested: win.showSettings()
        onCatalogsRequested: view.openCatalogs()
        onAddUrlRequested: view.openAddUrl()
    }

    // Everything right of the sidebar, for centring the empty states.
    Item {
        id: contentArea
        anchors.top: bar.bottom
        anchors.bottom: parent.bottom
        anchors.left: sidebar.right
        anchors.right: parent.right
    }

    // --- Toolbar (52 px, persistent hairline, J2 scan pill) --------------
    Item {
        id: bar
        anchors.top: parent.top
        anchors.left: sidebar.right
        anchors.right: parent.right
        height: 52

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 28
            anchors.rightMargin: 20
            spacing: 10

            Label {
                text: view.sectionTitle
                color: theme.ink1
                font.pixelSize: 15
                font.weight: Font.DemiBold
                font.letterSpacing: -0.15
                elide: Text.ElideRight
                Layout.maximumWidth: Math.max(120, bar.width * 0.3)
            }

            Item { Layout.fillWidth: true }

            // J2 right-aligned scanning progress pill with Stop action.
            Rectangle {
                visible: library.scanning
                Layout.preferredHeight: 30
                Layout.preferredWidth: scanRow.implicitWidth + 18
                radius: Ui.radius(15, theme.radius)
                color: Ui.tint(theme.ink1, 0.06)
                Accessible.role: Accessible.StatusBar

                Row {
                    id: scanRow
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 12
                    spacing: 10

                    Item {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14
                        height: 14
                        Rectangle {
                            anchors.fill: parent
                            radius: 7
                            color: "transparent"
                            border.width: 2
                            border.color: Ui.tint(theme.ink1, 0.15)
                        }
                        BusyIndicator {
                            anchors.fill: parent
                            padding: 0
                            running: library.scanning
                        }
                    }

                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: library.scanTotal > 0
                              ? qsTr("Adding books · %1 of %2")
                                    .arg(library.scanDone.toLocaleString(Qt.locale(), "f", 0))
                                    .arg(library.scanTotal.toLocaleString(Qt.locale(), "f", 0))
                              : qsTr("Looking for books…")
                        color: theme.ink1
                        font.pixelSize: 12
                        font.features: ({ "tnum": 1 })
                    }

                    AbstractButton {
                        id: stopScanBtn
                        anchors.verticalCenter: parent.verticalCenter
                        implicitHeight: 22
                        implicitWidth: stopLbl.implicitWidth + 16
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: qsTr("Stop scanning")
                        onClicked: library.cancelScan()

                        background: Rectangle {
                            radius: Ui.radius(11, theme.radius)
                            color: stopScanBtn.pressed ? Ui.tint(theme.ink1, 0.14)
                                 : stopScanBtn.hovered ? Ui.tint(theme.ink1, 0.09) : "transparent"
                            border.width: stopScanBtn.visualFocus ? 2 : 0
                            border.color: theme.accent
                        }
                        contentItem: Label {
                            id: stopLbl
                            text: qsTr("Stop")
                            color: stopScanBtn.hovered ? theme.ink1 : theme.ink3
                            font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
            }

            // Covers | List toggle
            Segmented {
                visible: library.bookCount > 0
                compact: true
                model: [{ value: "grid", label: qsTr("Covers (V)"), glyph: Icons.grid },
                        { value: "list", label: qsTr("List (V)"), glyph: Icons.list }]
                current: view.viewMode
                onActivated: view.toggleViewMode()
            }

            // Sort dropdown (Recent ▾); hidden on Recently added (always newest-first).
            AbstractButton {
                id: sortButton
                visible: library.bookCount > 0 && library.works.statusFilter !== "recent"
                Layout.preferredHeight: 28
                implicitWidth: sortRow.implicitWidth + 20
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.name: qsTr("Sort order: %1").arg(view.sortLabel(library.works.sortOrder))
                background: Rectangle {
                    radius: Ui.radius(7, theme.radius)
                    color: sortButton.pressed || sortMenu.opened ? theme.fillPressed
                         : sortButton.hovered ? theme.fill : "transparent"
                    border.width: sortButton.visualFocus ? 2 : 0
                    border.color: theme.accent
                }
                contentItem: Item {
                    Row {
                        id: sortRow
                        anchors.centerIn: parent
                        spacing: 6
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: view.sortLabel(library.works.sortOrder)
                            color: theme.ink2
                            font.pixelSize: 13
                        }
                        Glyph {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 10
                            height: 10
                            stroke: 2.2
                            path: Icons.chevronDown
                            color: theme.ink3
                        }
                    }
                }
                onClicked: sortMenu.opened ? sortMenu.close() : sortMenu.open()
                ToolTip.visible: hovered && !sortMenu.opened
                ToolTip.text: qsTr("Order (S)")
                ToolTip.delay: 600

                Popup {
                    id: sortMenu
                    y: sortButton.height + 6
                    x: sortButton.width - width
                    width: 220
                    padding: 5
                    focus: true
                    onClosed: view.focusGrid()
                    contentItem: Column {
                        spacing: 1
                        Repeater {
                            model: ["recent", "title", "author", "series", "added", "progress"]
                            delegate: MenuRow {
                                required property string modelData
                                text: view.sortLabel(modelData)
                                checked: library.works.sortOrder === modelData
                                onClicked: { library.works.sortOrder = modelData; sortMenu.close() }
                            }
                        }
                        MenuSeparator { width: parent.width }
                        MenuRow {
                            text: qsTr("Group editions")
                            hint: "G"
                            checked: library.works.grouped
                            onClicked: { library.works.grouped = !library.works.grouped; sortMenu.close() }
                        }
                    }
                }
            }

            // Format / series filter menu (preserved Clothbound power feature)
            IconButton {
                id: filterButton
                visible: library.bookCount > 0
                glyph: Icons.filter
                active: library.works.seriesFilter !== ""
                        || (library.works.formatFilter !== "" && (library.works.collectionId > 0 || library.works.statusFilter !== ""))
                tip: qsTr("Filter by format or series")
                onClicked: filterMenu.opened ? filterMenu.close() : filterMenu.open()

                Popup {
                    id: filterMenu
                    y: filterButton.height + 6
                    x: filterButton.width - width
                    width: 220
                    padding: 5
                    focus: true
                    onClosed: view.focusGrid()
                    contentItem: Column {
                        spacing: 1
                        MenuRow { text: qsTr("Any format"); checked: library.works.formatFilter === ""; onClicked: library.works.formatFilter = "" }
                        MenuRow { text: qsTr("EPUB only"); checked: library.works.formatFilter === "epub"; onClicked: library.works.formatFilter = "epub" }
                        MenuRow { text: qsTr("PDF only"); checked: library.works.formatFilter === "pdf"; onClicked: library.works.formatFilter = "pdf" }
                        MenuSeparator { width: parent.width }
                        MenuRow {
                            text: library.works.seriesFilter === "" ? qsTr("Series…")
                                : library.works.seriesFilter === "*" ? qsTr("Books in a series") : library.works.seriesFilter
                            checked: library.works.seriesFilter !== ""
                            onClicked: {
                                filterMenu.close()
                                if (library.works.seriesList.length > 0) {
                                    const pos = filterButton.mapToItem(view, filterButton.width, filterButton.height + 6)
                                    seriesMenu.x = Math.max(16, pos.x - seriesMenu.width)
                                    seriesMenu.y = pos.y
                                    seriesMenu.open()
                                } else {
                                    library.works.seriesFilter = library.works.seriesFilter === "" ? "*" : ""
                                }
                            }
                        }
                    }
                }
            }

            // 260×30 Search box with clickable Ctrl K badge
            SearchBox {
                id: search
                Layout.preferredWidth: Math.min(260, Math.max(180, view.width * 0.24))
                visible: library.bookCount > 0
                placeholder: qsTr("Search books, authors, text")
                shortcutHint: "Ctrl K"
                onTextChanged: library.works.searchText = text
                onPaletteRequested: view.paletteRequested(search.text)
                onAccepted: {
                    const activeView = view.viewMode === "list" ? listView : grid
                    if (activeView.count > 0)
                        activeView.currentIndex = 0
                    view.focusGrid()
                }
                onEscaped: view.focusGrid()
            }

            // More actions (Reading stats, Watched folders, Rescan, Add single books)
            IconButton {
                id: moreButton
                glyph: Icons.more
                tip: qsTr("More")
                onClicked: moreMenu.opened ? moreMenu.close() : moreMenu.open()

                Popup {
                    id: moreMenu
                    y: moreButton.height + 6
                    x: moreButton.width - width
                    width: 240
                    padding: 5
                    focus: true
                    onClosed: view.focusGrid()
                    contentItem: Column {
                        spacing: 1
                        MenuRow { text: qsTr("Reading stats"); onClicked: { moreMenu.close(); view.openStats() } }
                        MenuRow { text: qsTr("Watched folders…"); onClicked: { moreMenu.close(); view.openFolders() } }
                        MenuRow { text: qsTr("Look for new books"); onClicked: { moreMenu.close(); library.rescan() } }
                        MenuSeparator { width: parent.width }
                        MenuRow {
                            text: library.works.collectionId > 0
                                  ? qsTr("Add books to %1…").arg(view.activeCollectionName)
                                  : qsTr("Add books…")
                            onClicked: { moreMenu.close(); picker.chooseBooks() }
                        }
                        MenuRow {
                            text: qsTr("Add from URL…")
                            hint: "Ctrl ⇧ U"
                            onClicked: { moreMenu.close(); view.openAddUrl() }
                        }
                        MenuRow {
                            text: qsTr("Browse catalogs…")
                            hint: "Ctrl ⇧ O"
                            onClicked: { moreMenu.close(); view.openCatalogs() }
                        }
                        MenuSeparator { width: parent.width }
                        MenuRow {
                            text: qsTr("Settings…")
                            hint: "Ctrl ,"
                            onClicked: { moreMenu.close(); win.showSettings() }
                        }
                    }
                }
            }

            TextButton {
                visible: library.works.collectionId > 0
                size: "compact"
                glyph: Icons.plus
                text: qsTr("Add books")
                Accessible.name: qsTr("Add books to %1").arg(view.activeCollectionName)
                onClicked: picker.chooseBooks()
            }

            IconButton {
                visible: library.works.collectionId === 0
                glyph: Icons.plus
                tip: qsTr("Add books or folders (A)")
                onClicked: picker.chooseFolders()
            }
        }

        // Thin scan progress accent hairline along the toolbar bottom.
        Rectangle {
            visible: library.scanning && library.scanTotal > 0
            anchors.bottom: parent.bottom
            height: 2
            width: parent.width * (library.scanTotal > 0 ? library.scanDone / library.scanTotal : 0)
            color: theme.accent
            Behavior on width { enabled: !theme.reducedMotion; NumberAnimation { duration: 200 } }
        }
        // Persistent hairline separator (01_library.html).
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: theme.hairline
        }
    }

    // Series picker popup
    Popup {
        id: seriesMenu
        width: 240
        padding: 6
        focus: true
        onClosed: view.focusGrid()
        contentItem: Column {
            spacing: 2
            MenuRow {
                text: qsTr("All books in a series")
                checked: library.works.seriesFilter === "*"
                onClicked: {
                    library.works.seriesFilter = library.works.seriesFilter === "*" ? "" : "*"
                    if (library.works.seriesFilter !== "")
                        library.works.sortOrder = "series"
                    seriesMenu.close()
                }
            }
            MenuSeparator { width: parent.width }
            Repeater {
                model: library.works.seriesList
                delegate: MenuRow {
                    required property string modelData
                    text: modelData
                    checked: library.works.seriesFilter === modelData
                    onClicked: {
                        library.works.seriesFilter = modelData
                        library.works.sortOrder = "series"
                        seriesMenu.close()
                    }
                }
            }
            Rectangle {
                visible: library.works.seriesFilter !== ""
                width: parent.width
                height: 1
                color: theme.hairline
            }
            MenuRow {
                visible: library.works.seriesFilter !== ""
                text: qsTr("Show all books")
                checked: false
                onClicked: {
                    library.works.seriesFilter = ""
                    seriesMenu.close()
                }
            }
        }
    }

    // --- Cover grid view (6 cols @ 1280 px, 24 px col gap, 28 px outer) --
    GridView {
        id: grid
        anchors.top: bar.bottom
        anchors.left: sidebar.right
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        visible: library.bookCount > 0 && view.viewMode === "grid"
        clip: true
        focus: view.viewMode === "grid"
        keyNavigationEnabled: true
        keyNavigationWraps: false
        boundsBehavior: Flickable.StopAtBounds
        maximumFlickVelocity: 4500
        flickDeceleration: 2200
        pixelAligned: true
        cacheBuffer: 480
        currentIndex: -1

        readonly property int columns: Math.max(2, Math.floor((width + 24) / 172))
        readonly property int pendingSlots: (library.scanning && library.scanTotal > library.scanDone
                                             && !library.works.hasActiveFilters && library.works.searchText === "")
                                            ? Math.min(columns * 2, library.scanTotal - library.scanDone) : 0
        cellWidth: Math.floor(width / columns)
        cellHeight: Math.round((cellWidth - 24) * 1.5) + 12 + 56

        model: view.viewMode === "grid" ? library.works : null
        Accessible.role: Accessible.List
        Accessible.name: view.sectionTitle

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        SmoothWheel { stepSize: 165 }

        header: Column {
            width: grid.width
            topPadding: 28
            spacing: 0
            readonly property bool showShelf: view.continueBooks.length > 0
                                              && library.works.searchText === ""
                                              && library.works.collectionId === 0
                                              && library.works.formatFilter === ""
                                              && library.works.seriesFilter === ""
                                              && (library.works.statusFilter === "" || library.works.statusFilter === "reading")

            ContinueShelf {
                x: 12
                width: grid.width - 24
                visible: parent.showShelf
                height: visible ? implicitHeight : 0
                books: grid.visible && parent.showShelf ? view.continueBooks : []
                onOpen: (bookId) => view.openBook(bookId)
                onListen: (bookId) => view.listenBook(bookId)
                onLeaveDown: view.focusGrid()
            }
            Item { width: 1; height: parent.showShelf ? 32 : 0 }

            // J2 quiet reassurance line while books arrive into the grid.
            Item {
                x: 12
                width: grid.width - 24
                visible: library.scanning && library.bookCount > 0
                height: visible ? scanHintLabel.implicitHeight + 14 : 0
                Label {
                    id: scanHintLabel
                    width: parent.width
                    text: qsTr("Open any book while the rest arrive.")
                    color: theme.ink3
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
            }

            // Under the Continue shelf, "All books" + right-aligned "N books · M PDFs".
            RowLayout {
                x: 12
                width: grid.width - 24
                visible: parent.showShelf
                height: visible ? implicitHeight + 8 : 0
                spacing: 12

                Label {
                    Layout.fillWidth: true
                    text: qsTr("All books")
                    color: theme.ink1
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }
                Label {
                    text: view.booksCountSummary
                    color: theme.ink4
                    font.pixelSize: 12
                    font.features: ({ "tnum": 1 })
                }
            }
        }

        delegate: BookCard {
            nowEpoch: view.nowEpoch
            highlighted: bookId === view.pulsedBookId
            onActivated: view.activate(index)
            onDetailsRequested: view.showDetails(index)
            onDragStarted: (id, title, cover, sx, sy) => view.startBookDrag(id, title, cover, sx, sy)
            onDragMoved: (sx, sy) => view.moveBookDrag(sx, sy)
            onDragEnded: view.finishBookDrag()
        }

        // Reserve scroll height for any extra skeleton rows below arrived books.
        footer: Item {
            width: grid.width
            readonly property int existingRows: grid.columns > 0 ? Math.ceil(grid.count / grid.columns) : 0
            readonly property int totalRows: grid.columns > 0 ? Math.ceil((grid.count + grid.pendingSlots) / grid.columns) : 0
            height: Math.max(24, (totalRows - existingRows) * grid.cellHeight + 24)
        }

        // J2 inline trailing skeletons: placed directly in the slots after the
        // last arrived book on the SAME grid row, without polluting WorksModel.
        Item {
            parent: grid.contentItem
            visible: grid.pendingSlots > 0
            z: 1
            Repeater {
                model: grid.pendingSlots
                delegate: SkeletonCard {
                    required property int index
                    readonly property int slot: grid.count + index
                    x: (slot % grid.columns) * grid.cellWidth
                    y: (grid.headerItem ? grid.headerItem.height : 0) + Math.floor(slot / grid.columns) * grid.cellHeight
                    width: grid.cellWidth
                }
            }
        }

        Keys.onReturnPressed: view.activate(currentIndex)
        Keys.onEnterPressed: view.activate(currentIndex)
        onActiveFocusChanged: if (activeFocus && currentIndex < 0 && count > 0) currentIndex = 0
    }

    // --- Compact list view -----------------------------------------------
    ListView {
        id: listView
        anchors.top: bar.bottom
        anchors.left: sidebar.right
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 24
        anchors.rightMargin: 24
        visible: library.bookCount > 0 && view.viewMode === "list"
        clip: true
        focus: view.viewMode === "list"
        keyNavigationEnabled: true
        keyNavigationWraps: false
        boundsBehavior: Flickable.StopAtBounds
        maximumFlickVelocity: 4500
        flickDeceleration: 2200
        pixelAligned: true
        cacheBuffer: 420
        spacing: 2
        currentIndex: -1
        model: view.viewMode === "list" ? library.works : null
        Accessible.role: Accessible.List
        Accessible.name: view.sectionTitle

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
        SmoothWheel { stepSize: 115 }

        header: Column {
            width: listView.width
            topPadding: 24
            bottomPadding: 8
            spacing: 0
            readonly property bool showShelf: view.continueBooks.length > 0
                                              && library.works.searchText === ""
                                              && library.works.collectionId === 0
                                              && library.works.formatFilter === ""
                                              && library.works.seriesFilter === ""
                                              && (library.works.statusFilter === "" || library.works.statusFilter === "reading")

            ContinueShelf {
                x: 4
                width: listView.width - 8
                visible: parent.showShelf
                height: visible ? implicitHeight : 0
                books: listView.visible && parent.showShelf ? view.continueBooks : []
                onOpen: (bookId) => view.openBook(bookId)
                onListen: (bookId) => view.listenBook(bookId)
                onLeaveDown: view.focusGrid()
            }
            Item { width: 1; height: parent.showShelf ? 28 : 0 }
            RowLayout {
                x: 4
                width: listView.width - 8
                spacing: 12
                Label {
                    Layout.fillWidth: true
                    text: library.works.searchText !== ""
                          ? qsTr("%n matches", "", library.works.count)
                          : view.sectionTitle
                    color: theme.ink1
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }
                Label {
                    text: view.booksCountSummary
                    color: theme.ink4
                    font.pixelSize: 12
                    font.features: ({ "tnum": 1 })
                }
            }
        }

        delegate: Item {
            id: listRow
            required property int index
            required property string key
            required property int bookId
            required property string title
            required property string author
            required property string editionLine
            required property int editions
            required property string cover
            required property real progress
            required property bool finished
            required property string format
            required property string problem
            required property string series
            required property real seriesIndex
            required property int minutesLeft
            required property real addedAt

            readonly property bool selected: ListView.isCurrentItem && listView.activeFocus
            readonly property bool highlighted: bookId === view.pulsedBookId
            readonly property bool isNewArrival: !finished && progress <= 0 && addedAt > 0
                                                 && view.nowEpoch > 0 && (view.nowEpoch - addedAt) <= 172800

            width: listView.width
            height: 68
            Accessible.role: Accessible.ListItem
            Accessible.selected: listRow.selected
            Accessible.name: {
                const parts = [listRow.title]
                if (listRow.author !== "")
                    parts.push(listRow.author)
                if (listRow.problem !== "")
                    parts.push(listRow.problem)
                else if (listRow.finished)
                    parts.push(qsTr("Finished"))
                else if (listRow.progress > 0.005)
                    parts.push(qsTr("%1%").arg(Math.round(listRow.progress * 100).toLocaleString(Qt.locale(), "f", 0)))
                else
                    parts.push(qsTr("Unread"))
                return parts.join(", ")
            }

            Rectangle {
                anchors.fill: parent
                radius: Ui.radius(8, theme.radius)
                color: listRow.highlighted ? Ui.tint(theme.accent, theme.light ? 0.16 : 0.24)
                     : listRow.selected ? Ui.tint(theme.accent, theme.light ? 0.14 : 0.2)
                     : rowHover.hovered ? Ui.tint(theme.ink1, 0.05) : "transparent"
                border.width: listRow.highlighted ? 1.5 : (listRow.selected ? 1 : 0)
                border.color: theme.accent
                Behavior on color {
                    enabled: !theme.reducedMotion
                    ColorAnimation { duration: 220 }
                }
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 14
                spacing: 14

                CoverImage {
                    Layout.preferredWidth: 36
                    Layout.minimumWidth: 36
                    Layout.maximumWidth: 36
                    Layout.preferredHeight: 52
                    Layout.alignment: Qt.AlignVCenter
                    source: listRow.cover
                    title: listRow.title
                    author: listRow.author
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    Layout.preferredWidth: 0
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 2

                    RowLayout {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: 0
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.preferredWidth: 0
                            text: listRow.title
                            textFormat: Text.PlainText
                            color: listRow.problem !== "" ? theme.ink3 : theme.ink1
                            font.pixelSize: 14
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                        }
                        Label {
                            visible: listRow.isNewArrival
                            text: qsTr("New")
                            color: theme.accentInk
                            font.pixelSize: 10
                            font.weight: Font.DemiBold
                            leftPadding: 6
                            rightPadding: 6
                            topPadding: 1.5
                            bottomPadding: 1.5
                            background: Rectangle {
                                radius: Ui.radius(8, theme.radius)
                                color: theme.accentSoft
                            }
                        }
                        Label {
                            visible: listRow.series !== ""
                            Layout.maximumWidth: 180
                            text: listRow.seriesIndex > 0
                                ? (listRow.series + " #" + (Math.floor(listRow.seriesIndex) === listRow.seriesIndex ? Math.floor(listRow.seriesIndex) : listRow.seriesIndex))
                                : listRow.series
                            textFormat: Text.PlainText
                            elide: Text.ElideRight
                            color: theme.accentInk
                            font.pixelSize: 11
                            leftPadding: 7
                            rightPadding: 7
                            topPadding: 2
                            bottomPadding: 2
                            background: Rectangle {
                                radius: Ui.radius(10, theme.radius)
                                color: Ui.tint(theme.accent, 0.14)
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        Layout.preferredWidth: 0
                        text: {
                            if (listRow.problem !== "")
                                return listRow.problem
                            const parts = []
                            if (listRow.author !== "")
                                parts.push(listRow.author)
                            if (listRow.editions > 1)
                                parts.push(qsTr("%n editions", "", listRow.editions))
                            else if (listRow.editionLine !== "")
                                parts.push(listRow.editionLine)
                            return parts.join(" · ")
                        }
                        textFormat: Text.PlainText
                        color: listRow.problem !== "" ? theme.danger : theme.ink3
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }

                // Format badge (fixed-width column so EPUB / PDF align across all rows)
                Label {
                    Layout.preferredWidth: 48
                    Layout.minimumWidth: 48
                    Layout.maximumWidth: 48
                    Layout.alignment: Qt.AlignVCenter
                    horizontalAlignment: Text.AlignHCenter
                    text: listRow.format.toUpperCase()
                    color: theme.ink3
                    font.pixelSize: 10
                    font.weight: Font.Bold
                    font.letterSpacing: 0.5
                    leftPadding: 6
                    rightPadding: 6
                    topPadding: 3
                    bottomPadding: 3
                    background: Rectangle {
                        radius: Ui.radius(4, theme.radius)
                        color: Ui.tint(theme.ink1, 0.06)
                    }
                }

                // Progress / status column
                ColumnLayout {
                    Layout.preferredWidth: 112
                    Layout.minimumWidth: 112
                    Layout.maximumWidth: 112
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 4

                    RowLayout {
                        Layout.fillWidth: true
                        Item { Layout.fillWidth: true }
                        Glyph {
                            visible: listRow.finished
                            Layout.preferredWidth: 14
                            Layout.preferredHeight: 14
                            stroke: 2.3
                            path: Icons.check
                            color: theme.accentInk
                        }
                        Label {
                            text: {
                                const _lang = translator.language
                                if (listRow.finished)
                                    return qsTr("Finished")
                                if (listRow.progress > 0.005)
                                    return qsTr("%1%").arg(Math.round(listRow.progress * 100).toLocaleString(Qt.locale(), "f", 0))
                                return qsTr("Unread")
                            }
                            color: listRow.finished ? theme.accentInk : theme.ink2
                            font.pixelSize: 12
                            font.weight: listRow.finished ? Font.DemiBold : Font.Normal
                            font.features: ({ "tnum": 1 })
                        }
                    }

                    Rectangle {
                        visible: listRow.progress > 0.005 && !listRow.finished
                        Layout.fillWidth: true
                        Layout.preferredHeight: 4
                        radius: 2
                        color: Ui.tint(theme.ink1, 0.10)
                        Rectangle {
                            width: parent.width * listRow.progress
                            height: parent.height
                            radius: 2
                            color: theme.accent
                        }
                    }

                    Label {
                        visible: listRow.progress > 0.005 && !listRow.finished && listRow.minutesLeft > 0
                        Layout.alignment: Qt.AlignRight
                        text: Format.duration(listRow.minutesLeft)
                        color: theme.ink4
                        font.pixelSize: 11
                        font.features: ({ "tnum": 1 })
                    }
                }
            }

            HoverHandler {
                id: rowHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: {
                    listView.currentIndex = listRow.index
                    view.activate(listRow.index)
                }
            }
            TapHandler {
                acceptedButtons: Qt.RightButton
                onTapped: {
                    listView.currentIndex = listRow.index
                    view.showDetails(listRow.index)
                }
            }
            DragHandler {
                target: null
                acceptedButtons: Qt.LeftButton
                onActiveChanged: {
                    if (active) {
                        const pt = centroid.scenePosition
                        view.startBookDrag(listRow.bookId, listRow.title, listRow.cover, pt.x, pt.y)
                    } else {
                        view.finishBookDrag()
                    }
                }
                onCentroidChanged: {
                    if (active) {
                        const pt = centroid.scenePosition
                        view.moveBookDrag(pt.x, pt.y)
                    }
                }
            }
        }

        Keys.onReturnPressed: view.activate(currentIndex)
        Keys.onEnterPressed: view.activate(currentIndex)
        onActiveFocusChanged: if (activeFocus && currentIndex < 0 && count > 0) currentIndex = 0
    }

    // Empty collection / shelf / filter / search state
    Column {
        id: emptyFilterState
        anchors.centerIn: contentArea
        width: Math.min(420, contentArea.width - 48)
        visible: library.bookCount > 0 && library.works.count === 0
        spacing: 12

        readonly property bool isEmptyCollection: library.works.collectionId > 0
                                                  && library.works.searchText === ""
                                                  && library.works.statusFilter === ""
                                                  && library.works.formatFilter === ""
                                                  && library.works.seriesFilter === ""
        readonly property bool isEmptyRecent: library.works.collectionId === 0
                                              && library.works.statusFilter === "recent"
                                              && library.works.searchText === ""
        readonly property bool isEmptyPdfs: library.works.collectionId === 0
                                            && library.works.formatFilter === "pdf"
                                            && library.works.statusFilter === ""
                                            && library.works.searchText === ""
        readonly property bool isEmptyEpubs: library.works.collectionId === 0
                                             && library.works.formatFilter === "epub"
                                             && library.works.statusFilter === ""
                                             && library.works.searchText === ""
        readonly property bool isEmptyArticles: library.works.collectionId === 0
                                                && library.works.statusFilter === "articles"
                                                && library.works.searchText === ""

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: library.works.searchText !== ""
                ? qsTr("No books match “%1”").arg(library.works.searchText)
                : emptyFilterState.isEmptyCollection
                  ? qsTr("No books in “%1” yet").arg(view.activeCollectionName)
                : emptyFilterState.isEmptyRecent
                  ? qsTr("Nothing added in the last 14 days")
                : emptyFilterState.isEmptyArticles
                  ? qsTr("No saved articles yet")
                : emptyFilterState.isEmptyPdfs
                  ? qsTr("No papers or PDFs yet")
                : emptyFilterState.isEmptyEpubs
                  ? qsTr("No EPUB books yet")
                : qsTr("No books match these filters")
            color: theme.ink1
            font.pixelSize: 15
            font.weight: Font.Medium
        }
        Label {
            width: parent.width
            visible: emptyFilterState.isEmptyCollection
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: qsTr("Add files here, drop books on the window, or drag any cover onto this collection.")
            color: theme.ink3
            font.pixelSize: 13
            lineHeight: 1.45
        }
        Row {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8
            TextButton {
                visible: emptyFilterState.isEmptyArticles
                primary: true
                glyph: Icons.link
                text: qsTr("Add from URL…")
                onClicked: view.openAddUrl()
            }
            TextButton {
                visible: emptyFilterState.isEmptyCollection || emptyFilterState.isEmptyPdfs || emptyFilterState.isEmptyEpubs
                primary: true
                glyph: Icons.plus
                text: emptyFilterState.isEmptyPdfs ? qsTr("Add PDFs…") : qsTr("Add books…")
                onClicked: picker.chooseBooks()
            }
            TextButton {
                visible: library.works.hasActiveFilters
                text: (emptyFilterState.isEmptyCollection || emptyFilterState.isEmptyRecent
                       || emptyFilterState.isEmptyArticles
                       || emptyFilterState.isEmptyPdfs || emptyFilterState.isEmptyEpubs)
                      ? qsTr("Browse all books") : qsTr("Clear filters")
                onClicked: library.works.clearFilters()
            }
        }
    }

    EmptyLibrary {
        anchors.centerIn: contentArea
        visible: library.bookCount === 0 && !library.scanning
        onAddFolders: picker.chooseFolders()
        onAddBooks: picker.chooseBooks()
        onBrowseCatalogs: view.openCatalogs()
        onAddFromUrl: view.openAddUrl()
    }

    // The first books on their way before any arrive: 6-col skeleton grid.
    Flow {
        id: firstArrivals
        anchors.fill: contentArea
        anchors.leftMargin: 16
        anchors.rightMargin: 16
        anchors.topMargin: 16
        visible: library.bookCount === 0 && library.scanning
        clip: true
        readonly property int columns: Math.max(2, Math.floor((width + 24) / 172))
        readonly property real cell: Math.floor(width / columns)
        Repeater {
            model: firstArrivals.visible ? Math.min(18, Math.max(firstArrivals.columns * 2, library.scanTotal)) : 0
            delegate: SkeletonCard { width: firstArrivals.cell }
        }
    }

    component SkeletonCard: Item {
        height: Math.round((width - 24) * 1.5) + 12 + 56
        Skeleton {
            x: 12
            y: 12
            width: parent.width - 24
            height: (parent.width - 24) * 1.5
            radius: Ui.radius(5, theme.radius)
        }
        Skeleton {
            x: 12
            y: (parent.width - 24) * 1.5 + 22
            width: (parent.width - 24) * 0.78
            height: 12
            radius: Ui.radius(4, theme.radius)
            shimmer: false
        }
        Skeleton {
            x: 12
            y: (parent.width - 24) * 1.5 + 40
            width: (parent.width - 24) * 0.48
            height: 10
            radius: Ui.radius(4, theme.radius)
            shimmer: false
        }
    }

    Loader {
        id: details
        anchors.fill: parent
        z: 10
        active: false
        readonly property var loaded: item
        readonly property bool open: loaded ? !!loaded.open : false
        readonly property bool addingShelf: loaded ? !!loaded.addingShelf : false
        function show(bookId, workKey) {
            active = true
            if (loaded)
                loaded.show(bookId, workKey)
        }
        function close() {
            if (loaded)
                loaded.close()
        }
        sourceComponent: BookDetails {
            anchors.fill: parent
            onRead: (bookId) => { details.close(); view.openBook(bookId) }
            onListen: (bookId) => { details.close(); view.listenBook(bookId) }
            onClosed: view.focusGrid()
        }
    }

    // --- Watched-Folder Manager Sheet ------------------------------------
    Loader {
        id: foldersSheet
        anchors.fill: parent
        z: 15
        active: false
        readonly property var loaded: item
        readonly property bool open: loaded ? !!loaded.open : false
        function show() {
            active = true
            if (loaded)
                loaded.show()
        }
        function close() {
            if (loaded)
                loaded.close()
        }
        sourceComponent: FocusScope {
            id: foldersInner
            property bool open: false
            anchors.fill: parent
            visible: open

            function show() {
                open = true
                forceActiveFocus()
            }
            function close() {
                if (!open)
                    return
                open = false
                view.focusGrid()
            }

            Keys.onEscapePressed: close()

        Rectangle {
            anchors.fill: parent
            color: theme.scrim
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                onPressed: foldersSheet.close()
                onWheel: (wheel) => wheel.accepted = true
            }
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 540)
            height: Math.min(parent.height - 64, foldersCol.implicitHeight + 48)
            radius: Ui.radius(14, theme.radius)
            color: theme.raised
            border.color: Ui.tint(theme.ink1, 0.10)

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                onWheel: (wheel) => wheel.accepted = true
            }

            ColumnLayout {
                id: foldersCol
                anchors.fill: parent
                anchors.margins: 24
                spacing: 14

                RowLayout {
                    Layout.fillWidth: true
                    Glyph { Layout.preferredWidth: 20; Layout.preferredHeight: 20; path: Icons.folder; color: theme.accentInk }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Watched folders")
                        color: theme.ink1
                        font.family: "Literata"
                        font.pixelSize: 20
                        font.weight: Font.Medium
                    }
                    IconButton {
                        glyph: Icons.close
                        tip: qsTr("Close (Esc)")
                        onClicked: foldersSheet.close()
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Clothbound scans these folders for EPUB and PDF books automatically. Book files are never modified.")
                    color: theme.ink3
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }

                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    Layout.bottomMargin: 8
                    visible: library.folders.length === 0
                    text: qsTr("No watched folders yet. Add a folder to keep your library in sync automatically.")
                    color: theme.ink3
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }

                ListView {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.min(260, contentHeight)
                    visible: library.folders.length > 0
                    clip: true
                    spacing: 6
                    boundsBehavior: Flickable.StopAtBounds
                    pixelAligned: true
                    model: library.folders
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                    SmoothWheel { stepSize: 105 }

                    delegate: Rectangle {
                        id: folderRow
                        required property string modelData
                        width: ListView.view.width
                        height: 54
                        radius: Ui.radius(8, theme.radius)
                        color: Ui.tint(theme.ink1, 0.04)
                        border.color: Ui.tint(theme.ink1, 0.08)

                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 8
                            spacing: 10

                            Glyph { Layout.preferredWidth: 18; Layout.preferredHeight: 18; path: Icons.folder; color: theme.ink2 }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Label {
                                    Layout.fillWidth: true
                                    text: view.folderBasename(folderRow.modelData)
                                    textFormat: Text.PlainText
                                    color: theme.ink1
                                    font.pixelSize: 13
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: folderRow.modelData
                                    textFormat: Text.PlainText
                                    color: theme.ink3
                                    font.pixelSize: 11
                                    elide: Text.ElideMiddle
                                }
                            }

                            IconButton {
                                glyph: Icons.trash
                                tone: theme.ink2
                                tip: qsTr("Stop watching folder (files stay on disk)")
                                onClicked: {
                                    const p = folderRow.modelData
                                    library.removeFolder(p)
                                    toast.show(qsTr("Stopped watching “%1”").arg(view.folderBasename(p)))
                                }
                            }
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 6
                    spacing: 8

                    TextButton {
                        primary: true
                        glyph: Icons.plus
                        text: qsTr("Add folder…")
                        onClicked: picker.chooseFolders()
                    }
                    TextButton {
                        glyph: Icons.book
                        text: qsTr("Add books…")
                        onClicked: picker.chooseBooks()
                    }
                    Item { Layout.fillWidth: true }
                    TextButton {
                        glyph: view.refreshIcon
                        enabled: !library.scanning
                        text: library.scanning ? qsTr("Scanning…") : qsTr("Rescan")
                        onClicked: library.rescan()
                    }
                }
            }
        }
        }
    }

    // --- Reading Stats & Insights Sheet ----------------------------------
    Loader {
        id: statsSheet
        anchors.fill: parent
        z: 16
        active: false
        readonly property var loaded: item
        readonly property bool open: loaded ? !!loaded.open : false
        function show() {
            active = true
            if (loaded)
                loaded.show()
        }
        function close() {
            if (loaded)
                loaded.close()
        }
        sourceComponent: FocusScope {
            id: statsInner
            property bool open: false
            readonly property var stats: open && view.statsRevision >= 0 ? library.readingStats : ({})
            readonly property real maxDaySeconds: {
                const days = stats.recentDays || []
                let m = 60
                for (let i = 0; i < days.length; ++i)
                    if ((days[i].seconds || 0) > m)
                        m = days[i].seconds
                return m
            }

            anchors.fill: parent
            visible: open

            function show() {
                open = true
                forceActiveFocus()
            }
            function close() {
                if (!open)
                    return
                open = false
                view.focusGrid()
            }

            Keys.onEscapePressed: close()

            Rectangle {
                anchors.fill: parent
                color: theme.scrim
                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.AllButtons
                    onPressed: statsInner.close()
                    onWheel: (wheel) => wheel.accepted = true
                }
            }

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width - 48, 580)
                height: Math.min(parent.height - 64, statsCol.implicitHeight + 48)
                radius: Ui.radius(14, theme.radius)
                color: theme.raised
                border.color: Ui.tint(theme.ink1, 0.10)

                MouseArea {
                    anchors.fill: parent
                    acceptedButtons: Qt.AllButtons
                    onWheel: (wheel) => wheel.accepted = true
                }

                ColumnLayout {
                    id: statsCol
                    anchors.fill: parent
                    anchors.margins: 24
                    spacing: 16

                    RowLayout {
                        Layout.fillWidth: true
                        Glyph { Layout.preferredWidth: 20; Layout.preferredHeight: 20; path: view.statsIcon; color: theme.accentInk }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Reading stats")
                            color: theme.ink1
                            font.family: "Literata"
                            font.pixelSize: 20
                            font.weight: Font.Medium
                        }
                        IconButton {
                            glyph: Icons.close
                            tip: qsTr("Close (Esc)")
                            onClicked: statsInner.close()
                        }
                    }

                    // 4 KPI Cards
                    GridLayout {
                        Layout.fillWidth: true
                        columns: 2
                        columnSpacing: 10
                        rowSpacing: 10

                        component StatTile: Rectangle {
                            id: tile
                            property string label
                            property string value
                            property string detail
                            Layout.fillWidth: true
                            Layout.preferredHeight: 82
                            radius: Ui.radius(10, theme.radius)
                            color: Ui.tint(theme.ink1, 0.04)
                            border.color: Ui.tint(theme.ink1, 0.07)

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 14
                                spacing: 2
                                Label {
                                    text: tile.label
                                    color: theme.ink3
                                    font.pixelSize: 12
                                }
                                Label {
                                    text: tile.value
                                    color: theme.ink1
                                    font.family: "Literata"
                                    font.pixelSize: 21
                                    font.weight: Font.Medium
                                    font.features: ({ "tnum": 1 })
                                }
                                Label {
                                    visible: text !== ""
                                    text: tile.detail
                                    color: theme.ink3
                                    font.pixelSize: 11
                                    font.features: ({ "tnum": 1 })
                                }
                            }
                        }

                        StatTile {
                            label: qsTr("Total time read")
                            value: view.formatDurationSeconds(statsInner.stats.totalSeconds || 0)
                            detail: qsTr("Today: %1 · Week: %2")
                                .arg(view.formatDurationSeconds(statsInner.stats.todaySeconds || 0))
                                .arg(view.formatDurationSeconds(statsInner.stats.weekSeconds || 0))
                        }
                        StatTile {
                            label: qsTr("Reading streak")
                            value: qsTr("%n days", "", statsInner.stats.streakDays || 0)
                            detail: (statsInner.stats.longestStreak || 0) > 0
                                ? qsTr("Longest: %1 · Active: %2")
                                    .arg(qsTr("%n days", "", statsInner.stats.longestStreak || 0))
                                    .arg(qsTr("%n days", "", statsInner.stats.activeDays || 0))
                                : qsTr("Start reading today")
                        }
                        StatTile {
                            label: qsTr("Books finished")
                            value: {
                                const _lang = translator.language
                                return (statsInner.stats.finishedBooks || 0).toLocaleString(Qt.locale(), "f", 0)
                            }
                            detail: qsTr("%1 reading · %2 unread")
                                .arg(statsInner.stats.readingBooks || 0)
                                .arg(statsInner.stats.unreadBooks || 0)
                        }
                        StatTile {
                            label: qsTr("Reading pace")
                            value: {
                                const _lang = translator.language
                                return qsTr("%1 wpm").arg((statsInner.stats.wordsPerMinute || 260).toLocaleString(Qt.locale(), "f", 0))
                            }
                            detail: qsTr("Learned from your reading")
                        }
                    }

                    // 14-day activity chart
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            text: qsTr("Last 14 days")
                            color: theme.ink3
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.22
                        }

                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 116
                            radius: Ui.radius(10, theme.radius)
                            color: Ui.tint(theme.ink1, 0.03)
                            border.color: Ui.tint(theme.ink1, 0.07)

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                anchors.topMargin: 14
                                anchors.bottomMargin: 10
                                spacing: 6

                                Repeater {
                                    model: statsInner.stats.recentDays || []
                                    delegate: Item {
                                        id: dayBar
                                        required property var modelData
                                        required property int index
                                        Layout.fillWidth: true
                                        Layout.fillHeight: true

                                        readonly property real secs: modelData.seconds || 0
                                        readonly property real ratio: secs > 0 ? Math.max(0.08, secs / statsInner.maxDaySeconds) : 0

                                        Rectangle {
                                            anchors.bottom: dayLbl.top
                                            anchors.bottomMargin: 6
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            width: Math.min(20, parent.width - 2)
                                            height: dayBar.secs > 0 ? Math.max(5, (parent.height - 24) * dayBar.ratio) : 3
                                            radius: Ui.radius(3, theme.radius)
                                            color: dayBar.secs > 0 ? theme.accent : Ui.tint(theme.ink1, 0.12)
                                            opacity: barHover.hovered ? 1.0 : (dayBar.secs > 0 ? 0.85 : 0.5)
                                        }

                                        Label {
                                            id: dayLbl
                                            anchors.bottom: parent.bottom
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            text: (dayBar.modelData.shortLabel || "").slice(0, 2)
                                            color: dayBar.index === 13 ? theme.ink1 : theme.ink3
                                            font.pixelSize: 10
                                            font.weight: dayBar.index === 13 ? Font.DemiBold : Font.Normal
                                        }

                                        HoverHandler { id: barHover }
                                        ToolTip.visible: barHover.hovered
                                        ToolTip.text: dayBar.modelData.day + " · " + view.formatDurationSeconds(dayBar.secs)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // --- Create / Rename Collection Dialog -------------------------------
    Loader {
        id: collectionDialog
        anchors.fill: parent
        z: 18
        active: false
        readonly property var loaded: item
        readonly property bool open: loaded ? !!loaded.open : false
        function showCreate() {
            active = true
            if (loaded)
                loaded.showCreate()
        }
        function showRename(id, currentName) {
            active = true
            if (loaded)
                loaded.showRename(id, currentName)
        }
        function close() {
            if (loaded)
                loaded.close()
        }
        sourceComponent: FocusScope {
            id: collectionInner
            property bool open: false
            property int editingId: 0

            anchors.fill: parent
            visible: open

        function showCreate() {
            editingId = 0
            colNameInput.text = ""
            open = true
            colNameInput.forceActiveFocus()
        }
        function showRename(id, currentName) {
            editingId = id
            colNameInput.text = currentName
            open = true
            colNameInput.forceActiveFocus()
            colNameInput.selectAll()
        }
        function close() {
            if (!open)
                return
            open = false
            view.focusGrid()
        }
        function submit() {
            const name = colNameInput.text.trim()
            if (name === "")
                return
            if (editingId > 0) {
                if (library.renameCollection(editingId, name))
                    toast.show(qsTr("Renamed collection to “%1”").arg(name))
            } else {
                const id = library.createCollection(name)
                if (id > 0) {
                    if (search.text !== "")
                        search.text = ""
                    library.works.searchText = ""
                    library.works.clearFilters()
                    library.works.collectionId = id
                    grid.positionViewAtBeginning()
                    listView.positionViewAtBeginning()
                    toast.show(qsTr("Created collection “%1”").arg(name))
                }
            }
            close()
        }

        Rectangle {
            anchors.fill: parent
            color: theme.scrim
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                onPressed: collectionInner.close()
                onWheel: (wheel) => wheel.accepted = true
            }
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 380)
            height: dialogCol.implicitHeight + 40
            radius: Ui.radius(14, theme.radius)
            color: theme.raised
            border.color: Ui.tint(theme.ink1, 0.10)

            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                onWheel: (wheel) => wheel.accepted = true
            }

            ColumnLayout {
                id: dialogCol
                anchors.fill: parent
                anchors.margins: 20
                spacing: 12

                Label {
                    text: collectionInner.editingId > 0 ? qsTr("Rename collection") : qsTr("New collection")
                    color: theme.ink1
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    radius: Ui.radius(7, theme.radius)
                    color: Ui.tint(theme.ink1, 0.06)
                    border.width: 1.5
                    border.color: theme.accent

                    TextInput {
                        id: colNameInput
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        verticalAlignment: TextInput.AlignVCenter
                        color: theme.ink1
                        font.pixelSize: 13
                        selectByMouse: true
                        Keys.onReturnPressed: collectionInner.submit()
                        Keys.onEnterPressed: collectionInner.submit()
                        Keys.onEscapePressed: collectionInner.close()
                        Text {
                            anchors.fill: parent
                            verticalAlignment: Text.AlignVCenter
                            visible: colNameInput.text === ""
                            text: qsTr("e.g. Favorites, Science Fiction…")
                            color: theme.ink4
                            font: colNameInput.font
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    TextButton {
                        ghost: true
                        text: qsTr("Cancel")
                        onClicked: collectionInner.close()
                    }
                    TextButton {
                        primary: true
                        enabled: colNameInput.text.trim() !== ""
                        text: collectionInner.editingId > 0 ? qsTr("Save") : qsTr("Create")
                        onClicked: collectionInner.submit()
                    }
                }
            }
        }
        }
    }

    // Floating drag proxy when dragging a cover/row onto a Collection in the sidebar.
    Rectangle {
        id: bookDragProxy
        property bool dragging: false
        property int dragBookId: 0
        property string dragBookTitle: ""
        property string dragBookCover: ""

        visible: dragging
        z: 30
        width: Math.min(220, dragRow.implicitWidth + 20)
        height: 44
        radius: Ui.radius(10, theme.radius)
        color: theme.overlay
        border.width: 1.5
        border.color: theme.accent
        opacity: 0.94

        Drag.active: dragging
        Drag.keys: ["clothbound/book"]
        Drag.hotSpot.x: width / 2
        Drag.hotSpot.y: height / 2

        Row {
            id: dragRow
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: 8
            spacing: 8
            CoverImage {
                anchors.verticalCenter: parent.verticalCenter
                width: 22
                height: 32
                source: bookDragProxy.dragBookCover
                title: bookDragProxy.dragBookTitle
                author: ""
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(168, implicitWidth)
                text: bookDragProxy.dragBookTitle
                textFormat: Text.PlainText
                color: theme.ink1
                font.pixelSize: 12
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
        }
    }

    Loader {
        id: addUrlSheet
        anchors.fill: parent
        z: 45
        active: false
        readonly property var loaded: item
        readonly property bool open: loaded ? !!loaded.open : false
        function show(initialUrl) {
            active = true
            if (loaded)
                loaded.show(initialUrl || "")
        }
        function close() {
            if (loaded)
                loaded.close()
        }
        sourceComponent: AddUrlSheet {
            anchors.fill: parent
            onImported: (bookId, title, kind) => {
                const label = title !== "" ? title : qsTr("book")
                toast.show(kind === "article" ? qsTr("Saved article “%1”").arg(label)
                                              : qsTr("Added “%1”").arg(label))
                if (bookId > 0 && library.works.rowOfBook(bookId) >= 0) {
                    view.select(bookId)
                    view.pulsedBookId = bookId
                    pulseTimer.restart()
                }
            }
            onOpenBookRequested: (bookId) => view.openBook(bookId)
            onClosed: if (view.enabled) view.focusGrid()
        }
    }

    Loader {
        id: catalogsSheet
        anchors.fill: parent
        z: 46
        active: false
        readonly property var loaded: item
        readonly property bool open: loaded ? !!loaded.open : false
        function show() {
            active = true
            if (loaded)
                loaded.show()
        }
        function close() {
            if (loaded)
                loaded.close()
        }
        sourceComponent: CatalogsSheet {
            anchors.fill: parent
            targetCollectionId: library.works.collectionId
            onOpenBook: (bookId) => {
                catalogsSheet.close()
                view.openBook(bookId)
            }
            onClosed: if (view.enabled) view.focusGrid()
        }
    }
}
