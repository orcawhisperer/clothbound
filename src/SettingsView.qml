import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Icons.js" as Icons

// Settings, as a full view of its own: pages in a sidebar (General, Reading,
// Listening, Intelligence, Shortcuts, About) and the chosen one beside it.
// Ctrl+, opens it over the library or a book; Esc or Back returns.
FocusScope {
    id: settings

    property bool open: false
    property string page: "general"

    signal closed()

    readonly property var pages: [
        { id: "general", label: qsTr("General"), glyph: Icons.gear },
        { id: "reading", label: qsTr("Reading"), glyph: Icons.bookOpen },
        { id: "listening", label: qsTr("Listening"), glyph: Icons.headphones },
        { id: "intelligence", label: qsTr("Intelligence"), glyph: Icons.sparkle },
        { id: "shortcuts", label: qsTr("Shortcuts"), glyph: Icons.keyboard },
        { id: "about", label: qsTr("About"), glyph: Icons.info }
    ]

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
    Behavior on opacity { NumberAnimation { duration: 120 } }
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

    // --- The pages -------------------------------------------------------
    Rectangle {
        id: side
        width: 220
        height: parent.height
        color: theme.sidebar
        Rectangle { anchors.right: parent.right; width: 1; height: parent.height; color: theme.hairline }

        AbstractButton {
            id: back
            x: 10
            y: 12
            height: 30
            width: backRow.implicitWidth + 16
            hoverEnabled: true
            focusPolicy: Qt.NoFocus
            Accessible.name: qsTr("Back")
            onClicked: settings.close()
            background: Rectangle {
                radius: Math.min(7, theme.radius)
                color: back.pressed ? theme.fillPressed : back.hovered ? theme.fill : "transparent"
            }
            contentItem: Item {
                Row {
                    id: backRow
                    anchors.verticalCenter: parent.verticalCenter
                    x: 6
                    spacing: 4
                    Glyph { anchors.verticalCenter: parent.verticalCenter; width: 14; height: 14; stroke: 2; path: Icons.chevronLeft; color: theme.ink2 }
                    Label { anchors.verticalCenter: parent.verticalCenter; text: qsTr("Back"); color: theme.ink2; font.pixelSize: 13 }
                }
            }
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Back (Esc)")
            ToolTip.delay: 600
        }
        Label {
            x: 18
            y: 56
            text: qsTr("Settings")
            color: theme.ink1
            font.pixelSize: 20
            font.weight: Font.DemiBold
        }
        Column {
            x: 8
            y: 96
            width: parent.width - 16
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
                    focusPolicy: Qt.NoFocus
                    Accessible.role: Accessible.PageTab
                    Accessible.name: modelData.label
                    Accessible.selected: current
                    onClicked: {
                        settings.page = modelData.id
                        if (modelData.id === "intelligence")
                            intelligence.prepare()
                    }
                    background: Rectangle {
                        radius: Math.min(7, theme.radius)
                        color: entry.current ? theme.accentSoft : entry.pressed ? theme.fillPressed : entry.hovered ? theme.fill : "transparent"
                    }
                    contentItem: Item {
                        Glyph {
                            id: icon
                            x: 9
                            anchors.verticalCenter: parent.verticalCenter
                            width: 16
                            height: 16
                            path: entry.modelData.glyph
                            color: entry.current ? theme.accentInk : theme.ink3
                        }
                        Label {
                            anchors.left: icon.right
                            anchors.leftMargin: 10
                            anchors.verticalCenter: parent.verticalCenter
                            text: entry.modelData.label
                            color: entry.current ? theme.ink1 : theme.ink2
                            font.pixelSize: 13
                            font.weight: entry.current ? Font.DemiBold : Font.Normal
                        }
                    }
                }
            }
        }
    }

    // --- The chosen page -------------------------------------------------
    Flickable {
        id: scroller
        anchors.left: side.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        contentHeight: pageColumn.implicitHeight + 80
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar {}

        Connections {
            target: settings
            function onPageChanged() { scroller.contentY = 0 }
        }

        ColumnLayout {
            id: pageColumn
            x: Math.max(32, (scroller.width - width) / 2)
            y: 40
            width: Math.min(680, scroller.width - 64)
            spacing: 16

            Label {
                text: settings.pages.find(p => p.id === settings.page).label
                color: theme.ink1
                font.pixelSize: 24
                font.weight: Font.DemiBold
                Layout.bottomMargin: 4
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
