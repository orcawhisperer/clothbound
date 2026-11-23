import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// A quiet square button with a line icon, for toolbars: no frame until the
// pointer is on it, the accent when it's on.
AbstractButton {
    id: button

    property string glyph
    property string tip
    property bool active: false
    // Kept for older callers; the reading theme now arrives as the palette.
    property var look: null
    property color tone: palette.windowText
    readonly property color accentColor: palette.link

    implicitWidth: 32
    implicitHeight: 32
    hoverEnabled: true
    focusPolicy: Qt.NoFocus
    Accessible.name: tip

    background: Rectangle {
        radius: Math.min(7, theme.radius)
        color: button.active ? button.palette.linkVisited
             : button.enabled && button.pressed ? Ui.tint(button.tone, 0.14)
             : button.enabled && button.hovered ? Ui.tint(button.tone, 0.08) : "transparent"
        border.width: button.visualFocus ? 2 : 0
        border.color: button.palette.highlight
        Behavior on color { ColorAnimation { duration: 100 } }
    }

    contentItem: Item {
        Glyph {
            anchors.centerIn: parent
            width: 18
            height: 18
            path: button.glyph
            color: button.active ? button.accentColor : button.tone
            opacity: button.enabled ? 1 : 0.4
        }
    }

    ToolTip.visible: hovered && tip !== ""
    ToolTip.text: tip
    ToolTip.delay: 600
}
