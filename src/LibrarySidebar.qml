import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui

// The library's sidebar: the shelves (Reading now, All books, Want to read,
// Finished, Listening), the reader's collections, and at the foot where
// intelligence runs. Ctrl+B folds it to a rail of icons.
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
    readonly property bool onShelf: works.collectionId === 0 && works.formatFilter === "" && works.seriesFilter === ""

    function showShelf(status) {
        works.clearFilters()
        works.statusFilter = status
    }
    function showCollection(id) {
        works.clearFilters()
        works.collectionId = id
    }

    width: collapsed ? railWidth : expandedWidth
    Behavior on width { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
    color: theme.sidebar
    clip: true

    Rectangle {
        anchors.right: parent.right
        width: 1
        height: parent.height
        color: theme.hairline
    }

    // One row of the sidebar: an icon, a label and a count, or the icon
    // alone with a tooltip on the rail.
    component ShelfRow: AbstractButton {
        id: row
        property string glyph
        property int count: -1
        property bool current: false
        signal contextRequested()

        width: side.width - 16
        x: 8
        height: 30
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        Accessible.role: Accessible.ListItem
        Accessible.name: text
        Accessible.selected: current

        background: Rectangle {
            radius: Math.min(7, theme.radius)
            color: row.current ? theme.accentSoft
                 : row.pressed ? theme.fillPressed
                 : row.hovered ? theme.fill : "transparent"
        }
        contentItem: Item {
            Glyph {
                id: icon
                anchors.left: parent.left
                anchors.leftMargin: side.collapsed ? (row.width - width) / 2 : 9
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                path: row.glyph
                color: row.current ? theme.accentInk : theme.ink3
            }
            Label {
                anchors.left: icon.right
                anchors.leftMargin: 10
                anchors.right: countLabel.left
                anchors.rightMargin: 6
                anchors.verticalCenter: parent.verticalCenter
                visible: !side.collapsed
                text: row.text
                color: row.current ? theme.ink1 : theme.ink2
                font.pixelSize: 13
                font.weight: row.current ? Font.DemiBold : Font.Normal
                elide: Text.ElideRight
            }
            Label {
                id: countLabel
                anchors.right: parent.right
                anchors.rightMargin: 9
                anchors.verticalCenter: parent.verticalCenter
                visible: !side.collapsed && row.count >= 0
                text: row.count >= 0 ? row.count.toLocaleString(Qt.locale(), 'f', 0) : ""
                color: theme.ink4
                font.pixelSize: 12
                font.features: { "tnum": 1 }
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

    component GroupLabel: Item {
        property alias text: label.text
        width: side.width
        height: side.collapsed ? 14 : 30
        Label {
            id: label
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 6
            visible: !side.collapsed
            color: theme.ink4
            font.pixelSize: 11
            font.weight: Font.DemiBold
            font.capitalization: Font.AllUppercase
            font.letterSpacing: 0.4
        }
        Rectangle {
            visible: side.collapsed
            anchors.centerIn: parent
            width: 20
            height: 1
            color: theme.hairline
        }
    }

    // --- Header: the name, and the button that folds the sidebar --------
    Item {
        id: header
        width: parent.width
        height: 52
        Label {
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            visible: !side.collapsed
            opacity: side.width > side.expandedWidth - 40 ? 1 : 0
            text: "Dogear"
            color: theme.ink1
            font.family: "Literata"
            font.pixelSize: 17
            font.weight: Font.DemiBold
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            anchors.left: parent.left
            anchors.leftMargin: side.collapsed ? (side.width - width) / 2 : side.width - width - 10
            glyph: Icons.sidebar
            tip: side.collapsed ? qsTr("Show sidebar (Ctrl+B)") : qsTr("Hide sidebar (Ctrl+B)")
            onClicked: side.toggleRequested()
        }
    }

    Flickable {
        id: scroller
        anchors.top: header.bottom
        anchors.bottom: footer.top
        width: parent.width
        contentHeight: column.implicitHeight + 12
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar {}

        Column {
            id: column
            width: parent.width
            spacing: 1

            GroupLabel { text: qsTr("Library"); height: side.collapsed ? 8 : 22 }
            ShelfRow {
                text: qsTr("Reading now")
                glyph: Icons.bookOpen
                count: side.counts.reading || 0
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
                glyph: Icons.star
                count: side.counts.want || 0
                current: side.onShelf && side.works.statusFilter === "want"
                onClicked: side.showShelf("want")
            }
            ShelfRow {
                text: qsTr("Finished")
                glyph: Icons.checkCircle
                count: side.counts.finished || 0
                current: side.onShelf && side.works.statusFilter === "finished"
                onClicked: side.showShelf("finished")
            }
            ShelfRow {
                visible: (side.counts.listening || 0) > 0
                text: qsTr("Listening")
                glyph: Icons.headphones
                count: side.counts.listening || 0
                current: side.onShelf && side.works.statusFilter === "listening"
                onClicked: side.showShelf("listening")
            }

            Item {
                width: side.width
                height: side.collapsed ? 14 : 34
                Label {
                    anchors.left: parent.left
                    anchors.leftMargin: 18
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 6
                    visible: !side.collapsed
                    text: qsTr("Collections")
                    color: theme.ink4
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 0.4
                }
                IconButton {
                    visible: !side.collapsed
                    anchors.right: parent.right
                    anchors.rightMargin: 10
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 0
                    width: 24
                    height: 24
                    glyph: Icons.plus
                    tip: qsTr("New collection")
                    onClicked: side.createCollection()
                }
                Rectangle {
                    visible: side.collapsed
                    anchors.centerIn: parent
                    width: 20
                    height: 1
                    color: theme.hairline
                }
            }
            Repeater {
                model: library.collections
                delegate: ShelfRow {
                    required property var modelData
                    text: modelData.name
                    glyph: Icons.stack
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
                visible: library.collections.length === 0 || side.collapsed
                text: qsTr("New collection")
                glyph: Icons.plus
                onClicked: side.createCollection()
                opacity: 0.8
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

    // --- Footer: where intelligence runs, opening its settings ----------
    AbstractButton {
        id: footer
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 10
        x: 8
        width: side.width - 16
        height: side.collapsed ? 40 : 56
        hoverEnabled: true
        focusPolicy: Qt.NoFocus

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
            if (ai.hasKey && where !== "local") {
                const spent = Number(ai.spentThisMonth).toLocaleCurrencyString(Qt.locale("en_US"), "$")
                return ai.unlimitedBudget || ai.budget <= 0
                    ? qsTr("%1 this month").arg(spent)
                    : qsTr("%1 of %2 this month").arg(spent)
                          .arg(Number(ai.budget).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
            }
            return ai.localTextModel !== "" ? ai.localTextModel : qsTr("Free and private")
        }
        Accessible.name: title + ", " + detail

        background: Rectangle {
            radius: Math.min(10, theme.radius)
            color: footer.pressed ? theme.fillPressed : footer.hovered ? theme.fillHover : side.collapsed ? "transparent" : theme.fill
            border.width: side.collapsed ? 0 : 1
            border.color: theme.hairline
        }
        contentItem: Item {
            Glyph {
                id: spark
                anchors.left: parent.left
                anchors.leftMargin: side.collapsed ? (footer.width - width) / 2 : 12
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                path: Icons.sparkle
                color: footer.where === "local" ? theme.positive
                     : footer.where === "cloud" ? theme.cloud
                     : footer.where === "hybrid" ? theme.accentInk : theme.ink3
            }
            Column {
                visible: !side.collapsed
                anchors.left: spark.right
                anchors.leftMargin: 10
                anchors.right: parent.right
                anchors.rightMargin: 10
                anchors.verticalCenter: parent.verticalCenter
                spacing: 2
                Label {
                    width: parent.width
                    text: footer.title
                    color: theme.ink1
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
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
        }
        onClicked: side.intelligenceRequested()
        ToolTip.visible: side.collapsed && hovered
        ToolTip.text: footer.title + " · " + footer.detail
        ToolTip.delay: 300
    }
}
