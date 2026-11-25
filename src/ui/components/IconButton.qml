import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// A quiet square button with a line icon, for toolbars: no frame until the
// pointer is on it, the accent when it's active.
AbstractButton {
    id: button

    property string glyph
    property string tip
    property bool active: false
    property bool tabFocusable: true
    // Kept for reading-surface callers passing a Themes.palette() look object.
    property var look: null
    property color tone: look ? (look.inkSoft || look.fg) : palette.windowText
    readonly property color accentColor: look ? look.link : palette.link

    implicitWidth: 30
    implicitHeight: 30
    hoverEnabled: true
    focusPolicy: tabFocusable ? Qt.TabFocus : Qt.NoFocus
    Accessible.name: tip

    background: Rectangle {
        radius: Ui.radius(7, theme.radius)
        color: button.active ? (button.look ? Ui.tint(button.look.link, 0.16) : button.palette.linkVisited)
             : button.enabled && button.pressed ? Ui.tint(button.tone, 0.14)
             : button.enabled && button.hovered ? Ui.tint(button.tone, 0.08) : "transparent"
        Behavior on color { enabled: !theme.reducedMotion; ColorAnimation { duration: 100 } }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            visible: button.visualFocus
            radius: Ui.radius(10, theme.radius)
            color: "transparent"
            border.width: 2
            border.color: button.palette.highlight
        }
    }

    contentItem: Item {
        Glyph {
            anchors.centerIn: parent
            width: 16
            height: 16
            path: button.glyph
            color: button.active ? button.accentColor : button.tone
            opacity: button.enabled ? 1 : 0.4
        }
    }

    ToolTip.visible: hovered && tip !== ""
    ToolTip.text: tip
    ToolTip.delay: 600
}
