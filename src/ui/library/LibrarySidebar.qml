import QtQuick
import QtQuick.Controls
import ".."
import "../components"

import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui

// The library sidebar: Clothbound brand mark, core shelves, Listening (Downloaded &
// Audiobooks), color-swatched Collections, transient J2 watching card, and the
// Intelligence status card at the foot (01_library.html, 10_j1, 11_j2).
// Ctrl+B folds it to a 60 px icon rail on a damped 350 ms transition.
Rectangle {
    id: side

    property bool collapsed: false
    readonly property real expandedWidth: 232
    readonly property real railWidth: 60

    signal toggleRequested()
    signal createCollection()
    signal renameCollection(int id, string name)
    signal intelligenceRequested()

    readonly property var works: library.works
    readonly property var counts: library.shelfCounts
    readonly property bool emptyLibrary: library.bookCount === 0
    readonly property bool onShelf: works.collectionId === 0 && works.formatFilter === "" && works.seriesFilter === ""

    function showShelf(status) {
        works.clearFilters()
        works.statusFilter = status
    }
    function showCollection(id) {
        works.clearFilters()
        works.collectionId = id
    }
    function shortFolder(path) {
        if (!path)
            return ""
        return path.replace(/^\/home\/[^/]+/, "~")
    }

    width: collapsed ? railWidth : expandedWidth
    Behavior on width {
        enabled: !theme.reducedMotion
        NumberAnimation { duration: 350; easing.type: Easing.OutQuart }
    }
    color: theme.sidebar
    clip: true

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: theme.hairline
    }

    // One row of the sidebar: a glyph (or 10×10 color swatch), a label, and an
    // optional tabular count; collapses cleanly into a centered icon with tooltip.
    component ShelfRow: AbstractButton {
        id: row
        property string glyph
        property color swatchColor: "transparent"
        property int count: -1
        property bool current: false
        property bool muted: false
        signal contextRequested()

        width: side.width - 20
        x: 10
        height: 30
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        Accessible.role: Accessible.ListItem
        Accessible.name: text
        Accessible.selected: current

        background: Rectangle {
            radius: Ui.radius(7, theme.radius)
            color: row.current ? theme.accentSoft
                 : row.pressed ? theme.fillPressed
                 : row.hovered ? theme.fill : "transparent"
            border.width: row.visualFocus ? 2 : 0
            border.color: theme.accent
        }
        contentItem: Item {
            Glyph {
                id: icon
                visible: row.swatchColor.a === 0 && row.glyph !== ""
                anchors.left: parent.left
                anchors.leftMargin: side.collapsed ? (row.width - width) / 2 : 10
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                stroke: 1.7
                path: row.glyph
                color: row.current ? theme.accentInk : (row.muted ? theme.ink4 : theme.ink3)
            }
            Rectangle {
                id: swatch
                visible: row.swatchColor.a > 0
                anchors.left: parent.left
                anchors.leftMargin: side.collapsed ? (row.width - width) / 2 : 13
                anchors.verticalCenter: parent.verticalCenter
                width: 10
                height: 10
                radius: Ui.radius(3, theme.radius)
                color: row.swatchColor
            }
            Label {
                anchors.left: parent.left
                anchors.leftMargin: 36
                anchors.right: countLabel.visible ? countLabel.left : parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                visible: !side.collapsed
                text: row.text
                color: row.current ? theme.ink1 : (row.muted ? theme.ink4 : theme.ink2)
                font.pixelSize: 13
                font.weight: row.current ? Font.Medium : Font.Normal
                elide: Text.ElideRight
            }
            Label {
                id: countLabel
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                visible: !side.collapsed && row.count >= 0
                text: row.count >= 0 ? row.count.toLocaleString(Qt.locale(), "f", 0) : ""
                color: row.current ? theme.ink3 : theme.ink4
                font.pixelSize: 13
                font.features: ({ "tnum": 1 })
            }
        }
        TapHandler {
            acceptedButtons: Qt.RightButton
            onTapped: row.contextRequested()
        }
        ToolTip.visible: side.collapsed && hovered
        ToolTip.text: row.count >= 0 ? qsTr("%1 · %2").arg(row.text).arg(row.count) : row.text
        ToolTip.delay: 300
    }

    // Sentence-case section label (11/600, +0.02em) — collapses to a subtle rule.
    component GroupLabel: Item {
        property alias text: label.text
        width: side.width
        height: side.collapsed ? 18 : 38
        Label {
            id: label
            anchors.left: parent.left
            anchors.leftMargin: 20
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 6
            visible: !side.collapsed
            color: theme.ink4
            font.pixelSize: 11
            font.weight: Font.DemiBold
            font.letterSpacing: 0.22
        }
        Rectangle {
            visible: side.collapsed
            anchors.centerIn: parent
            width: 20
            height: 1
            color: theme.hairline
        }
    }

    // --- Header: folded-page mark, Inter 14/600 wordmark, collapse btn ---
    Item {
        id: header
        width: parent.width
        height: 54

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            spacing: 9
            visible: !side.collapsed
            opacity: side.width > side.expandedWidth - 40 ? 1 : 0

            ClothboundMark {
                anchors.verticalCenter: parent.verticalCenter
                size: 20
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: "Clothbound"
                color: theme.ink1
                font.pixelSize: 14
                font.weight: Font.DemiBold
                font.letterSpacing: -0.14
            }
        }

        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: side.collapsed ? (side.width - width) / 2 : side.width - width - 10
            width: 28
            height: 28
            glyph: Icons.sidebar
            tone: theme.ink3
            tip: side.collapsed ? qsTr("Show sidebar (Ctrl+B)") : qsTr("Hide sidebar (Ctrl+B)")
            onClicked: side.toggleRequested()
        }
    }

    Flickable {
        id: scroller
        anchors.top: header.bottom
        anchors.topMargin: 4
        anchors.bottom: footerCol.top
        anchors.bottomMargin: 8
        width: parent.width
        contentHeight: column.implicitHeight + 12
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar {}

        Column {
            id: column
            width: parent.width
            spacing: 1

            ShelfRow {
                text: qsTr("Reading now")
                glyph: Icons.bookOpen
                count: side.emptyLibrary ? -1 : (side.counts.reading || 0)
                muted: side.emptyLibrary
                current: side.onShelf && side.works.statusFilter === "reading"
                onClicked: side.showShelf("reading")
            }
            ShelfRow {
                text: qsTr("All books")
                glyph: Icons.books
                count: side.counts.all || 0
                current: side.onShelf && side.works.statusFilter === ""
                onClicked: side.showShelf("")
            }
            ShelfRow {
                text: qsTr("Want to read")
                glyph: Icons.bookmark
                count: side.emptyLibrary ? -1 : (side.counts.want || 0)
                muted: side.emptyLibrary
                current: side.onShelf && side.works.statusFilter === "want"
                onClicked: side.showShelf("want")
            }
            ShelfRow {
                text: qsTr("Finished")
                glyph: Icons.checkCircle
                count: side.emptyLibrary ? -1 : (side.counts.finished || 0)
                muted: side.emptyLibrary
                current: side.onShelf && side.works.statusFilter === "finished"
                onClicked: side.showShelf("finished")
            }

            // Listening section: Downloaded + Audiobooks (01_library.html).
            GroupLabel {
                visible: !side.emptyLibrary
                text: qsTr("Listening")
            }
            ShelfRow {
                visible: !side.emptyLibrary
                text: qsTr("Downloaded")
                glyph: Icons.headphones
                count: side.counts.downloaded || 0
                current: side.onShelf && side.works.statusFilter === "downloaded"
                onClicked: side.showShelf("downloaded")
            }
            ShelfRow {
                visible: !side.emptyLibrary
                text: qsTr("Audiobooks")
                glyph: Icons.disc
                count: side.counts.audiobooks || 0
                current: side.onShelf && side.works.statusFilter === "audiobooks"
                onClicked: side.showShelf("audiobooks")
            }

            // Collections section: color swatches + persistent + New collection.
            GroupLabel {
                visible: !side.emptyLibrary || library.collections.length > 0
                text: qsTr("Collections")
            }
            Repeater {
                model: library.collections
                delegate: ShelfRow {
                    required property var modelData
                    text: modelData.name
                    swatchColor: Ui.collectionColor(modelData.name)
                    count: modelData.count
                    current: side.works.collectionId === modelData.id
                    onClicked: side.showCollection(modelData.id)
                    onContextRequested: {
                        collectionMenu.target = modelData
                        collectionMenu.popup()
                    }
                }
            }
            ShelfRow {
                visible: !side.emptyLibrary || library.collections.length > 0 || side.collapsed
                text: qsTr("New collection")
                glyph: Icons.plus
                muted: true
                onClicked: side.createCollection()
            }
        }
    }

    Menu {
        id: collectionMenu
        property var target: null
        MenuItem {
            text: qsTr("Rename…")
            onTriggered: if (collectionMenu.target) side.renameCollection(collectionMenu.target.id, collectionMenu.target.name)
        }
        MenuItem {
            text: qsTr("Delete collection")
            onTriggered: {
                const col = collectionMenu.target
                if (!col)
                    return
                library.deleteCollection(col.id)
                toast.show(qsTr("Deleted “%1”").arg(col.name))
            }
        }
    }

    // --- Footer: transient Watching card (J2) & Intelligence status card --
    Column {
        id: footerCol
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        x: 10
        width: side.width - 20
        spacing: 8

        // J2 transient watching card while books arrive.
        Rectangle {
            visible: !side.collapsed && library.scanning && library.folders.length > 0
            width: parent.width
            height: watchCol.implicitHeight + 20
            radius: Ui.radius(10, theme.radius)
            color: Ui.tint(theme.ink1, 0.04)
            border.width: 1
            border.color: Ui.tint(theme.ink1, 0.06)

            Column {
                id: watchCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 10
                anchors.rightMargin: 10
                spacing: 4

                Label {
                    width: parent.width
                    text: library.folders.length > 0
                          ? qsTr("Watching %1").arg(side.shortFolder(library.folders[0])) : ""
                    textFormat: Text.PlainText
                    color: theme.ink1
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    elide: Text.ElideMiddle
                }
                Label {
                    width: parent.width
                    text: qsTr("New books there are added by themselves.")
                    color: theme.ink3
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                    lineHeight: 1.35
                }
            }
        }

        AbstractButton {
            id: footer
            visible: !(side.emptyLibrary && library.scanning)
            width: parent.width
            height: side.collapsed ? 40 : 52
            hoverEnabled: true
            focusPolicy: Qt.TabFocus

            readonly property bool setUp: ai.hasKey || ai.hasLocal
            readonly property string where: !setUp ? "none"
                                           : ai.preset === "local" ? "local"
                                           : ai.preset === "cloud" ? "cloud" : "hybrid"
            readonly property string title: where === "none" ? qsTr("Intelligence")
                                           : where === "local" ? qsTr("On this computer")
                                           : where === "cloud" ? qsTr("Gemini") : qsTr("Hybrid intelligence")
            readonly property string detail: {
                if (!setUp)
                    return qsTr("Optional · set up")
                if (where === "hybrid")
                    return qsTr("Gemini · this computer")
                if (where === "cloud") {
                    const spent = Number(ai.spentThisMonth).toLocaleCurrencyString(Qt.locale("en_US"), "$")
                    return ai.unlimitedBudget || ai.budget <= 0
                        ? qsTr("%1 this month").arg(spent)
                        : qsTr("%1 of %2").arg(spent)
                              .arg(Number(ai.budget).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
                }
                return ai.localTextModel !== "" ? ai.localTextModel : qsTr("Free and private")
            }
            Accessible.name: title + ", " + detail

            background: Rectangle {
                radius: Ui.radius(10, theme.radius)
                color: footer.pressed ? theme.fillPressed
                     : footer.hovered ? theme.fillHover
                     : side.collapsed ? "transparent" : Ui.tint(theme.ink1, 0.04)
                border.width: footer.visualFocus ? 2 : (side.collapsed ? 0 : 1)
                border.color: footer.visualFocus ? theme.accent : Ui.tint(theme.ink1, 0.06)
            }
            contentItem: Item {
                Glyph {
                    id: spark
                    anchors.left: parent.left
                    anchors.leftMargin: side.collapsed ? (footer.width - width) / 2 : 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 16
                    height: 16
                    stroke: 1.7
                    path: Icons.sparkle
                    color: footer.setUp ? theme.accentInk : theme.ink3
                }
                Column {
                    visible: !side.collapsed
                    anchors.left: spark.right
                    anchors.leftMargin: 10
                    anchors.right: statusDot.visible ? statusDot.left : parent.right
                    anchors.rightMargin: 8
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2
                    Label {
                        width: parent.width
                        text: footer.title
                        color: theme.ink1
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }
                    Label {
                        width: parent.width
                        text: footer.detail
                        color: theme.ink3
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }
                Rectangle {
                    id: statusDot
                    visible: !side.collapsed && footer.setUp
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.verticalCenter: parent.verticalCenter
                    width: 7
                    height: 7
                    radius: 3.5
                    color: footer.where === "cloud" ? theme.cloud : theme.positive
                    Accessible.name: qsTr("Connected")
                }
            }
            onClicked: side.intelligenceRequested()
            ToolTip.visible: hovered && (side.collapsed || (footer.setUp && ai.hasKey))
            ToolTip.text: {
                if (!footer.setUp)
                    return footer.title + " · " + footer.detail
                if (ai.hasKey && footer.where !== "local") {
                    const spent = Number(ai.spentThisMonth).toLocaleCurrencyString(Qt.locale("en_US"), "$")
                    const budgetStr = ai.unlimitedBudget || ai.budget <= 0
                        ? qsTr("%1 this month").arg(spent)
                        : qsTr("%1 of %2 this month").arg(spent)
                              .arg(Number(ai.budget).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
                    return footer.title + " · " + budgetStr
                }
                return footer.title + " · " + footer.detail
            }
            ToolTip.delay: 400
        }
    }
}
