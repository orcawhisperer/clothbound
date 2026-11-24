import QtQuick
import QtQuick.Controls
import "../components"

import "../components/Ui.js" as Ui

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
    width: Math.min(parent ? parent.width - 48 : 420, 420)
    height: column.implicitHeight + 36

    // Soft elevation shadow.
    Rectangle {
        z: -2
        anchors.fill: parent
        anchors.leftMargin: -4
        anchors.rightMargin: -4
        anchors.topMargin: 8
        anchors.bottomMargin: -14
        radius: cardBg.radius > 0 ? cardBg.radius + 4 : 0
        color: "black"
        opacity: box.look.dark ? 0.36 : 0.12
    }
    Rectangle {
        z: -1
        anchors.fill: parent
        anchors.topMargin: 3
        anchors.bottomMargin: -5
        radius: cardBg.radius
        color: "black"
        opacity: box.look.dark ? 0.28 : 0.08
    }

    Rectangle {
        id: cardBg
        anchors.fill: parent
        radius: Ui.radius(12, theme.radius)
        color: box.look.popover
        border.width: 1
        border.color: box.look.line
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
            height: 36
            radius: Ui.radius(7, theme.radius)
            color: Ui.tint(box.look.fg, 0.06)
            border.width: 1.5
            border.color: box.look.link
            TextInput {
                id: input
                anchors.left: parent.left
                anchors.right: enterHint.left
                anchors.top: parent.top
                anchors.bottom: parent.bottom
                anchors.leftMargin: 12
                anchors.rightMargin: 8
                verticalAlignment: TextInput.AlignVCenter
                color: box.look.fg
                selectionColor: Ui.tint(box.look.link, 0.35)
                selectedTextColor: box.look.fg
                font.pixelSize: 13
                font.features: ({ "tnum": 1 })
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
            KeyHint {
                id: enterHint
                anchors.right: parent.right
                anchors.rightMargin: 8
                anchors.verticalCenter: parent.verticalCenter
                text: "↵"
                keycap: true
            }
        }
        Label {
            visible: box.problem !== ""
            text: box.problem
            color: theme.danger
            font.pixelSize: 12
        }
    }
}
