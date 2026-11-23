import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui

// Settings, as a full view of its own: pages in a sidebar (General, Reading,
// Voices, Intelligence, Shortcuts, About) and the chosen one beside it.
// Ctrl+, opens it over the library or a book; Esc or Settings back returns.
FocusScope {
    id: settings

    property bool open: false
    // Internal page ids preserved ("listening" is labelled "Voices").
    property string page: "general"

    signal closed()

    readonly property var pages: [
        { id: "general", label: qsTr("General"), glyph: Icons.gear, subtitle: "" },
        { id: "reading", label: qsTr("Reading"), glyph: Icons.bookOpen, subtitle: "" },
        { id: "listening", label: qsTr("Voices"), glyph: Icons.headphones, subtitle: "" },
        { id: "intelligence", label: qsTr("Intelligence"), glyph: Icons.sparkle,
          subtitle: qsTr("All of it is optional: Dogear is a complete reader without a model. Each answer says where it was made.") },
        { id: "shortcuts", label: qsTr("Shortcuts"), glyph: Icons.keyboard, subtitle: "" },
        { id: "about", label: qsTr("About"), glyph: Icons.info, subtitle: "" }
    ]
    readonly property var currentPage: pages.find(p => p.id === settings.page) || pages[0]
    readonly property bool widePage: settings.page === "intelligence"

    function preparePage(which) {
        if (which === "intelligence")
            intelligence.prepare()
        else if (which === "listening")
            listeningPage.prepare()
        else if (which === "about")
            aboutPage.prepare()
    }
    function selectPage(which) {
        if (!pages.some(p => p.id === which))
            return
        const same = (page === which)
        page = which
        if (same)
            scroller.contentY = 0
        preparePage(which)
    }
    function show(which) {
        if (which && pages.some(p => p.id === which))
            page = which
        else if (!pages.some(p => p.id === page))
            page = "general"
        scroller.contentY = 0
        open = true
        forceActiveFocus()
        preparePage(page)
    }
    function close() {
        if (!open)
            return
        if (intelligence.budgetOpen) {
            intelligence.closeBudget()
            return
        }
        if (page === "intelligence" && intelligence.replacing) {
            intelligence.cancelReplace()
            return
        }
        if (page === "shortcuts" && shortcutsPage.clearFilterIfActive())
            return
        open = false
        closed()
    }
    function step(delta) {
        const ids = pages.map(p => p.id)
        const idx = Math.max(0, ids.indexOf(page))
        selectPage(ids[(idx + delta + ids.length) % ids.length])
    }

    visible: opacity > 0
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: theme.reducedMotion ? 0 : 120 } }
    Keys.onEscapePressed: (event) => {
        close()
        event.accepted = true
    }
    Keys.onUpPressed: (event) => {
        if (!(event.modifiers & Qt.ControlModifier))
            return
        step(-1)
        event.accepted = true
    }
    Keys.onDownPressed: (event) => {
        if (!(event.modifiers & Qt.ControlModifier))
            return
        step(1)
        event.accepted = true
    }

    Rectangle {
        anchors.fill: parent
        color: theme.ground
        // Nothing underneath reacts while settings are open.
        MouseArea {
            anchors.fill: parent
            enabled: settings.open
            acceptedButtons: Qt.AllButtons
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    // --- Sidebar (232 px) --------------------------------------------------
    Rectangle {
        id: side
        anchors.left: parent.left
        width: 232
        height: parent.height
        color: theme.sidebar
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: theme.hairline }

        AbstractButton {
            id: back
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.right: parent.right
            anchors.rightMargin: 10
            y: 14
            height: 30
            hoverEnabled: true
            focusPolicy: Qt.TabFocus
            Accessible.name: qsTr("Back from Settings")
            onClicked: settings.close()
            HoverHandler { cursorShape: Qt.PointingHandCursor }
            background: Rectangle {
                radius: Ui.radius(7, theme.radius)
                color: back.pressed ? theme.fillPressed : back.hovered ? theme.fill : "transparent"
                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -3
                    visible: back.visualFocus
                    radius: Ui.radius(10, theme.radius)
                    color: "transparent"
                    border.width: 2
                    border.color: theme.accent
                }
            }
            contentItem: Item {
                Row {
                    anchors.verticalCenter: parent.verticalCenter
                    anchors.left: parent.left
                    anchors.leftMargin: 8
                    spacing: 6
                    Glyph {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 15
                        height: 15
                        stroke: 2
                        path: Icons.chevronLeft
                        color: back.hovered ? theme.ink1 : theme.ink3
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Settings")
                        color: back.hovered ? theme.ink1 : theme.ink3
                        font.pixelSize: 13
                        font.weight: Font.Medium
                    }
                }
            }
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Back (Esc)")
            ToolTip.delay: 600
        }

        Column {
            id: navList
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.right: parent.right
            anchors.rightMargin: 10
            y: 54
            spacing: 2

            Repeater {
                id: navRepeater
                model: settings.pages
                delegate: AbstractButton {
                    id: entry
                    required property var modelData
                    required property int index
                    readonly property bool current: settings.page === modelData.id
                    width: parent.width
                    height: 32
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
                    Accessible.role: Accessible.PageTab
                    Accessible.name: modelData.label
                    Accessible.selected: current
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    onClicked: settings.selectPage(modelData.id)
                    Keys.onUpPressed: (event) => {
                        const prev = (entry.index - 1 + settings.pages.length) % settings.pages.length
                        settings.selectPage(settings.pages[prev].id)
                        const item = navRepeater.itemAt(prev)
                        if (item)
                            item.forceActiveFocus()
                        event.accepted = true
                    }
                    Keys.onDownPressed: (event) => {
                        const next = (entry.index + 1) % settings.pages.length
                        settings.selectPage(settings.pages[next].id)
                        const item = navRepeater.itemAt(next)
                        if (item)
                            item.forceActiveFocus()
                        event.accepted = true
                    }
                    Keys.onPressed: (event) => {
                        if (event.key === Qt.Key_Home) {
                            settings.selectPage(settings.pages[0].id)
                            const first = navRepeater.itemAt(0)
                            if (first)
                                first.forceActiveFocus()
                            event.accepted = true
                        } else if (event.key === Qt.Key_End) {
                            const lastIdx = settings.pages.length - 1
                            settings.selectPage(settings.pages[lastIdx].id)
                            const last = navRepeater.itemAt(lastIdx)
                            if (last)
                                last.forceActiveFocus()
                            event.accepted = true
                        }
                    }
                    background: Rectangle {
                        radius: Ui.radius(7, theme.radius)
                        color: entry.current ? theme.accentSoft
                             : entry.pressed ? theme.fillPressed
                             : entry.hovered ? theme.fill : "transparent"
                        Rectangle {
                            visible: entry.current
                            anchors.left: parent.left
                            anchors.leftMargin: 2
                            anchors.verticalCenter: parent.verticalCenter
                            width: 3
                            height: 14
                            radius: 1
                            color: theme.accent
                        }
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: -3
                            visible: entry.visualFocus
                            radius: Ui.radius(10, theme.radius)
                            color: "transparent"
                            border.width: 2
                            border.color: theme.accent
                        }
                    }
                    contentItem: Item {
                        Rectangle {
                            id: iconBadge
                            anchors.left: parent.left
                            anchors.leftMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            width: 20
                            height: 20
                            radius: Ui.radius(5, theme.radius)
                            color: entry.current ? theme.accent : Ui.tint(theme.ink1, 0.08)
                            Glyph {
                                anchors.centerIn: parent
                                width: 12
                                height: 12
                                stroke: 2.0
                                path: entry.modelData.glyph
                                color: entry.current ? theme.accentOn : theme.ink2
                            }
                        }
                        Label {
                            anchors.left: iconBadge.right
                            anchors.leftMargin: 10
                            anchors.right: parent.right
                            anchors.rightMargin: 8
                            anchors.verticalCenter: parent.verticalCenter
                            text: entry.modelData.label
                            color: entry.current ? theme.ink1 : theme.ink2
                            font.pixelSize: 13
                            font.weight: entry.current ? Font.Medium : Font.Normal
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 14
            spacing: 6
            opacity: 0.72
            KeyHint { text: "Esc"; keycap: true }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("Close")
                color: theme.ink4
                font.pixelSize: 11
            }
        }
    }

    // --- The chosen page ---------------------------------------------------
    Flickable {
        id: scroller
        anchors.left: side.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        contentWidth: width
        contentHeight: pageColumn.implicitHeight + 68
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar {}

        Connections {
            target: settings
            function onPageChanged() { scroller.contentY = 0 }
        }

        ColumnLayout {
            id: pageColumn
            x: Math.max(32, Math.round((scroller.width - width) / 2))
            y: 28
            width: settings.widePage ? Math.min(1040, Math.max(320, scroller.width - 64))
                                     : Math.min(680, Math.max(320, scroller.width - 64))
            spacing: settings.widePage ? 22 : 16

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                Label {
                    Layout.fillWidth: true
                    text: settings.currentPage.label
                    color: theme.ink1
                    font.pixelSize: theme.scale.pageTitle
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    visible: settings.currentPage.subtitle !== ""
                    text: settings.currentPage.subtitle
                    color: theme.ink3
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    lineHeight: 1.4
                }
            }

            SettingsGeneral { visible: settings.page === "general"; Layout.fillWidth: true }
            SettingsReading { visible: settings.page === "reading"; Layout.fillWidth: true }
            SettingsListening { id: listeningPage; visible: settings.page === "listening"; Layout.fillWidth: true }
            SettingsIntelligence { id: intelligence; visible: settings.page === "intelligence"; Layout.fillWidth: true }
            SettingsShortcuts { id: shortcutsPage; visible: settings.page === "shortcuts"; Layout.fillWidth: true }
            SettingsAbout { id: aboutPage; visible: settings.page === "about"; Layout.fillWidth: true }
        }
    }
}
