import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui

// A centred modal card over a dimmed window: a title, an optional close
// button, and content. Esc or a click outside closes it (unless `modal`
// is strict). Content goes inside, below the title.
FocusScope {
    id: sheet

    property bool open: false
    property string title
    property string glyph
    property bool closable: true
    property real cardWidth: 560
    property real maxHeight: height - 96
    property alias card: card
    default property alias content: body.data

    signal closed()

    function show() { open = true; forceActiveFocus() }
    function close() { if (!open) return; open = false; closed() }

    anchors.fill: parent
    visible: opacity > 0
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140; easing.type: Easing.OutCubic } }
    Keys.onEscapePressed: if (closable) close()

    Rectangle {
        anchors.fill: parent
        color: theme.scrim
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onClicked: if (sheet.closable) sheet.close()
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(sheet.cardWidth, sheet.width - 48)
        height: Math.min(sheet.maxHeight, titleRow.height + body.childrenRect.height + 24)
        radius: Math.min(14, theme.radius)
        color: sheet.palette.light
        border.width: 1
        border.color: sheet.palette.dark
        scale: sheet.open ? 1 : 0.98
        Behavior on scale { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
        // Clicks on the card stay on the card.
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons }

        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.topMargin: 10
            anchors.margins: -4
            radius: parent.radius + 4
            color: sheet.palette.shadow
            opacity: 0.5
        }

        Item {
            id: titleRow
            width: parent.width
            height: sheet.title !== "" || sheet.closable ? 56 : 12
            Row {
                anchors.left: parent.left
                anchors.leftMargin: 22
                anchors.verticalCenter: parent.verticalCenter
                spacing: 9
                Glyph {
                    visible: sheet.glyph !== ""
                    anchors.verticalCenter: parent.verticalCenter
                    width: 18
                    height: 18
                    path: sheet.glyph
                    color: sheet.palette.link
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: sheet.title
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
            }
            IconButton {
                visible: sheet.closable
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                glyph: Icons.close
                tip: qsTr("Close (Esc)")
                onClicked: sheet.close()
            }
        }

        Item {
            id: body
            anchors.top: titleRow.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 22
            anchors.rightMargin: 22
            anchors.bottomMargin: 20
        }
    }
}
