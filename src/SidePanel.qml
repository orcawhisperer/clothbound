import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui

// The shell of a side panel: slides in from its edge over the page on a
// critically damped curve (~350 ms, fade under reduced motion), with a title,
// close button (Esc), hairline edge and soft shadow. Content goes inside below
// the header; `headerExtra` sits beside the title.
FocusScope {
    id: panel

    property bool open: false
    property string title
    property string glyph
    property bool leftEdge: false
    property var look: null
    property alias headerExtra: extra.data
    default property alias content: body.data
    readonly property real headerHeight: 52

    readonly property color surfaceColor: look ? (look.panel || look.popover) : palette.light
    readonly property color edgeColor: look ? look.line : palette.dark
    readonly property color dividerColor: look ? look.line : palette.midlight
    readonly property color accentTone: look ? look.link : palette.link

    signal closed()

    function show() { open = true; forceActiveFocus() }
    function close() { if (!open) return; open = false; closed() }

    width: 384
    property real slideProgress: open ? 1 : 0
    Behavior on slideProgress {
        NumberAnimation {
            duration: theme.reducedMotion ? 110 : (panel.open ? 260 : 210)
            easing.type: panel.open ? Easing.OutQuint : Easing.OutCubic
        }
    }
    readonly property real travel: width + 24
    x: theme.reducedMotion
       ? (leftEdge ? 0 : (parent ? parent.width - width : 0))
       : (leftEdge ? -travel * (1 - slideProgress)
                   : (parent ? parent.width - width + travel * (1 - slideProgress) : 0))
    opacity: theme.reducedMotion ? slideProgress : Math.min(1, slideProgress * 2.5)
    visible: slideProgress > 0.005
    Keys.onEscapePressed: close()

    // A soft shadow toward the page, fading smoothly with drawer travel.
    Rectangle {
        x: panel.leftEdge ? parent.width : -width
        width: 18
        height: parent.height
        opacity: panel.slideProgress
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: panel.leftEdge ? 0 : 1; color: Ui.tint("#000000", theme.light ? 0.08 : 0.28) }
            GradientStop { position: panel.leftEdge ? 1 : 0; color: "transparent" }
        }
    }
    Rectangle {
        anchors.fill: parent
        color: panel.surfaceColor
        Rectangle {
            x: panel.leftEdge ? parent.width - 1 : 0
            width: 1
            height: parent.height
            color: panel.edgeColor
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
            anchors.leftMargin: 16
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Glyph {
                visible: panel.glyph !== ""
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                path: panel.glyph
                color: panel.accentTone
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: panel.title
                color: panel.look ? panel.look.fg : panel.palette.windowText
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
            width: 28
            height: 28
            look: panel.look
            glyph: Icons.close
            tip: qsTr("Close (Esc)")
            onClicked: panel.close()
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: panel.dividerColor
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
