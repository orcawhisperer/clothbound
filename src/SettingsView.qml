import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Icons.js" as Icons
import "Ui.js" as Ui

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

    function show(which) {
        if (which)
            page = which
        open = true
        forceActiveFocus()
        if (page === "intelligence")
            intelligence.prepare()
    }
    function close() {
        if (!open)
            return
        if (intelligence.budgetOpen) {
            intelligence.closeBudget()
            return
        }
        open = false
        closed()
    }
    function step(delta) {
        const ids = pages.map(p => p.id)
        page = ids[(ids.indexOf(page) + delta + ids.length) % ids.length]
        if (page === "intelligence")
            intelligence.prepare()
    }

    visible: opacity > 0
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: theme.reducedMotion ? 80 : 120 } }
    Keys.onEscapePressed: close()
    Keys.onUpPressed: (event) => { if (!(event.modifiers & Qt.ControlModifier)) return; step(-1) }
    Keys.onDownPressed: (event) => { if (!(event.modifiers & Qt.ControlModifier)) return; step(1) }

    Rectangle {
        anchors.fill: parent
        color: theme.ground
        // Nothing underneath reacts while settings are open.
        MouseArea {
            anchors.fill: parent
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
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.right: parent.right
            anchors.rightMargin: 10
            y: 54
            spacing: 1

            Repeater {
                model: settings.pages
                delegate: AbstractButton {
                    id: entry
                    required property var modelData
                    readonly property bool current: settings.page === modelData.id
                    width: parent.width
                    height: 30
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
                    Accessible.role: Accessible.PageTab
                    Accessible.name: modelData.label
                    Accessible.selected: current
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    onClicked: {
                        settings.page = modelData.id
                        if (modelData.id === "intelligence")
                            intelligence.prepare()
                    }
                    background: Rectangle {
                        radius: Ui.radius(7, theme.radius)
                        color: entry.current ? theme.accentSoft
                             : entry.pressed ? theme.fillPressed
                             : entry.hovered ? theme.fill : "transparent"
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
    }

    // --- The chosen page ---------------------------------------------------
    Flickable {
        id: scroller
        anchors.left: side.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        contentHeight: pageColumn.implicitHeight + 64
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar {}

        Connections {
            target: settings
            function onPageChanged() { scroller.contentY = 0 }
        }

        ColumnLayout {
            id: pageColumn
            x: settings.widePage ? Math.max(32, (scroller.width - width) / 2) : Math.max(32, (scroller.width - width) / 2)
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
            SettingsListening { visible: settings.page === "listening"; Layout.fillWidth: true }
            SettingsIntelligence { id: intelligence; visible: settings.page === "intelligence"; Layout.fillWidth: true }
            SettingsShortcuts { visible: settings.page === "shortcuts"; Layout.fillWidth: true }
            SettingsAbout { visible: settings.page === "about"; Layout.fillWidth: true }
        }
    }
}
