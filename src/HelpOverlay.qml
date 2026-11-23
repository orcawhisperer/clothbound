import QtQuick
import QtQuick.Controls

import "Shortcuts.js" as Shortcuts
import "Ui.js" as Ui

// Every shortcut, shown with ?. The section for where you are comes first.
Rectangle {
    id: help

    property bool reading: false

    signal dismissed()

    color: Ui.tint("black", 0.72)

    TapHandler { onTapped: help.dismissed() }
    // The page underneath doesn't turn while the shortcuts are shown.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.NoButton
        onWheel: (wheel) => wheel.accepted = true
    }

    readonly property var librarySection: Shortcuts.library(translator.language)
    readonly property var readingSection: Shortcuts.reading(translator.language)
    readonly property var listeningSection: Shortcuts.listening(translator.language)
    readonly property var everywhere: Shortcuts.everywhere(translator.language)

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, columns.implicitWidth + 64)
        height: Math.min(parent.height - 48, columns.implicitHeight + 56)
        radius: Math.min(12, theme.radius)
        color: theme.ground
        border.color: Ui.tint(theme.ink1, 0.1)

        TapHandler {}

        Flickable {
            anchors.fill: parent
            anchors.leftMargin: 32
            anchors.rightMargin: 32
            anchors.topMargin: 28
            anchors.bottomMargin: 28
            contentWidth: columns.implicitWidth
            contentHeight: columns.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
            ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }

            Grid {
                id: columns
                columns: help.width >= 960 ? 2 : 1
                columnSpacing: 48
                rowSpacing: 24

                // Each column is a few sections; the ones for where you are come first.
                Repeater {
                    model: {
                        const reading = { title: qsTr("Reading"), items: help.readingSection }
                        const listening = { title: qsTr("Listening"), items: help.listeningSection }
                        const library = { title: qsTr("Library"), items: help.librarySection.concat(help.everywhere) }
                        return help.reading ? [[reading], [listening, library]] : [[library], [reading, listening]]
                    }
                    delegate: Column {
                        id: column
                        required property var modelData
                        spacing: 22

                        Repeater {
                            model: column.modelData
                            delegate: Column {
                                id: section
                                required property var modelData
                                spacing: 9

                                Label {
                                    text: section.modelData.title
                                    color: theme.ink1
                                    font.pixelSize: 16
                                    font.weight: Font.DemiBold
                                    bottomPadding: 6
                                }
                                Repeater {
                                    model: section.modelData.items
                                    delegate: Row {
                                        id: shortcutRow
                                        required property var modelData
                                        spacing: 18
                                        // The keys, as key badges, set right against their action.
                                        Item {
                                            width: 170
                                            height: Math.max(22, keyBadges.height)
                                            Row {
                                                id: keyBadges
                                                anchors.right: parent.right
                                                anchors.verticalCenter: parent.verticalCenter
                                                spacing: 4
                                                Repeater {
                                                    model: shortcutRow.modelData.keys.split(/\s{2,}/)
                                                    delegate: KeyHint {
                                                        required property string modelData
                                                        text: modelData
                                                    }
                                                }
                                            }
                                        }
                                        Label {
                                            topPadding: 2
                                            width: Math.min(implicitWidth, Math.max(180, help.width - 280))
                                            text: modelData.action
                                            color: theme.ink2
                                            font.pixelSize: 13
                                            wrapMode: Text.Wrap
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
}
