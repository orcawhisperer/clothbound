import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Format.js" as Format
import "Icons.js" as Icons
import "Ui.js" as Ui

// The library: books under way on top, filter chips & shelves, grid or compact
// list view, watched-folder manager, and reading stats & insights.
FocusScope {
    id: view

    property string viewMode: "grid"
    property int statsRevision: 0

    readonly property bool typing: search.activeFocus || collectionDialog.open || details.addingShelf
    readonly property bool detailsOpen: details.open
    readonly property bool foldersOpen: foldersSheet.open
    readonly property bool statsOpen: statsSheet.open
    readonly property bool collectionDialogOpen: collectionDialog.open
    readonly property bool menuOpen: sortMenu.opened || seriesMenu.opened || filterMenu.opened || moreMenu.opened
    property bool sidebarCollapsed: false
    function toggleSidebar() { sidebarCollapsed = !sidebarCollapsed }
    // The toolbar names what the grid shows: a shelf, a collection, or a search.
    readonly property string sectionTitle: {
        const works = library.works
        if (works.searchText !== "")
            return qsTr("Search")
        if (works.collectionId !== 0) {
            const col = library.collections.find(c => c.id === works.collectionId)
            return col ? col.name : qsTr("Collection")
        }
        return works.statusFilter === "reading" ? qsTr("Reading now")
             : works.statusFilter === "want" ? qsTr("Want to read")
             : works.statusFilter === "finished" ? qsTr("Finished")
             : works.statusFilter === "listening" ? qsTr("Listening")
             : works.statusFilter === "unread" ? qsTr("Not started")
             : qsTr("All books")
    }
    readonly property int currentRow: viewMode === "list" ? listView.currentIndex : grid.currentIndex

    readonly property string listIcon: "M8 6.5 H20 M8 12 H20 M8 17.5 H20 M4 6.5 H5 M4 12 H5 M4 17.5 H5"
    readonly property string gridIcon: "M4.5 4.5 H10.5 V10.5 H4.5 Z M13.5 4.5 H19.5 V10.5 H13.5 Z M4.5 13.5 H10.5 V19.5 H4.5 Z M13.5 13.5 H19.5 V19.5 H13.5 Z"
    readonly property string statsIcon: "M5 19.5 V13 M10 19.5 V8.5 M15 19.5 V11 M20 19.5 V5.5"
    readonly property string refreshIcon: "M19 12 A7 7 0 1 1 16.95 7.05 M19 4.5 V8.5 H15"

    signal openBook(int bookId)

    Connections {
        target: library
        function onBooksChanged() { view.statsRevision++ }
        function onReadingStatsChanged() { view.statsRevision++ }
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
        foldersSheet.show()
    }
    function closeFolders() { foldersSheet.close() }
    function openStats() {
        foldersSheet.close()
        statsRevision++
        statsSheet.show()
    }
    function closeStats() { statsSheet.close() }
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
        onIntelligenceRequested: win.showAiSettings()
    }

    // Everything right of the sidebar, for centring the empty states.
    Item {
        id: contentArea
        anchors.top: bar.bottom
        anchors.bottom: parent.bottom
        anchors.left: sidebar.right
        anchors.right: parent.right
    }

    // --- Toolbar ---------------------------------------------------------
    Item {
        id: bar
        anchors.top: parent.top
        anchors.left: sidebar.right
        anchors.right: parent.right
        height: 56

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 28
            anchors.rightMargin: 16
            spacing: 6

            Label {
                text: view.sectionTitle
                color: theme.ink1
                font.pixelSize: 20
                font.weight: Font.DemiBold
                elide: Text.ElideRight
                Layout.maximumWidth: Math.max(120, bar.width * 0.3)
            }
            Label {
                Layout.leftMargin: 4
                Layout.alignment: Qt.AlignBaseline
                visible: library.bookCount > 0 && !library.scanning
                text: library.works.grouped && !library.works.hasActiveFilters && library.works.searchText === ""
                      ? qsTr("%n works", "", library.works.count) : qsTr("%n books", "", library.works.count)
                color: theme.ink3
                font.pixelSize: 13
            }
            // While books arrive, their count takes the place of the total.
            Row {
                visible: library.scanning
                Layout.leftMargin: 6
                spacing: 8
                BusyIndicator {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 16
                    height: 16
                    padding: 0
                    running: parent.visible
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: library.scanTotal > 0
                          ? qsTr("Adding books · %1 of %2").arg(library.scanDone.toLocaleString(Qt.locale(), 'f', 0))
                                                            .arg(library.scanTotal.toLocaleString(Qt.locale(), 'f', 0))
                          : qsTr("Looking for books…")
                    color: theme.ink3
                    font.pixelSize: 13
                    font.features: { "tnum": 1 }
                }
            }
            Item { Layout.fillWidth: true }

            SearchBox {
                id: search
                Layout.preferredWidth: Math.min(280, view.width * 0.24)
                visible: library.bookCount > 0
                placeholder: qsTr("Search title, author…")
                onTextChanged: library.works.searchText = text
                onAccepted: {
                    const activeView = view.viewMode === "list" ? listView : grid
                    if (activeView.count > 0)
                        activeView.currentIndex = 0
                    view.focusGrid()
                }
                onEscaped: view.focusGrid()
            }
            Segmented {
                visible: library.bookCount > 0
                Layout.leftMargin: 4
                compact: true
                model: [{ value: "grid", label: qsTr("Covers (V)"), glyph: Icons.grid },
                        { value: "list", label: qsTr("List (V)"), glyph: Icons.list }]
                current: view.viewMode
                onActivated: view.toggleViewMode()
            }
            AbstractButton {
                id: sortButton
                visible: library.bookCount > 0
                Layout.preferredHeight: 30
                implicitWidth: sortRow.implicitWidth + 20
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                background: Rectangle {
                    radius: Math.min(7, theme.radius)
                    color: sortButton.pressed || sortMenu.opened ? theme.fillPressed : sortButton.hovered ? theme.fill : "transparent"
                }
                contentItem: Item {
                    Row {
                        id: sortRow
                        anchors.centerIn: parent
                        spacing: 5
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: view.sortLabel(library.works.sortOrder)
                            color: theme.ink2
                            font.pixelSize: 13
                        }
                        Glyph { anchors.verticalCenter: parent.verticalCenter; width: 12; height: 12; stroke: 2; path: Icons.chevronDown; color: theme.ink3 }
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
            IconButton {
                id: filterButton
                visible: library.bookCount > 0
                glyph: Icons.filter
                active: library.works.formatFilter !== "" || library.works.seriesFilter !== ""
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
                        MenuRow { text: qsTr("Add books…"); onClicked: { moreMenu.close(); picker.chooseBooks() } }
                    }
                }
            }
            IconButton {
                glyph: Icons.plus
                tip: qsTr("Add a folder of books (A)")
                onClicked: picker.chooseFolders()
            }
        }

        // Reading new books in: a thin line along the bar.
        Rectangle {
            visible: library.scanning && library.scanTotal > 0
            anchors.bottom: parent.bottom
            height: 2
            width: parent.width * (library.scanTotal > 0 ? library.scanDone / library.scanTotal : 0)
            color: theme.accent
            Behavior on width { NumberAnimation { duration: 200 } }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            readonly property real scrollY: view.viewMode === "list"
                ? (listView.contentY - listView.originY) : (grid.contentY - grid.originY)
            color: theme.hairline
            opacity: scrollY > 4 ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 120 } }
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
                color: Ui.tint(theme.ink1, 0.08)
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

    // --- Cover grid view -------------------------------------------------
    GridView {
        id: grid
        anchors.top: bar.bottom
        anchors.left: sidebar.right
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 18
        anchors.rightMargin: 18
        visible: library.bookCount > 0 && view.viewMode === "grid"
        clip: true
        focus: view.viewMode === "grid"
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
            readonly property bool showShelf: library.continueReading.length > 0 && library.works.searchText === "" && !library.works.hasActiveFilters

            ContinueShelf {
                x: 14
                width: grid.width - 28
                visible: parent.showShelf
                height: visible ? implicitHeight : 0
                books: library.continueReading
                onOpen: (bookId) => view.openBook(bookId)
                onLeaveDown: view.focusGrid()
            }
            Item { width: 1; height: parent.showShelf ? 28 : 4 }
            // Under the Continue card, the rest of the shelf gets a heading.
            Label {
                x: 14
                visible: parent.showShelf
                text: view.sectionTitle
                color: theme.ink1
                font.pixelSize: 15
                font.weight: Font.DemiBold
                bottomPadding: 6
            }
        }

        delegate: BookCard {
            onActivated: view.activate(index)
            onDetailsRequested: view.showDetails(index)
        }
        // More books still arriving: a row of placeholders after the last.
        footer: Flow {
            width: grid.width
            visible: library.scanning && library.scanTotal > library.scanDone
            height: visible ? implicitHeight : 0
            Repeater {
                model: parent.visible ? Math.min(grid.columns, library.scanTotal - library.scanDone) : 0
                delegate: SkeletonCard { width: grid.cellWidth }
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
        spacing: 2
        currentIndex: -1
        model: library.works

        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        header: Column {
            width: listView.width
            topPadding: 12
            bottomPadding: 8
            spacing: 0
            readonly property bool showShelf: library.continueReading.length > 0 && library.works.searchText === "" && !library.works.hasActiveFilters

            ContinueShelf {
                x: 8
                width: listView.width - 16
                visible: parent.showShelf
                height: visible ? implicitHeight : 0
                books: library.continueReading
                onOpen: (bookId) => view.openBook(bookId)
                onLeaveDown: view.focusGrid()
            }
            Item { width: 1; height: parent.showShelf ? 24 : 0 }
            Label {
                x: 8
                text: (library.works.searchText !== "" || library.works.hasActiveFilters)
                      ? qsTr("%n matches", "", library.works.count)
                      : library.works.grouped ? qsTr("All books · %n works", "", library.works.count) : qsTr("All books")
                color: theme.ink1
                font.pixelSize: 15
                font.weight: Font.DemiBold
                bottomPadding: 4
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

            readonly property bool selected: ListView.isCurrentItem && listView.activeFocus

            width: listView.width
            height: 68

            Rectangle {
                anchors.fill: parent
                radius: Math.min(8, theme.radius)
                color: listRow.selected ? Ui.tint(theme.accent, theme.light ? 0.14 : 0.2)
                     : rowHover.hovered ? Ui.tint(theme.ink1, 0.05) : "transparent"
                border.width: listRow.selected ? 1 : 0
                border.color: theme.accent
            }

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 10
                anchors.rightMargin: 14
                spacing: 14

                CoverImage {
                    Layout.preferredWidth: 36
                    Layout.preferredHeight: 52
                    source: listRow.cover
                    title: listRow.title
                    author: listRow.author
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 2

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            text: listRow.title
                            textFormat: Text.PlainText
                            color: listRow.problem !== "" ? theme.ink3 : theme.ink1
                            font.pixelSize: 14
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Label {
                            visible: listRow.series !== ""
                            text: listRow.seriesIndex > 0
                                ? (listRow.series + " #" + (Math.floor(listRow.seriesIndex) === listRow.seriesIndex ? Math.floor(listRow.seriesIndex) : listRow.seriesIndex))
                                : listRow.series
                            textFormat: Text.PlainText
                            color: theme.accentInk
                            font.pixelSize: 11
                            leftPadding: 7
                            rightPadding: 7
                            topPadding: 2
                            bottomPadding: 2
                            background: Rectangle {
                                radius: 10
                                color: Ui.tint(theme.accent, 0.14)
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
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
                        color: listRow.problem !== "" ? (theme.colors.red || "#e06c75") : theme.ink2
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }

                // Format badge
                Label {
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
                        radius: 4
                        color: Ui.tint(theme.ink1, 0.06)
                    }
                }

                // Progress / status column
                ColumnLayout {
                    Layout.preferredWidth: 112
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
                        }
                    }

                    Rectangle {
                        visible: listRow.progress > 0.005 && !listRow.finished
                        Layout.fillWidth: true
                        Layout.preferredHeight: 4
                        radius: 2
                        color: Ui.tint(theme.ink1, 0.12)
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
                        color: theme.ink3
                        font.pixelSize: 11
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
        }

        Keys.onReturnPressed: view.activate(currentIndex)
        Keys.onEnterPressed: view.activate(currentIndex)
        onActiveFocusChanged: if (activeFocus && currentIndex < 0 && count > 0) currentIndex = 0
    }

    // Empty filter / search state
    Column {
        anchors.centerIn: contentArea
        visible: library.bookCount > 0 && library.works.count === 0
        spacing: 12

        Label {
            anchors.horizontalCenter: parent.horizontalCenter
            text: library.works.searchText !== ""
                ? qsTr("No books match “%1”").arg(library.works.searchText)
                : qsTr("No books match these filters")
            color: theme.ink2
            font.pixelSize: 15
        }
        TextButton {
            anchors.horizontalCenter: parent.horizontalCenter
            visible: library.works.hasActiveFilters
            text: qsTr("Clear filters")
            onClicked: library.works.clearFilters()
        }
    }

    EmptyLibrary {
        anchors.centerIn: contentArea
        visible: library.bookCount === 0 && !library.scanning
        onAddFolders: picker.chooseFolders()
        onAddBooks: picker.chooseBooks()
    }

    // The first books on their way: placeholders where their covers will be.
    Flow {
        id: firstArrivals
        anchors.fill: contentArea
        anchors.margins: 18
        anchors.topMargin: 16
        visible: library.bookCount === 0 && library.scanning
        clip: true
        readonly property int columns: Math.max(2, Math.floor(width / 190))
        readonly property real cell: Math.floor(width / columns)
        Repeater {
            model: firstArrivals.visible ? Math.min(18, Math.max(firstArrivals.columns, library.scanTotal)) : 0
            delegate: SkeletonCard { width: firstArrivals.cell }
        }
    }

    component SkeletonCard: Item {
        height: Math.round((width - 28) * 1.5) + 14 + 64
        Skeleton {
            x: 14
            y: 14
            width: parent.width - 28
            height: (parent.width - 28) * 1.5
            radius: Math.min(4, theme.radius)
        }
        Skeleton {
            x: 14
            y: (parent.width - 28) * 1.5 + 26
            width: (parent.width - 28) * 0.8
            height: 12
            shimmer: false
        }
        Skeleton {
            x: 14
            y: (parent.width - 28) * 1.5 + 46
            width: (parent.width - 28) * 0.5
            height: 10
            shimmer: false
        }
    }

    BookDetails {
        id: details
        anchors.fill: parent
        z: 10
        onRead: (bookId) => { details.close(); view.openBook(bookId) }
        onClosed: view.focusGrid()
    }

    // --- Watched-Folder Manager Sheet ------------------------------------
    FocusScope {
        id: foldersSheet
        property bool open: false
        anchors.fill: parent
        z: 15
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
            color: Ui.tint("black", theme.light ? 0.22 : 0.5)
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
            radius: Math.min(12, theme.radius)
            color: theme.ground
            border.color: Ui.tint(theme.ink1, 0.12)

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
                        font.weight: Font.DemiBold
                    }
                    IconButton {
                        glyph: Icons.close
                        tip: qsTr("Close (Esc)")
                        onClicked: foldersSheet.close()
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Dogear scans these folders for EPUB and PDF books automatically. Book files are never modified.")
                    color: theme.ink2
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
                    model: library.folders
                    ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                    delegate: Rectangle {
                        id: folderRow
                        required property string modelData
                        width: ListView.view.width
                        height: 54
                        radius: Math.min(8, theme.radius)
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
                                    font.weight: Font.DemiBold
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

    // --- Reading Stats & Insights Sheet ----------------------------------
    FocusScope {
        id: statsSheet
        property bool open: false
        readonly property var stats: view.statsRevision >= 0 ? library.readingStats : ({})
        readonly property real maxDaySeconds: {
            const days = stats.recentDays || []
            let m = 60
            for (let i = 0; i < days.length; ++i)
                if ((days[i].seconds || 0) > m)
                    m = days[i].seconds
            return m
        }

        anchors.fill: parent
        z: 16
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
            color: Ui.tint("black", theme.light ? 0.22 : 0.5)
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                onPressed: statsSheet.close()
                onWheel: (wheel) => wheel.accepted = true
            }
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 580)
            height: Math.min(parent.height - 64, statsCol.implicitHeight + 48)
            radius: Math.min(12, theme.radius)
            color: theme.ground
            border.color: Ui.tint(theme.ink1, 0.12)

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
                        font.weight: Font.DemiBold
                    }
                    IconButton {
                        glyph: Icons.close
                        tip: qsTr("Close (Esc)")
                        onClicked: statsSheet.close()
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
                        radius: Math.min(10, theme.radius)
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
                                font.weight: Font.DemiBold
                            }
                            Label {
                                visible: text !== ""
                                text: tile.detail
                                color: theme.ink2
                                font.pixelSize: 11
                            }
                        }
                    }

                    StatTile {
                        label: qsTr("Total time read")
                        value: view.formatDurationSeconds(statsSheet.stats.totalSeconds || 0)
                        detail: qsTr("Today: %1 · Week: %2")
                            .arg(view.formatDurationSeconds(statsSheet.stats.todaySeconds || 0))
                            .arg(view.formatDurationSeconds(statsSheet.stats.weekSeconds || 0))
                    }
                    StatTile {
                        label: qsTr("Reading streak")
                        value: qsTr("%n days", "", statsSheet.stats.streakDays || 0)
                        detail: (statsSheet.stats.longestStreak || 0) > 0
                            ? qsTr("Longest: %1 · Active: %2")
                                .arg(qsTr("%n days", "", statsSheet.stats.longestStreak || 0))
                                .arg(qsTr("%n days", "", statsSheet.stats.activeDays || 0))
                            : qsTr("Start reading today")
                    }
                    StatTile {
                        label: qsTr("Books finished")
                        value: {
                            const _lang = translator.language
                            return (statsSheet.stats.finishedBooks || 0).toLocaleString(Qt.locale(), "f", 0)
                        }
                        detail: qsTr("%1 reading · %2 unread")
                            .arg(statsSheet.stats.readingBooks || 0)
                            .arg(statsSheet.stats.unreadBooks || 0)
                    }
                    StatTile {
                        label: qsTr("Reading pace")
                        value: {
                            const _lang = translator.language
                            return qsTr("%1 wpm").arg((statsSheet.stats.wordsPerMinute || 260).toLocaleString(Qt.locale(), "f", 0))
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
                        color: theme.ink2
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 116
                        radius: Math.min(10, theme.radius)
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
                                model: statsSheet.stats.recentDays || []
                                delegate: Item {
                                    id: dayBar
                                    required property var modelData
                                    required property int index
                                    Layout.fillWidth: true
                                    Layout.fillHeight: true

                                    readonly property real secs: modelData.seconds || 0
                                    readonly property real ratio: secs > 0 ? Math.max(0.08, secs / statsSheet.maxDaySeconds) : 0

                                    Rectangle {
                                        anchors.bottom: dayLbl.top
                                        anchors.bottomMargin: 6
                                        anchors.horizontalCenter: parent.horizontalCenter
                                        width: Math.min(20, parent.width - 2)
                                        height: dayBar.secs > 0 ? Math.max(5, (parent.height - 24) * dayBar.ratio) : 3
                                        radius: 3
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

    // --- Create / Rename Shelf Dialog ------------------------------------
    FocusScope {
        id: collectionDialog
        property bool open: false
        property int editingId: 0

        anchors.fill: parent
        z: 18
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
                    library.works.collectionId = id
                    toast.show(qsTr("Created collection “%1”").arg(name))
                }
            }
            close()
        }

        Rectangle {
            anchors.fill: parent
            color: Ui.tint("black", theme.light ? 0.22 : 0.5)
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.AllButtons
                onPressed: collectionDialog.close()
                onWheel: (wheel) => wheel.accepted = true
            }
        }

        Rectangle {
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 380)
            height: dialogCol.implicitHeight + 40
            radius: Math.min(12, theme.radius)
            color: theme.ground
            border.color: Ui.tint(theme.ink1, 0.12)

            ColumnLayout {
                id: dialogCol
                anchors.fill: parent
                anchors.margins: 20
                spacing: 12

                Label {
                    text: collectionDialog.editingId > 0 ? qsTr("Rename collection") : qsTr("New collection")
                    color: theme.ink1
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 38
                    radius: Math.min(8, theme.radius)
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
                        font.pixelSize: 14
                        selectByMouse: true
                        Keys.onReturnPressed: collectionDialog.submit()
                        Keys.onEnterPressed: collectionDialog.submit()
                        Keys.onEscapePressed: collectionDialog.close()
                        Text {
                            anchors.fill: parent
                            verticalAlignment: Text.AlignVCenter
                            visible: colNameInput.text === ""
                            text: qsTr("e.g. Favorites, Science Fiction…")
                            color: theme.ink3
                            font: colNameInput.font
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    Item { Layout.fillWidth: true }
                    TextButton {
                        text: qsTr("Cancel")
                        onClicked: collectionDialog.close()
                    }
                    TextButton {
                        primary: true
                        enabled: colNameInput.text.trim() !== ""
                        text: collectionDialog.editingId > 0 ? qsTr("Save") : qsTr("Create")
                        onClicked: collectionDialog.submit()
                    }
                }
            }
        }
    }
}
