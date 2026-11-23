import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui

// The shell of a side panel: slides in from its edge over the page, with a
// title, a close button (Esc), a hairline edge and a soft shadow. Content
// goes inside, below the header; `headerExtra` sits beside the title.
FocusScope {
    id: panel

    property bool open: false
    property string title
    property string glyph
    property bool leftEdge: false
    property alias headerExtra: extra.data
    default property alias content: body.data
    readonly property real headerHeight: 52

    signal closed()

    function show() { open = true; forceActiveFocus() }
    function close() { if (!open) return; open = false; closed() }

    width: 400
    x: leftEdge ? (open ? 0 : -width - 24) : (open ? parent.width - width : parent.width + 24)
    Behavior on x { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
    visible: leftEdge ? x > -width - 20 : x < parent.width + 20
    Keys.onEscapePressed: close()

    // A soft shadow toward the page.
    Rectangle {
        x: panel.leftEdge ? parent.width : -width
        width: 16
        height: parent.height
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: panel.leftEdge ? 0 : 1; color: Ui.tint("#000000", theme.light ? 0.08 : 0.25) }
            GradientStop { position: panel.leftEdge ? 1 : 0; color: "transparent" }
        }
    }
    Rectangle {
        anchors.fill: parent
        color: panel.palette.light
        Rectangle {
            x: panel.leftEdge ? parent.width - 1 : 0
            width: 1
            height: parent.height
            color: panel.palette.dark
        }
    }
    // The panel keeps its clicks and the wheel: nothing falls through to the page.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    Item {
        id: header
        width: parent.width
        height: panel.headerHeight
        Row {
            anchors.left: parent.left
            anchors.leftMargin: 18
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Glyph {
                visible: panel.glyph !== ""
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                path: panel.glyph
                color: panel.palette.link
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: panel.title
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }
        }
        Row {
            id: extra
            anchors.right: closeButton.left
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
        }
        IconButton {
            id: closeButton
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            glyph: Icons.close
            tip: qsTr("Close (Esc)")
            onClicked: panel.close()
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: panel.palette.midlight
        }
    }

    Item {
        id: body
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
}
