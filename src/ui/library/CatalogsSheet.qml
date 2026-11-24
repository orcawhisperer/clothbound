import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui

// Spacious OPDS 1.2 & OPDS 2.0 catalog browser modal sheet:
// Left rail (220 px) with curated DRM-free public catalogs + user servers,
// right pane with breadcrumbs, search, inline HTTP 401 sign-in, navigation
// subsections, and 1-click Get -> progress -> Open publication cards.
FocusScope {
    id: sheet

    property bool open: false
    property int targetCollectionId: 0
    property bool addingServer: false
    property string confirmRemoveId: ""
    property string addServerError: ""
    property bool showAddPassword: false
    property bool showAuthPassword: false
    property string _lastNavKey: ""

    // Local SVG paths (24x24 grid) for glyphs not present in Icons.js.
    readonly property string globeIcon: "M12 3 A9 9 0 1 1 12 21 A9 9 0 1 1 12 3 Z M3.5 12 H20.5 M12 3 C15 6 15 18 12 21 C9 18 9 6 12 3 Z"
    readonly property string serverIcon: "M4.5 5.5 H19.5 Q20.5 5.5 20.5 6.5 V9.5 Q20.5 10.5 19.5 10.5 H4.5 Q3.5 10.5 3.5 9.5 V6.5 Q3.5 5.5 4.5 5.5 Z M4.5 13.5 H19.5 Q20.5 13.5 20.5 14.5 V17.5 Q20.5 18.5 19.5 18.5 H4.5 Q3.5 18.5 3.5 17.5 V14.5 Q3.5 13.5 4.5 13.5 Z M7 8 H7.01 M7 16 H7.01"
    readonly property string lockIcon: "M6.5 11 H17.5 Q19 11 19 12.5 V18.5 Q19 20 17.5 20 H6.5 Q5 20 5 18.5 V12.5 Q5 11 6.5 11 Z M8.5 11 V8 A3.5 3.5 0 0 1 15.5 8 V11"
    readonly property string eyeIcon: "M2.5 12 C5 7.5 8.5 5.5 12 5.5 C15.5 5.5 19 7.5 21.5 12 C19 16.5 15.5 18.5 12 18.5 C8.5 18.5 5 16.5 2.5 12 Z M14.8 12 A2.8 2.8 0 1 1 9.2 12 A2.8 2.8 0 1 1 14.8 12 Z"
    readonly property string eyeOffIcon: "M3 3 L21 21 M10.5 5.7 A9.2 9.2 0 0 1 21.5 12 A11.8 11.8 0 0 1 18.2 16.2 M14.2 14.2 A2.8 2.8 0 0 1 9.8 9.8 M6.5 7.8 A11.5 11.5 0 0 0 2.5 12 C5 16.5 8.5 18.5 12 18.5 A9.3 9.3 0 0 0 15 18"
    readonly property string refreshIcon: "M19 12 A7 7 0 1 1 16.95 7.05 M19 4.5 V8.5 H15"

    readonly property var builtInCatalogs: {
        const list = opds.catalogs || []
        const out = []
        for (let i = 0; i < list.length; ++i)
            if (list[i].builtIn)
                out.push(list[i])
        return out
    }
    readonly property var customCatalogs: {
        const list = opds.catalogs || []
        const out = []
        for (let i = 0; i < list.length; ++i)
            if (!list[i].builtIn)
                out.push(list[i])
        return out
    }

    signal openBook(int bookId)
    signal closed()

    function show() {
        addingServer = false
        confirmRemoveId = ""
        addServerError = ""
        open = true
        forceActiveFocus()
        if (opds.activeCatalogId === "" && opds.catalogs && opds.catalogs.length > 0)
            opds.openCatalog(opds.catalogs[0].id)
    }

    function close() {
        if (!open)
            return
        addingServer = false
        confirmRemoveId = ""
        addServerError = ""
        open = false
        closed()
    }

    function openAddServer() {
        confirmRemoveId = ""
        addServerError = ""
        showAddPassword = false
        serverNameField.text = ""
        serverUrlField.text = ""
        serverUserField.text = ""
        serverPassField.text = ""
        addingServer = true
        serverNameField.forceActiveFocus()
    }

    function submitAddServer() {
        const rawUrl = serverUrlField.text.trim()
        const rawName = serverNameField.text.trim()
        if (rawUrl === "") {
            addServerError = qsTr("Enter your OPDS server URL (starting with http:// or https://).")
            serverUrlField.forceActiveFocus()
            return
        }
        if (!/^https?:\/\/.+/i.test(rawUrl)) {
            addServerError = qsTr("Catalog URL must start with http:// or https://.")
            serverUrlField.forceActiveFocus()
            return
        }
        const id = opds.addCatalog(rawName, rawUrl, serverUserField.text.trim(), serverPassField.text)
        if (!id || id === "") {
            addServerError = opds.error || qsTr("Could not add this OPDS server.")
            return
        }
        addServerError = ""
        serverPassField.text = ""
        addingServer = false
        searchField.text = ""
        opds.openCatalog(id)
    }

    function submitAuth() {
        if (opds.activeCatalogId === "")
            return
        if (opds.updateCatalogCredentials(opds.activeCatalogId, authUserField.text.trim(), authPassField.text)) {
            authPassField.text = ""
            opds.retry()
        }
    }

    function submitSearch() {
        const q = searchField.text.trim()
        if (q !== "")
            opds.search(q)
    }

    Connections {
        target: opds
        function onFeedChanged() {
            if (opds.authRequired) {
                authUserField.text = opds.activeCatalogUsername || ""
                authPassField.text = ""
                showAuthPassword = false
                if (sheet.open && !sheet.addingServer) {
                    if (authUserField.text === "")
                        authUserField.forceActiveFocus()
                    else
                        authPassField.forceActiveFocus()
                }
            }
            const crumbs = opds.breadcrumbs || []
            const topUrl = crumbs.length > 0 ? (crumbs[crumbs.length - 1].url || "") : ""
            const navKey = (opds.activeCatalogId || "") + "|" + crumbs.length + "|" + topUrl
            if (navKey !== sheet._lastNavKey) {
                sheet._lastNavKey = navKey
                if (feedList.count > 0)
                    feedList.positionViewAtBeginning()
            }
        }
    }

    anchors.fill: parent
    property real reveal: open ? 1 : 0
    Behavior on reveal {
        NumberAnimation {
            duration: theme.reducedMotion ? 0 : (sheet.open ? 220 : 160)
            easing.type: sheet.open ? Easing.OutCubic : Easing.InCubic
        }
    }
    visible: reveal > 0.005
    opacity: reveal

    Keys.onShortcutOverride: (event) => {
        if (sheet.open && event.key === Qt.Key_Escape
                && (confirmRemoveId !== "" || addingServer || (searchField.activeFocus && searchField.text !== "")))
            event.accepted = true
    }
    Keys.onEscapePressed: (event) => {
        if (confirmRemoveId !== "") {
            confirmRemoveId = ""
            event.accepted = true
        } else if (addingServer) {
            addingServer = false
            addServerError = ""
            sheet.forceActiveFocus()
            event.accepted = true
        } else if (searchField.activeFocus && searchField.text !== "") {
            searchField.text = ""
            event.accepted = true
        } else {
            close()
            event.accepted = true
        }
    }

    // Dimmed backdrop; clicking outside closes the sheet.
    Rectangle {
        anchors.fill: parent
        color: theme.scrim
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onClicked: sheet.close()
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    // Centered spacious modal card.
    Rectangle {
        id: card
        anchors.centerIn: parent
        anchors.verticalCenterOffset: theme.reducedMotion ? 0 : Math.round((1 - sheet.reveal) * 10)
        width: Math.max(520, Math.min(960, sheet.width - 64))
        height: Math.max(400, Math.min(680, sheet.height - 64))
        radius: Ui.radius(14, theme.radius)
        color: theme.ground
        border.width: 1
        border.color: Ui.tint(theme.ink1, theme.light ? 0.12 : 0.09)
        scale: theme.reducedMotion ? 1.0 : (0.97 + 0.03 * sheet.reveal)

        // Absorb clicks and wheel events so the library behind doesn't move.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: (wheel) => wheel.accepted = true
        }

        // Soft elevation shadow matching Sheet.qml.
        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.topMargin: 16
            anchors.margins: -6
            radius: parent.radius > 0 ? parent.radius + 6 : 0
            color: theme.shadow
            opacity: 0.52
        }

        // Clipped split interior.
        Item {
            anchors.fill: parent
            anchors.margins: 1
            clip: true

            // --- Left catalog rail (220 px) --------------------------------
            Rectangle {
                id: rail
                anchors.left: parent.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                width: 220
                radius: Math.max(0, card.radius - 1)
                color: theme.sidebar

                // Square off the right corners of the rail against the hairline divider.
                Rectangle {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: Math.max(1, parent.radius)
                    color: theme.sidebar
                }
                Rectangle {
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 1
                    color: theme.hairline
                }

                // Rail header.
                Item {
                    id: railHeader
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 56

                    Row {
                        anchors.left: parent.left
                        anchors.leftMargin: 18
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 8
                        Glyph {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 16
                            height: 16
                            stroke: 1.7
                            path: sheet.globeIcon
                            color: theme.accentInk
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Catalogs")
                            color: theme.ink1
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                        }
                    }
                }

                Flickable {
                    id: railFlick
                    anchors.top: railHeader.bottom
                    anchors.bottom: railFooter.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    contentWidth: width
                    contentHeight: railCol.implicitHeight + 16
                    boundsBehavior: Flickable.StopAtBounds
                    clip: true
                    ScrollBar.vertical: ScrollBar {}
                    SmoothWheel { stepSize: 90 }

                    Column {
                        id: railCol
                        width: parent.width
                        spacing: 2

                        // Public catalogs label.
                        Label {
                            x: 18
                            height: 24
                            verticalAlignment: Text.AlignBottom
                            bottomPadding: 4
                            text: qsTr("Public catalogs")
                            color: theme.ink4
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.22
                        }

                        Repeater {
                            model: sheet.builtInCatalogs
                            delegate: AbstractButton {
                                id: builtInRow
                                required property var modelData
                                readonly property bool selected: !sheet.addingServer && opds.activeCatalogId === modelData.id

                                x: 10
                                width: railCol.width - 20
                                height: 48
                                hoverEnabled: true
                                focusPolicy: Qt.TabFocus
                                Accessible.role: Accessible.ListItem
                                Accessible.name: modelData.name
                                Accessible.selected: selected
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                onClicked: {
                                    sheet.addingServer = false
                                    sheet.confirmRemoveId = ""
                                    searchField.text = ""
                                    opds.openCatalog(builtInRow.modelData.id)
                                }

                                background: Rectangle {
                                    radius: Ui.radius(8, theme.radius)
                                    color: builtInRow.selected ? theme.accentSoft
                                         : builtInRow.pressed ? theme.fillPressed
                                         : builtInRow.hovered ? theme.fill : "transparent"
                                    border.width: builtInRow.visualFocus ? 2 : 0
                                    border.color: theme.accent

                                    Rectangle {
                                        visible: builtInRow.selected
                                        anchors.left: parent.left
                                        anchors.leftMargin: 2
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 3
                                        height: 18
                                        radius: 1.5
                                        color: theme.accent
                                    }
                                }

                                contentItem: Item {
                                    Glyph {
                                        id: catGlyph
                                        anchors.left: parent.left
                                        anchors.leftMargin: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 15
                                        height: 15
                                        stroke: 1.7
                                        path: builtInRow.modelData.authOptional ? sheet.lockIcon : sheet.globeIcon
                                        color: builtInRow.selected ? theme.accentInk : theme.ink3
                                    }
                                    Column {
                                        anchors.left: catGlyph.right
                                        anchors.leftMargin: 9
                                        anchors.right: parent.right
                                        anchors.rightMargin: 8
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 1
                                        Label {
                                            width: parent.width
                                            text: builtInRow.modelData.name
                                            textFormat: Text.PlainText
                                            color: builtInRow.selected ? theme.ink1 : theme.ink2
                                            font.pixelSize: 13
                                            font.weight: builtInRow.selected ? Font.Medium : Font.Normal
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            width: parent.width
                                            text: builtInRow.modelData.subtitle || ""
                                            textFormat: Text.PlainText
                                            color: builtInRow.selected ? theme.ink3 : theme.ink4
                                            font.pixelSize: 11
                                            elide: Text.ElideRight
                                        }
                                    }
                                }
                            }
                        }

                        // Your servers label.
                        Label {
                            x: 18
                            height: 36
                            verticalAlignment: Text.AlignBottom
                            bottomPadding: 4
                            text: qsTr("Your servers")
                            color: theme.ink4
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.letterSpacing: 0.22
                        }

                        Label {
                            visible: sheet.customCatalogs.length === 0
                            x: 18
                            width: railCol.width - 36
                            text: qsTr("Calibre-Web, Kavita, Komga, Ubooquity…")
                            color: theme.ink4
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                            lineHeight: 1.35
                        }

                        Repeater {
                            model: sheet.customCatalogs
                            delegate: Column {
                                id: customWrap
                                required property var modelData
                                readonly property bool confirming: sheet.confirmRemoveId === modelData.id
                                readonly property bool selected: !sheet.addingServer && opds.activeCatalogId === modelData.id

                                width: railCol.width
                                spacing: 4

                                AbstractButton {
                                    id: customRow
                                    x: 10
                                    width: railCol.width - 20
                                    height: 46
                                    hoverEnabled: true
                                    focusPolicy: Qt.TabFocus
                                    Accessible.role: Accessible.ListItem
                                    Accessible.name: customWrap.modelData.name
                                    Accessible.selected: customWrap.selected
                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                    onClicked: {
                                        sheet.addingServer = false
                                        sheet.confirmRemoveId = ""
                                        searchField.text = ""
                                        opds.openCatalog(customWrap.modelData.id)
                                    }

                                    background: Rectangle {
                                        radius: Ui.radius(8, theme.radius)
                                        color: customWrap.selected ? theme.accentSoft
                                             : customRow.pressed ? theme.fillPressed
                                             : customRow.hovered ? theme.fill : "transparent"
                                        border.width: customRow.visualFocus ? 2 : 0
                                        border.color: theme.accent

                                        Rectangle {
                                            visible: customWrap.selected
                                            anchors.left: parent.left
                                            anchors.leftMargin: 2
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: 3
                                            height: 18
                                            radius: 1.5
                                            color: theme.accent
                                        }
                                    }

                                    contentItem: Item {
                                        Glyph {
                                            id: srvGlyph
                                            anchors.left: parent.left
                                            anchors.leftMargin: 10
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: 15
                                            height: 15
                                            stroke: 1.7
                                            path: sheet.serverIcon
                                            color: customWrap.selected ? theme.accentInk : theme.ink3
                                        }
                                        Column {
                                            anchors.left: srvGlyph.right
                                            anchors.leftMargin: 9
                                            anchors.right: removeBtn.visible ? removeBtn.left : parent.right
                                            anchors.rightMargin: 6
                                            anchors.verticalCenter: parent.verticalCenter
                                            spacing: 1
                                            Label {
                                                width: parent.width
                                                text: customWrap.modelData.name
                                                textFormat: Text.PlainText
                                                color: customWrap.selected ? theme.ink1 : theme.ink2
                                                font.pixelSize: 13
                                                font.weight: customWrap.selected ? Font.Medium : Font.Normal
                                                elide: Text.ElideRight
                                            }
                                            Label {
                                                width: parent.width
                                                text: customWrap.modelData.subtitle || ""
                                                textFormat: Text.PlainText
                                                color: customWrap.selected ? theme.ink3 : theme.ink4
                                                font.pixelSize: 11
                                                elide: Text.ElideRight
                                            }
                                        }
                                        IconButton {
                                            id: removeBtn
                                            visible: customRow.hovered || customWrap.confirming
                                            anchors.right: parent.right
                                            anchors.rightMargin: 4
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: 24
                                            height: 24
                                            glyph: Icons.trash
                                            tone: theme.ink3
                                            tip: qsTr("Remove server")
                                            onClicked: {
                                                sheet.confirmRemoveId = customWrap.confirming ? "" : customWrap.modelData.id
                                            }
                                        }
                                    }
                                }

                                // Calm inline confirmation before removing a custom server.
                                Rectangle {
                                    visible: customWrap.confirming
                                    x: 10
                                    width: railCol.width - 20
                                    height: 36
                                    radius: Ui.radius(7, theme.radius)
                                    color: Ui.tint(theme.danger, 0.10)
                                    border.width: 1
                                    border.color: Ui.tint(theme.danger, 0.28)

                                    RowLayout {
                                        anchors.fill: parent
                                        anchors.leftMargin: 10
                                        anchors.rightMargin: 6
                                        spacing: 6
                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("Remove?")
                                            color: theme.ink1
                                            font.pixelSize: 12
                                            font.weight: Font.Medium
                                        }
                                        TextButton {
                                            ghost: true
                                            size: "compact"
                                            text: qsTr("Cancel")
                                            onClicked: sheet.confirmRemoveId = ""
                                        }
                                        TextButton {
                                            size: "compact"
                                            text: qsTr("Remove")
                                            onClicked: {
                                                const id = customWrap.modelData.id
                                                sheet.confirmRemoveId = ""
                                                opds.removeCatalog(id)
                                                if (opds.activeCatalogId === "" && opds.catalogs && opds.catalogs.length > 0)
                                                    opds.openCatalog(opds.catalogs[0].id)
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                // Rail footer: + Add OPDS server (+ restore public catalogs if hidden).
                Column {
                    id: railFooter
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    padding: 10
                    spacing: 6

                    Rectangle {
                        width: rail.width - 20
                        height: 1
                        color: theme.hairline
                    }

                    AbstractButton {
                        id: addServerBtn
                        width: rail.width - 20
                        height: 34
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: qsTr("Add OPDS server")
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        onClicked: {
                            if (sheet.addingServer)
                                sheet.addingServer = false
                            else
                                sheet.openAddServer()
                        }

                        background: Rectangle {
                            radius: Ui.radius(8, theme.radius)
                            color: sheet.addingServer ? theme.accentSoft
                                 : addServerBtn.pressed ? theme.fillPressed
                                 : addServerBtn.hovered ? theme.fillHover : Ui.tint(theme.ink1, 0.04)
                            border.width: addServerBtn.visualFocus ? 2 : 1
                            border.color: addServerBtn.visualFocus || sheet.addingServer
                                        ? theme.accent : Ui.tint(theme.ink1, 0.08)
                        }
                        contentItem: Item {
                            Row {
                                anchors.centerIn: parent
                                spacing: 6
                                Glyph {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 13
                                    height: 13
                                    stroke: 2.1
                                    path: Icons.plus
                                    color: sheet.addingServer ? theme.accentInk : theme.ink2
                                }
                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("Add OPDS server")
                                    color: sheet.addingServer ? theme.ink1 : theme.ink2
                                    font.pixelSize: 12
                                    font.weight: Font.Medium
                                }
                            }
                        }
                    }

                    TextButton {
                        visible: opds.hasHiddenBuiltIns
                        width: rail.width - 20
                        ghost: true
                        size: "compact"
                        text: qsTr("Restore public catalogs")
                        onClicked: opds.restoreBuiltInCatalogs()
                    }
                }
            }

            // --- Right browser pane ----------------------------------------
            Item {
                id: browserPane
                anchors.left: rail.right
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: parent.bottom

                // Top header bar: back, breadcrumbs / title, search, close.
                Item {
                    id: topBar
                    anchors.top: parent.top
                    anchors.left: parent.left
                    anchors.right: parent.right
                    height: 56

                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 14
                        anchors.rightMargin: 12
                        spacing: 8

                        IconButton {
                            visible: !sheet.addingServer
                            enabled: opds.canGoBack
                            glyph: Icons.back
                            tone: theme.ink2
                            tip: qsTr("Back")
                            onClicked: opds.goBack()
                        }

                        // Breadcrumb trail + current title.
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 1

                            Row {
                                visible: !sheet.addingServer && opds.breadcrumbs && opds.breadcrumbs.length > 1
                                Layout.fillWidth: true
                                spacing: 4
                                clip: true

                                Repeater {
                                    model: opds.breadcrumbs ? opds.breadcrumbs.slice(0, Math.max(0, opds.breadcrumbs.length - 1)) : []
                                    delegate: Row {
                                        id: crumbWrap
                                        required property var modelData
                                        required property int index
                                        spacing: 4

                                        AbstractButton {
                                            id: crumbBtn
                                            implicitHeight: 16
                                            implicitWidth: Math.min(130, crumbLbl.implicitWidth)
                                            hoverEnabled: true
                                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                                            onClicked: opds.navigateBreadcrumb(crumbWrap.index)
                                            contentItem: Label {
                                                id: crumbLbl
                                                text: crumbWrap.modelData.title || ""
                                                textFormat: Text.PlainText
                                                color: crumbBtn.hovered ? theme.ink1 : theme.ink3
                                                font.pixelSize: 11
                                                elide: Text.ElideRight
                                            }
                                        }
                                        Glyph {
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: 10
                                            height: 10
                                            stroke: 2
                                            path: Icons.chevronRight
                                            color: theme.ink4
                                        }
                                    }
                                }
                            }

                            Label {
                                Layout.fillWidth: true
                                text: sheet.addingServer
                                      ? qsTr("Add OPDS server")
                                      : (opds.feedTitle || opds.activeCatalogName || qsTr("Browse catalogs"))
                                textFormat: Text.PlainText
                                color: theme.ink1
                                font.pixelSize: 15
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                        }

                        // Inline catalog search input (visible when catalog advertises search).
                        Rectangle {
                            visible: !sheet.addingServer && opds.canSearch
                            Layout.preferredWidth: 210
                            Layout.preferredHeight: 30
                            radius: Ui.radius(7, theme.radius)
                            color: Ui.tint(theme.ink1, searchField.activeFocus ? 0.09 : 0.06)
                            border.width: searchField.activeFocus ? 1.5 : 0
                            border.color: theme.accent

                            Glyph {
                                id: searchGlyph
                                anchors.left: parent.left
                                anchors.leftMargin: 9
                                anchors.verticalCenter: parent.verticalCenter
                                width: 13
                                height: 13
                                stroke: 2
                                path: Icons.search
                                color: theme.ink4
                            }
                            TextInput {
                                id: searchField
                                anchors.left: searchGlyph.right
                                anchors.leftMargin: 7
                                anchors.right: clearSearchBtn.visible ? clearSearchBtn.left : parent.right
                                anchors.rightMargin: 6
                                anchors.verticalCenter: parent.verticalCenter
                                color: theme.ink1
                                selectionColor: Ui.tint(theme.accent, 0.35)
                                selectedTextColor: theme.ink1
                                font.pixelSize: 12
                                clip: true
                                selectByMouse: true
                                Accessible.role: Accessible.EditableText
                                Accessible.name: qsTr("Search catalog")
                                Keys.onReturnPressed: sheet.submitSearch()
                                Keys.onEnterPressed: sheet.submitSearch()

                                Text {
                                    anchors.fill: parent
                                    verticalAlignment: Text.AlignVCenter
                                    visible: searchField.text === ""
                                    text: qsTr("Search catalog…")
                                    color: theme.ink4
                                    font: searchField.font
                                    elide: Text.ElideRight
                                }
                            }
                            IconButton {
                                id: clearSearchBtn
                                visible: searchField.text !== ""
                                anchors.right: parent.right
                                anchors.rightMargin: 2
                                anchors.verticalCenter: parent.verticalCenter
                                width: 22
                                height: 22
                                tabFocusable: false
                                glyph: Icons.close
                                tone: theme.ink3
                                tip: qsTr("Clear")
                                onClicked: searchField.text = ""
                            }
                        }

                        IconButton {
                            width: 28
                            height: 28
                            glyph: Icons.close
                            tone: theme.ink3
                            tip: qsTr("Close (Esc)")
                            onClicked: sheet.close()
                        }
                    }

                    Rectangle {
                        anchors.bottom: parent.bottom
                        anchors.left: parent.left
                        anchors.right: parent.right
                        height: 1
                        color: theme.hairline
                    }
                }

                // Content host below header bar.
                Item {
                    anchors.top: topBar.bottom
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom

                    // --- State 1: Inline Add OPDS Server Form --------------
                    Flickable {
                        visible: sheet.addingServer
                        anchors.fill: parent
                        contentWidth: width
                        contentHeight: addFormCol.implicitHeight + 48
                        boundsBehavior: Flickable.StopAtBounds
                        clip: true
                        ScrollBar.vertical: ScrollBar {}
                        SmoothWheel { stepSize: 100 }

                        ColumnLayout {
                            id: addFormCol
                            x: Math.max(28, Math.round((parent.width - width) / 2))
                            y: 24
                            width: Math.min(440, Math.max(280, parent.width - 56))
                            spacing: 14

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Connect a self-hosted Calibre-Web, Kavita, Komga, Ubooquity, or Stump server, or any OPDS 1.2 / 2.0 catalog URL.")
                                color: theme.ink3
                                font.pixelSize: 13
                                wrapMode: Text.Wrap
                                lineHeight: 1.4
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 5
                                Label {
                                    text: qsTr("Name")
                                    color: theme.ink2
                                    font.pixelSize: 12
                                    font.weight: Font.Medium
                                }
                                TextField {
                                    id: serverNameField
                                    Layout.fillWidth: true
                                    placeholderText: qsTr("e.g. Home Calibre-Web")
                                    Keys.onReturnPressed: serverUrlField.forceActiveFocus()
                                    Keys.onEnterPressed: serverUrlField.forceActiveFocus()
                                }
                            }

                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 5
                                Label {
                                    text: qsTr("OPDS URL")
                                    color: theme.ink2
                                    font.pixelSize: 12
                                    font.weight: Font.Medium
                                }
                                TextField {
                                    id: serverUrlField
                                    Layout.fillWidth: true
                                    placeholderText: qsTr("https://books.example.org/opds")
                                    onTextChanged: if (sheet.addServerError !== "") sheet.addServerError = ""
                                    Keys.onReturnPressed: sheet.submitAddServer()
                                    Keys.onEnterPressed: sheet.submitAddServer()
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 10

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 5
                                    Label {
                                        text: qsTr("Username (optional)")
                                        color: theme.ink2
                                        font.pixelSize: 12
                                        font.weight: Font.Medium
                                    }
                                    TextField {
                                        id: serverUserField
                                        Layout.fillWidth: true
                                        placeholderText: qsTr("Username or email")
                                        Keys.onReturnPressed: serverPassField.forceActiveFocus()
                                        Keys.onEnterPressed: serverPassField.forceActiveFocus()
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 5
                                    Label {
                                        text: qsTr("Password (optional)")
                                        color: theme.ink2
                                        font.pixelSize: 12
                                        font.weight: Font.Medium
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 4
                                        TextField {
                                            id: serverPassField
                                            Layout.fillWidth: true
                                            placeholderText: qsTr("Password")
                                            echoMode: sheet.showAddPassword ? TextInput.Normal : TextInput.Password
                                            Keys.onReturnPressed: sheet.submitAddServer()
                                            Keys.onEnterPressed: sheet.submitAddServer()
                                        }
                                        IconButton {
                                            width: 30
                                            height: 30
                                            glyph: sheet.showAddPassword ? sheet.eyeOffIcon : sheet.eyeIcon
                                            tone: theme.ink3
                                            tip: sheet.showAddPassword ? qsTr("Hide password") : qsTr("Show password")
                                            onClicked: sheet.showAddPassword = !sheet.showAddPassword
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 7
                                Glyph {
                                    width: 14
                                    height: 14
                                    stroke: 1.8
                                    path: Icons.shieldCheck
                                    color: theme.positiveInk
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: qsTr("Password is stored in your desktop keyring")
                                    color: theme.ink3
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                            }

                            Label {
                                visible: sheet.addServerError !== ""
                                Layout.fillWidth: true
                                text: sheet.addServerError
                                textFormat: Text.PlainText
                                color: theme.danger
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: 6
                                Item { Layout.fillWidth: true }
                                TextButton {
                                    ghost: true
                                    text: qsTr("Cancel")
                                    onClicked: {
                                        sheet.addingServer = false
                                        sheet.addServerError = ""
                                    }
                                }
                                TextButton {
                                    primary: true
                                    text: qsTr("Add server")
                                    enabled: serverUrlField.text.trim() !== ""
                                    onClicked: sheet.submitAddServer()
                                }
                            }
                        }
                    }

                    // --- State 2: Calm loading skeleton --------------------
                    Column {
                        visible: !sheet.addingServer && opds.loading
                        anchors.fill: parent
                        anchors.margins: 20
                        spacing: 16

                        Repeater {
                            model: 5
                            delegate: RowLayout {
                                width: parent.width
                                spacing: 14

                                Skeleton {
                                    Layout.preferredWidth: 52
                                    Layout.preferredHeight: 76
                                    shimmer: !theme.reducedMotion
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Skeleton {
                                        Layout.preferredWidth: Math.min(280, parent.width * 0.65)
                                        Layout.preferredHeight: 15
                                        shimmer: !theme.reducedMotion
                                    }
                                    Skeleton {
                                        Layout.preferredWidth: Math.min(180, parent.width * 0.4)
                                        Layout.preferredHeight: 11
                                        shimmer: !theme.reducedMotion
                                    }
                                    Skeleton {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 11
                                        shimmer: !theme.reducedMotion
                                    }
                                }
                                Skeleton {
                                    Layout.preferredWidth: 74
                                    Layout.preferredHeight: 30
                                    shimmer: !theme.reducedMotion
                                }
                            }
                        }
                    }

                    // --- State 3: Inline HTTP 401 Sign-In Card -------------
                    Item {
                        visible: !sheet.addingServer && !opds.loading && opds.authRequired
                        anchors.fill: parent

                        Rectangle {
                            anchors.centerIn: parent
                            width: Math.min(410, parent.width - 48)
                            height: authCol.implicitHeight + 44
                            radius: Ui.radius(12, theme.radius)
                            color: theme.overlay
                            border.width: 1
                            border.color: theme.hairline

                            ColumnLayout {
                                id: authCol
                                anchors.fill: parent
                                anchors.margins: 22
                                spacing: 12

                                RowLayout {
                                    spacing: 10
                                    Rectangle {
                                        width: 32
                                        height: 32
                                        radius: Ui.radius(8, theme.radius)
                                        color: theme.accentSoft
                                        Glyph {
                                            anchors.centerIn: parent
                                            width: 16
                                            height: 16
                                            stroke: 1.8
                                            path: sheet.lockIcon
                                            color: theme.accentInk
                                        }
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 1
                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("Sign in to %1").arg(opds.activeCatalogName || qsTr("catalog"))
                                            textFormat: Text.PlainText
                                            color: theme.ink1
                                            font.pixelSize: 15
                                            font.weight: Font.DemiBold
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: qsTr("Authentication required")
                                            color: theme.ink3
                                            font.pixelSize: 12
                                        }
                                    }
                                }

                                Label {
                                    visible: opds.authHint !== ""
                                    Layout.fillWidth: true
                                    text: opds.authHint
                                    textFormat: Text.PlainText
                                    color: theme.ink2
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                    lineHeight: 1.4
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 5
                                    Label {
                                        text: qsTr("Username or email")
                                        color: theme.ink2
                                        font.pixelSize: 12
                                        font.weight: Font.Medium
                                    }
                                    TextField {
                                        id: authUserField
                                        Layout.fillWidth: true
                                        placeholderText: qsTr("Username or Patron email")
                                        Keys.onReturnPressed: sheet.submitAuth()
                                        Keys.onEnterPressed: sheet.submitAuth()
                                    }
                                }

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 5
                                    Label {
                                        text: qsTr("Password")
                                        color: theme.ink2
                                        font.pixelSize: 12
                                        font.weight: Font.Medium
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 4
                                        TextField {
                                            id: authPassField
                                            Layout.fillWidth: true
                                            placeholderText: qsTr("Optional for email-only catalogs")
                                            echoMode: sheet.showAuthPassword ? TextInput.Normal : TextInput.Password
                                            Keys.onReturnPressed: sheet.submitAuth()
                                            Keys.onEnterPressed: sheet.submitAuth()
                                        }
                                        IconButton {
                                            width: 30
                                            height: 30
                                            glyph: sheet.showAuthPassword ? sheet.eyeOffIcon : sheet.eyeIcon
                                            tone: theme.ink3
                                            tip: sheet.showAuthPassword ? qsTr("Hide password") : qsTr("Show password")
                                            onClicked: sheet.showAuthPassword = !sheet.showAuthPassword
                                        }
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 6
                                    Glyph {
                                        width: 13
                                        height: 13
                                        stroke: 1.8
                                        path: Icons.shieldCheck
                                        color: theme.positiveInk
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: qsTr("Password is stored in your desktop keyring")
                                        color: theme.ink3
                                        font.pixelSize: 11
                                        wrapMode: Text.Wrap
                                    }
                                }

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.topMargin: 4
                                    Item { Layout.fillWidth: true }
                                    TextButton {
                                        primary: true
                                        text: qsTr("Sign in")
                                        enabled: authUserField.text.trim() !== "" || authPassField.text !== ""
                                        onClicked: sheet.submitAuth()
                                    }
                                }
                            }
                        }
                    }

                    // --- State 4: Calm Error Retry -------------------------
                    Item {
                        visible: !sheet.addingServer && !opds.loading && !opds.authRequired && opds.error !== ""
                        anchors.fill: parent

                        ColumnLayout {
                            anchors.centerIn: parent
                            width: Math.min(380, parent.width - 48)
                            spacing: 12

                            Glyph {
                                Layout.alignment: Qt.AlignHCenter
                                width: 28
                                height: 28
                                stroke: 1.6
                                path: Icons.infoCircle
                                color: theme.ink3
                            }
                            Label {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: qsTr("Couldn't load catalog")
                                color: theme.ink1
                                font.pixelSize: 15
                                font.weight: Font.DemiBold
                            }
                            Label {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: opds.error
                                textFormat: Text.PlainText
                                color: theme.ink3
                                font.pixelSize: 13
                                wrapMode: Text.Wrap
                                lineHeight: 1.4
                            }
                            RowLayout {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.topMargin: 4
                                spacing: 8
                                TextButton {
                                    visible: opds.canGoBack
                                    ghost: true
                                    text: qsTr("Go back")
                                    onClicked: opds.goBack()
                                }
                                TextButton {
                                    primary: true
                                    glyph: sheet.refreshIcon
                                    text: qsTr("Try again")
                                    onClicked: opds.retry()
                                }
                            }
                        }
                    }

                    // --- State 5: Empty feed -------------------------------
                    Item {
                        visible: !sheet.addingServer && !opds.loading && !opds.authRequired
                                 && opds.error === "" && (!opds.entries || opds.entries.length === 0)
                        anchors.fill: parent

                        ColumnLayout {
                            anchors.centerIn: parent
                            width: Math.min(340, parent.width - 48)
                            spacing: 10

                            Label {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: qsTr("No books found here")
                                color: theme.ink2
                                font.pixelSize: 14
                                font.weight: Font.Medium
                            }
                            Label {
                                Layout.fillWidth: true
                                horizontalAlignment: Text.AlignHCenter
                                text: qsTr("Choose another section or try a different search.")
                                color: theme.ink3
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                            }
                            TextButton {
                                visible: opds.canGoBack
                                Layout.alignment: Qt.AlignHCenter
                                Layout.topMargin: 4
                                text: qsTr("Go back")
                                onClicked: opds.goBack()
                            }
                        }
                    }

                    // --- State 6: Entries List (Navigation + Publications) --
                    ListView {
                        id: feedList
                        visible: !sheet.addingServer && !opds.loading && !opds.authRequired
                                 && opds.error === "" && opds.entries && opds.entries.length > 0
                        anchors.fill: parent
                        topMargin: 12
                        bottomMargin: 20
                        spacing: 2
                        boundsBehavior: Flickable.StopAtBounds
                        maximumFlickVelocity: 4500
                        flickDeceleration: 2200
                        pixelAligned: true
                        clip: true
                        model: opds.entries || []
                        ScrollBar.vertical: ScrollBar {}
                        SmoothWheel { stepSize: 120 }

                        delegate: Column {
                            id: entryWrap
                            required property var modelData
                            required property int index

                            readonly property string groupTitle: modelData.groupTitle || ""
                            readonly property bool showGroupHeader: {
                                if (groupTitle === "")
                                    return false
                                if (index === 0)
                                    return true
                                const prev = opds.entries[index - 1]
                                return !prev || (prev.groupTitle || "") !== groupTitle
                            }

                            width: feedList.width

                            // Optional OPDS 2.0 group section header.
                            Item {
                                visible: entryWrap.showGroupHeader
                                width: parent.width
                                height: visible ? 34 : 0

                                RowLayout {
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.bottom: parent.bottom
                                    anchors.leftMargin: 20
                                    anchors.rightMargin: 20
                                    anchors.bottomMargin: 6

                                    Label {
                                        Layout.fillWidth: true
                                        text: entryWrap.groupTitle
                                        textFormat: Text.PlainText
                                        color: theme.ink3
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                        font.letterSpacing: 0.2
                                        elide: Text.ElideRight
                                    }

                                    AbstractButton {
                                        id: seeAllBtn
                                        visible: (entryWrap.modelData.groupUrl || "") !== ""
                                        implicitHeight: 20
                                        implicitWidth: seeAllLbl.implicitWidth + 4
                                        hoverEnabled: true
                                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                                        onClicked: opds.navigate(entryWrap.modelData.groupUrl, entryWrap.groupTitle)
                                        contentItem: Label {
                                            id: seeAllLbl
                                            text: qsTr("See all")
                                            color: seeAllBtn.hovered ? theme.accent : theme.accentInk
                                            font.pixelSize: 12
                                            font.weight: Font.Medium
                                        }
                                    }
                                }
                            }

                            // Navigation subsection row.
                            AbstractButton {
                                id: navRow
                                visible: !!entryWrap.modelData.isNavigation
                                x: 12
                                width: parent.width - 24
                                height: visible ? (navSub.visible ? 56 : 44) : 0
                                hoverEnabled: true
                                focusPolicy: Qt.TabFocus
                                Accessible.role: Accessible.ListItem
                                Accessible.name: entryWrap.modelData.title || ""
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                onClicked: opds.navigate(entryWrap.modelData.navigationUrl, entryWrap.modelData.title)

                                background: Rectangle {
                                    radius: Ui.radius(8, theme.radius)
                                    color: navRow.pressed ? theme.fillPressed
                                         : navRow.hovered ? theme.fill : "transparent"
                                    border.width: navRow.visualFocus ? 2 : 0
                                    border.color: theme.accent
                                }

                                contentItem: Item {
                                    Rectangle {
                                        id: navIconBox
                                        anchors.left: parent.left
                                        anchors.leftMargin: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 28
                                        height: 28
                                        radius: Ui.radius(6, theme.radius)
                                        color: Ui.tint(theme.ink1, 0.06)
                                        Glyph {
                                            anchors.centerIn: parent
                                            width: 14
                                            height: 14
                                            stroke: 1.7
                                            path: Icons.books
                                            color: theme.ink3
                                        }
                                    }
                                    Column {
                                        anchors.left: navIconBox.right
                                        anchors.leftMargin: 12
                                        anchors.right: navChevron.left
                                        anchors.rightMargin: 10
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 2
                                        Label {
                                            width: parent.width
                                            text: entryWrap.modelData.title || ""
                                            textFormat: Text.PlainText
                                            color: theme.ink1
                                            font.pixelSize: 14
                                            font.weight: Font.Medium
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            id: navSub
                                            visible: text !== ""
                                            width: parent.width
                                            text: entryWrap.modelData.subtitle || entryWrap.modelData.summary || ""
                                            textFormat: Text.PlainText
                                            color: theme.ink3
                                            font.pixelSize: 12
                                            elide: Text.ElideRight
                                        }
                                    }
                                    Glyph {
                                        id: navChevron
                                        anchors.right: parent.right
                                        anchors.rightMargin: 12
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 14
                                        height: 14
                                        stroke: 2
                                        path: Icons.chevronRight
                                        color: navRow.hovered ? theme.ink2 : theme.ink4
                                    }
                                }
                            }

                            // Acquisition publication card.
                            Rectangle {
                                id: pubCard
                                visible: !entryWrap.modelData.isNavigation
                                x: 12
                                width: parent.width - 24
                                height: visible ? Math.max(106, pubLayout.implicitHeight + 24) : 0
                                radius: Ui.radius(10, theme.radius)
                                color: pubHover.hovered ? Ui.tint(theme.ink1, 0.035) : "transparent"
                                Behavior on color {
                                    enabled: !theme.reducedMotion
                                    ColorAnimation { duration: 100 }
                                }

                                readonly property string primaryUrl: entryWrap.modelData.primaryAcquisitionUrl || ""
                                readonly property string primaryFormat: entryWrap.modelData.primaryFormat || ""
                                readonly property var dlState: primaryUrl !== "" ? (opds.downloadStates[primaryUrl] || null) : null
                                readonly property string dlStatus: dlState ? (dlState.status || "") : ""
                                readonly property real dlProgress: dlState ? (dlState.progress || 0) : 0
                                readonly property int dlBookId: dlState ? (dlState.bookId || 0) : 0
                                readonly property string dlError: dlState ? (dlState.error || "") : ""

                                HoverHandler { id: pubHover }

                                RowLayout {
                                    id: pubLayout
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    spacing: 14

                                    // Book cover thumbnail with graceful GeneratedCover fallback.
                                    Rectangle {
                                        Layout.preferredWidth: 54
                                        Layout.preferredHeight: 80
                                        Layout.alignment: Qt.AlignTop
                                        radius: Ui.radius(5, theme.radius)
                                        color: theme.fill
                                        clip: true

                                        GeneratedCover {
                                            anchors.fill: parent
                                            visible: coverImg.status !== Image.Ready
                                            title: entryWrap.modelData.title || ""
                                            author: entryWrap.modelData.author || ""
                                        }

                                        Image {
                                            id: coverImg
                                            anchors.fill: parent
                                            source: entryWrap.modelData.thumbnailUrl || entryWrap.modelData.coverUrl || ""
                                            asynchronous: true
                                            cache: true
                                            fillMode: Image.PreserveAspectCrop
                                            smooth: true
                                            mipmap: true
                                            sourceSize.width: 140
                                            sourceSize.height: 210
                                            opacity: status === Image.Ready ? 1 : 0
                                            Behavior on opacity {
                                                enabled: !theme.reducedMotion
                                                NumberAnimation { duration: 140; easing.type: Easing.OutCubic }
                                            }
                                        }

                                        // Subtle left spine crease matching CoverImage.qml.
                                        Rectangle {
                                            anchors.left: parent.left
                                            anchors.top: parent.top
                                            anchors.bottom: parent.bottom
                                            width: 2
                                            color: Qt.rgba(0, 0, 0, 0.22)
                                        }
                                    }

                                    // Title, author, pills, and 2-line summary.
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.alignment: Qt.AlignTop
                                        spacing: 4

                                        Label {
                                            Layout.fillWidth: true
                                            text: entryWrap.modelData.title || qsTr("Untitled")
                                            textFormat: Text.PlainText
                                            color: theme.ink1
                                            font.family: "Literata"
                                            font.pixelSize: 15
                                            font.weight: Font.Medium
                                            wrapMode: Text.Wrap
                                            maximumLineCount: 2
                                            elide: Text.ElideRight
                                            lineHeight: 1.18
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            visible: text !== ""
                                            text: {
                                                const a = entryWrap.modelData.author || ""
                                                const y = entryWrap.modelData.year || 0
                                                if (a !== "" && y > 0)
                                                    return a + " · " + y
                                                return a !== "" ? a : (y > 0 ? String(y) : "")
                                            }
                                            textFormat: Text.PlainText
                                            color: theme.ink2
                                            font.pixelSize: 12
                                            elide: Text.ElideRight
                                        }

                                        // Format + category pills.
                                        Flow {
                                            Layout.fillWidth: true
                                            Layout.topMargin: 2
                                            spacing: 5

                                            Rectangle {
                                                visible: pubCard.primaryFormat !== ""
                                                implicitWidth: fmtLbl.implicitWidth + 12
                                                implicitHeight: 19
                                                radius: Ui.radius(4, theme.radius)
                                                color: theme.accentSoft
                                                Label {
                                                    id: fmtLbl
                                                    anchors.centerIn: parent
                                                    text: pubCard.primaryFormat.toUpperCase()
                                                    color: theme.accentInk
                                                    font.pixelSize: 10
                                                    font.weight: Font.DemiBold
                                                    font.letterSpacing: 0.4
                                                }
                                            }

                                            // Secondary available format pill (e.g. PDF when EPUB is primary).
                                            Repeater {
                                                model: {
                                                    const acqs = entryWrap.modelData.acquisitions || []
                                                    const seen = {}
                                                    if (pubCard.primaryFormat !== "")
                                                        seen[pubCard.primaryFormat] = true
                                                    const alt = []
                                                    for (let i = 0; i < acqs.length; ++i) {
                                                        const f = acqs[i].format || ""
                                                        if (f !== "" && !seen[f]) {
                                                            seen[f] = true
                                                            alt.push(acqs[i])
                                                        }
                                                    }
                                                    return alt
                                                }
                                                delegate: AbstractButton {
                                                    id: altFmtBtn
                                                    required property var modelData
                                                    readonly property var altState: opds.downloadStates[modelData.href] || null
                                                    implicitWidth: altFmtLbl.implicitWidth + 12
                                                    implicitHeight: 19
                                                    hoverEnabled: true
                                                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                                                    ToolTip.visible: hovered
                                                    ToolTip.delay: 400
                                                    ToolTip.text: altState && altState.status === "done"
                                                                  ? qsTr("Open %1").arg(modelData.format.toUpperCase())
                                                                  : qsTr("Download %1 instead").arg(modelData.format.toUpperCase())
                                                    onClicked: {
                                                        if (altState && altState.status === "done" && altState.bookId > 0) {
                                                            sheet.openBook(altState.bookId)
                                                            sheet.close()
                                                        } else {
                                                            opds.downloadPublication(
                                                                altFmtBtn.modelData.href,
                                                                entryWrap.modelData.title || "",
                                                                entryWrap.modelData.author || "",
                                                                altFmtBtn.modelData.format || "",
                                                                sheet.targetCollectionId)
                                                        }
                                                    }
                                                    background: Rectangle {
                                                        radius: Ui.radius(4, theme.radius)
                                                        color: altFmtBtn.hovered ? theme.fillHover : Ui.tint(theme.ink1, 0.06)
                                                    }
                                                    contentItem: Label {
                                                        id: altFmtLbl
                                                        anchors.centerIn: parent
                                                        text: altFmtBtn.modelData.format.toUpperCase()
                                                        color: theme.ink3
                                                        font.pixelSize: 10
                                                        font.weight: Font.Medium
                                                    }
                                                }
                                            }

                                            // Up to 2 subject/category tags.
                                            Repeater {
                                                model: (entryWrap.modelData.categories || []).slice(0, 2)
                                                delegate: Rectangle {
                                                    required property string modelData
                                                    implicitWidth: Math.min(130, catTagLbl.implicitWidth + 12)
                                                    implicitHeight: 19
                                                    radius: Ui.radius(4, theme.radius)
                                                    color: Ui.tint(theme.ink1, 0.05)
                                                    Label {
                                                        id: catTagLbl
                                                        anchors.centerIn: parent
                                                        width: Math.min(implicitWidth, 118)
                                                        text: parent.modelData
                                                        textFormat: Text.PlainText
                                                        color: theme.ink3
                                                        font.pixelSize: 10
                                                        elide: Text.ElideRight
                                                    }
                                                }
                                            }

                                            Label {
                                                visible: (entryWrap.modelData.price || "") !== ""
                                                height: 19
                                                verticalAlignment: Text.AlignVCenter
                                                text: entryWrap.modelData.price || ""
                                                textFormat: Text.PlainText
                                                color: theme.ink3
                                                font.pixelSize: 11
                                            }
                                        }

                                        Label {
                                            Layout.fillWidth: true
                                            Layout.topMargin: 2
                                            visible: text !== ""
                                            text: entryWrap.modelData.summary || ""
                                            textFormat: Text.PlainText
                                            color: theme.ink3
                                            font.pixelSize: 12
                                            wrapMode: Text.Wrap
                                            maximumLineCount: 2
                                            elide: Text.ElideRight
                                            lineHeight: 1.38
                                        }
                                    }

                                    // Right 1-click Get -> Progress -> Open action area.
                                    ColumnLayout {
                                        Layout.preferredWidth: 104
                                        Layout.alignment: Qt.AlignVCenter
                                        spacing: 4

                                        // Open (downloaded)
                                        TextButton {
                                            visible: pubCard.dlStatus === "done" && pubCard.dlBookId > 0
                                            Layout.alignment: Qt.AlignRight
                                            primary: true
                                            size: "compact"
                                            glyph: Icons.bookOpen
                                            text: qsTr("Open")
                                            onClicked: {
                                                sheet.openBook(pubCard.dlBookId)
                                                sheet.close()
                                            }
                                        }

                                        // Downloading progress pill + cancel button.
                                        RowLayout {
                                            visible: pubCard.dlStatus === "downloading"
                                            Layout.alignment: Qt.AlignRight
                                            spacing: 4

                                            Rectangle {
                                                Layout.preferredWidth: 74
                                                Layout.preferredHeight: 28
                                                radius: Ui.radius(7, theme.radius)
                                                color: theme.accentSoft
                                                clip: true

                                                Rectangle {
                                                    anchors.left: parent.left
                                                    anchors.top: parent.top
                                                    anchors.bottom: parent.bottom
                                                    width: Math.max(6, Math.round(parent.width * Math.max(0.08, pubCard.dlProgress)))
                                                    color: Ui.tint(theme.accent, 0.35)
                                                    Behavior on width {
                                                        enabled: !theme.reducedMotion
                                                        NumberAnimation { duration: 120 }
                                                    }
                                                }
                                                Label {
                                                    anchors.centerIn: parent
                                                    text: pubCard.dlProgress > 0.01
                                                          ? Math.round(pubCard.dlProgress * 100) + "%"
                                                          : qsTr("Saving…")
                                                    color: theme.accentInk
                                                    font.pixelSize: 11
                                                    font.weight: Font.DemiBold
                                                    font.features: ({ "tnum": 1 })
                                                }
                                            }

                                            IconButton {
                                                width: 24
                                                height: 24
                                                glyph: Icons.close
                                                tone: theme.ink3
                                                tip: qsTr("Cancel download")
                                                onClicked: opds.cancelDownload(pubCard.primaryUrl)
                                            }
                                        }

                                        // Get / Retry button.
                                        TextButton {
                                            visible: pubCard.primaryUrl !== ""
                                                     && pubCard.dlStatus !== "downloading"
                                                     && !(pubCard.dlStatus === "done" && pubCard.dlBookId > 0)
                                            Layout.alignment: Qt.AlignRight
                                            size: "compact"
                                            glyph: pubCard.dlStatus === "error" ? sheet.refreshIcon : Icons.download
                                            text: pubCard.dlStatus === "error" ? qsTr("Retry") : qsTr("Get")
                                            ToolTip.visible: hovered && pubCard.dlError !== ""
                                            ToolTip.delay: 300
                                            ToolTip.text: pubCard.dlError
                                            onClicked: opds.downloadPublication(
                                                           pubCard.primaryUrl,
                                                           entryWrap.modelData.title || "",
                                                           entryWrap.modelData.author || "",
                                                           pubCard.primaryFormat,
                                                           sheet.targetCollectionId)
                                        }

                                        Label {
                                            visible: pubCard.dlStatus === "error" && pubCard.dlError !== ""
                                            Layout.fillWidth: true
                                            horizontalAlignment: Text.AlignRight
                                            text: qsTr("Failed")
                                            color: theme.danger
                                            font.pixelSize: 11
                                        }

                                        Label {
                                            visible: pubCard.primaryUrl === ""
                                            Layout.alignment: Qt.AlignRight
                                            text: qsTr("No EPUB/PDF")
                                            color: theme.ink4
                                            font.pixelSize: 11
                                        }
                                    }
                                }

                                // Quiet separator hairline beneath each card.
                                Rectangle {
                                    anchors.bottom: parent.bottom
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.leftMargin: 12
                                    anchors.rightMargin: 12
                                    height: 1
                                    color: theme.hairline
                                    opacity: 0.65
                                }
                            }
                        }

                        // Load more pagination footer.
                        footer: Item {
                            width: feedList.width
                            height: opds.canLoadMore ? 60 : 8

                            TextButton {
                                visible: opds.canLoadMore
                                anchors.centerIn: parent
                                enabled: !opds.loadingMore
                                text: opds.loadingMore ? qsTr("Loading…") : qsTr("Load more")
                                onClicked: opds.loadMore()
                            }
                        }
                    }
                }
            }
        }
    }
}
