import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// "Go to": a printed page number when the book has them, or a percentage.
FocusScope {
    id: box

    property bool open: false
    property var look
    property string problem

    signal go(var target)
    signal closed()

    function show() {
        open = true
        problem = ""
        input.text = ""
        input.forceActiveFocus()
    }
    function close() {
        if (!open)
            return
        open = false
        closed()
    }
    function submit() {
        const text = input.text.trim()
        if (text === "")
            return
        const percent = text.match(/^(\d+(?:[.,]\d+)?)\s*%$/)
        let target = null
        if (percent) {
            target = reader.targetForFraction(Number(percent[1].replace(",", ".")) / 100)
        } else if (reader.hasPageList) {
            target = reader.targetForPageLabel(text.replace(/^p(age)?\.?\s*/i, ""))
            if (!target.chapter && target.chapter !== 0)
                target = null
        } else if (/^\d+(?:[.,]\d+)?$/.test(text) && Number(text.replace(",", ".")) <= 100) {
            target = reader.targetForFraction(Number(text.replace(",", ".")) / 100)
        }
        if (!target) {
            problem = reader.hasPageList ? qsTr("No page “%1” in this book").arg(text)
                                         : qsTr("Type a percentage, like 40%")
            return
        }
        close()
        go(target)
    }

    visible: open
    width: 420
    height: column.implicitHeight + 36

    Rectangle {
        anchors.fill: parent
        radius: Math.min(12, theme.radius)
        color: box.look.popover
        border.color: box.look.line
        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.topMargin: 4
            anchors.bottomMargin: -4
            radius: parent.radius
            color: "black"
            opacity: box.look.dark ? 0.35 : 0.12
        }
        // Keep clicks and wheel inside the box from falling through to the scrim.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    Column {
        id: column
        x: 18
        y: 18
        width: parent.width - 36
        spacing: 10

        Label {
            text: reader.hasPageList ? qsTr("Go to a page or a percentage") : qsTr("Go to a percentage")
            color: box.look.fg
            font.pixelSize: 15
            font.weight: Font.DemiBold
        }
        Rectangle {
            width: parent.width
            height: 38
            radius: Math.min(8, theme.radius)
            color: Ui.tint(box.look.fg, 0.06)
            border.width: 1.5
            border.color: box.look.link
            TextInput {
                id: input
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                verticalAlignment: TextInput.AlignVCenter
                color: box.look.fg
                font.pixelSize: 15
                selectByMouse: true
                Keys.onReturnPressed: box.submit()
                Keys.onEnterPressed: box.submit()
                Keys.onEscapePressed: box.close()
                Text {
                    anchors.fill: parent
                    verticalAlignment: Text.AlignVCenter
                    visible: input.text === ""
                    text: reader.hasPageList ? qsTr("87, or 40%") : qsTr("40%")
                    color: box.look.muted
                    font: input.font
                }
            }
        }
        Label {
            visible: box.problem !== ""
            text: box.problem
            color: theme.colors.red || "#e06c75"
            font.pixelSize: 13
        }
    }
}
