import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// A short message at the bottom of the window, with one optional action
// (Undo). It goes away by itself; Ctrl+Z also runs Undo while it shows.
Rectangle {
    id: toast

    property string message
    property string actionText
    property var action: null

    function show(text, label, callback) {
        message = text
        actionText = label || ""
        action = callback || null
        shown = true
        timer.restart()
    }
    function hide() { shown = false }
    function runAction() {
        if (!shown || !action)
            return false
        const callback = action
        hide()
        callback()
        return true
    }

    property bool shown: false

    anchors.horizontalCenter: parent.horizontalCenter
    y: parent.height - (shown ? height + 28 : -height)
    Behavior on y { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 180 } }
    visible: opacity > 0
    z: 50

    width: Math.min(row.implicitWidth + 36, parent.width - 48)
    height: 48
    radius: Math.min(10, theme.radius)
    color: theme.surface
    border.color: Ui.tint(theme.text, 0.1)

    Timer {
        id: timer
        interval: 5000
        onTriggered: toast.hide()
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 18
        Label {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, toast.parent.width - 200)
            text: toast.message
            color: theme.text
            font.pixelSize: 14
            elide: Text.ElideMiddle
        }
        AbstractButton {
            visible: toast.actionText !== ""
            anchors.verticalCenter: parent.verticalCenter
            hoverEnabled: true
            focusPolicy: Qt.NoFocus
            contentItem: Label {
                text: toast.actionText
                color: theme.accent
                font.pixelSize: 14
                font.weight: Font.DemiBold
            }
            background: Item {}
            onClicked: toast.runAction()
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }
    }
}
