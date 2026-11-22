import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// A quiet square button with a line icon, for toolbars.
AbstractButton {
    id: button

    property string glyph
    property string tip
    property bool active: false
    property color tone: theme.text

    implicitWidth: 36
    implicitHeight: 36
    hoverEnabled: true
    focusPolicy: Qt.NoFocus

    background: Rectangle {
        radius: Math.min(8, theme.radius)
        color: button.active ? Ui.tint(theme.accent, 0.18)
             : button.pressed ? Ui.tint(button.tone, 0.16)
             : button.hovered ? Ui.tint(button.tone, 0.08) : "transparent"
        Behavior on color { ColorAnimation { duration: 120 } }
    }

    contentItem: Item {
        Glyph {
            anchors.centerIn: parent
            width: 20
            height: 20
            path: button.glyph
            color: button.active ? theme.accent : button.tone
            opacity: button.enabled ? 1 : 0.4
        }
    }

    ToolTip.visible: hovered && tip !== ""
    ToolTip.text: tip
    ToolTip.delay: 700
}
