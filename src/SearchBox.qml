import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui

// A compact 30 px search field: inline library filter while typing, with a
// clickable Ctrl K badge when empty to open the full command palette
// (01_library.html). Escape clears it, and a second Escape hands focus back.
FocusScope {
    id: box

    property alias text: input.text
    property string placeholder
    property string shortcutHint: "Ctrl K"

    signal accepted()
    signal escaped()
    signal paletteRequested()

    implicitWidth: 260
    implicitHeight: 30

    Rectangle {
        anchors.fill: parent
        radius: Ui.radius(7, theme.radius)
        color: Ui.tint(theme.ink1, input.activeFocus ? 0.09 : 0.06)
        border.width: input.activeFocus ? 1.5 : 0
        border.color: theme.accent
    }

    Glyph {
        id: icon
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        width: 14
        height: 14
        stroke: 2
        path: Icons.search
        color: theme.ink4
    }

    TextInput {
        id: input
        anchors.left: icon.right
        anchors.leftMargin: 8
        anchors.right: clear.visible ? clear.left : (hintPill.visible ? hintPill.left : parent.right)
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        focus: true
        color: theme.ink1
        selectionColor: Ui.tint(theme.accent, 0.35)
        selectedTextColor: theme.ink1
        font.pixelSize: 13
        clip: true
        selectByMouse: true
        Accessible.role: Accessible.EditableText
        Accessible.name: box.placeholder
        Keys.onReturnPressed: box.accepted()
        Keys.onEnterPressed: box.accepted()
        Keys.onDownPressed: box.accepted()
        Keys.onEscapePressed: {
            if (input.text !== "")
                input.text = ""
            else
                box.escaped()
        }

        Text {
            anchors.fill: parent
            verticalAlignment: Text.AlignVCenter
            visible: input.text === ""
            text: box.placeholder
            color: theme.ink4
            font: input.font
            elide: Text.ElideRight
        }
    }

    KeyHint {
        id: hintPill
        visible: input.text === "" && box.shortcutHint !== ""
        anchors.right: parent.right
        anchors.rightMargin: 6
        anchors.verticalCenter: parent.verticalCenter
        text: box.shortcutHint
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: box.paletteRequested() }
    }

    IconButton {
        id: clear
        visible: input.text !== ""
        anchors.right: parent.right
        anchors.rightMargin: 3
        anchors.verticalCenter: parent.verticalCenter
        width: 24
        height: 24
        tabFocusable: false
        glyph: Icons.close
        tone: theme.ink3
        tip: qsTr("Clear search")
        onClicked: input.text = ""
    }
}
