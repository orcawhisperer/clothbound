import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// A short message at the bottom of the window, with an optional action (Undo)
// and an optional keycap hint ("Enter"). Goes away by itself; Ctrl+Z runs the
// action while it shows.
Rectangle {
    id: toast

    property string message
    property string actionText
    property var action: null
    property string hint: ""

    // Supports:
    //   show(text)
    //   show(text, undoFn[, hintText])
    //   show(text, label, callback[, hintText])
    //   show(text, "", hintText)
    function show(text, a, b, c) {
        message = text
        if (typeof a === "function") {
            actionText = qsTr("Undo")
            action = a
            hint = (typeof b === "string") ? b : ""
        } else {
            actionText = a || ""
            action = (typeof b === "function") ? b : null
            hint = (typeof b === "string") ? b : ((typeof c === "string") ? c : "")
        }
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
    property real bottomClearance: 28

    anchors.horizontalCenter: parent.horizontalCenter
    y: parent.height - (shown || theme.reducedMotion ? height + bottomClearance : -height)
    Behavior on y {
        enabled: !theme.reducedMotion
        NumberAnimation { duration: 280; easing.type: Easing.OutQuart }
    }
    opacity: shown ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 160 } }
    visible: opacity > 0
    z: 50

    width: Math.min(row.implicitWidth + 28, parent.width - 48)
    height: 42
    radius: Ui.radius(12, theme.radius)
    color: theme.overlay
    border.width: 1
    border.color: Ui.tint(theme.ink1, 0.08)

    // Soft floating elevation shadow.
    Rectangle {
        z: -1
        anchors.fill: parent
        anchors.topMargin: 8
        anchors.margins: -3
        radius: toast.radius > 0 ? toast.radius + 3 : 0
        color: theme.shadow
        opacity: 0.45
    }

    Timer {
        id: timer
        interval: 5000
        onTriggered: toast.hide()
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 12
        Label {
            anchors.verticalCenter: parent.verticalCenter
            width: Math.min(implicitWidth, toast.parent.width - 200)
            text: toast.message
            color: theme.ink1
            font.pixelSize: 13
            elide: Text.ElideMiddle
        }
        AbstractButton {
            visible: toast.actionText !== ""
            anchors.verticalCenter: parent.verticalCenter
            hoverEnabled: true
            focusPolicy: Qt.NoFocus
            contentItem: Label {
                text: toast.actionText
                color: theme.accentInk
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }
            background: Item {}
            onClicked: toast.runAction()
            HoverHandler { cursorShape: Qt.PointingHandCursor }
        }
        KeyHint {
            visible: toast.hint !== ""
            anchors.verticalCenter: parent.verticalCenter
            text: toast.hint
            keycap: true
        }
    }
}
