import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui

// A search field: an icon, the text, and a way to clear it. Escape clears
// it, and a second Escape (or an empty one) hands the keys back.
FocusScope {
    id: box

    property alias text: input.text
    property string placeholder

    signal accepted()
    signal escaped()

    implicitWidth: 300
    implicitHeight: 36

    Rectangle {
        anchors.fill: parent
        radius: Math.min(8, theme.radius)
        color: Ui.tint(theme.ink1, input.activeFocus ? 0.09 : 0.06)
        border.width: input.activeFocus ? 1.5 : 0
        border.color: theme.accent
    }

    Glyph {
        id: icon
        anchors.left: parent.left
        anchors.leftMargin: 10
        anchors.verticalCenter: parent.verticalCenter
        width: 17
        height: 17
        path: Icons.search
        color: theme.ink3
    }

    TextInput {
        id: input
        anchors.left: icon.right
        anchors.leftMargin: 8
        anchors.right: clear.visible ? clear.left : parent.right
        anchors.rightMargin: 8
        anchors.verticalCenter: parent.verticalCenter
        focus: true
        color: theme.ink1
        selectionColor: Ui.tint(theme.accent, 0.4)
        selectedTextColor: theme.ink1
        font.pixelSize: 14
        clip: true
        selectByMouse: true
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
            color: theme.ink3
            font: input.font
            elide: Text.ElideRight
        }
    }

    IconButton {
        id: clear
        visible: input.text !== ""
        anchors.right: parent.right
        anchors.rightMargin: 3
        anchors.verticalCenter: parent.verticalCenter
        width: 30
        height: 30
        glyph: Icons.close
        tone: theme.ink2
        onClicked: input.text = ""
    }
}
