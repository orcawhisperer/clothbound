import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

import "../components/Ui.js" as Ui

// A note on a highlight: the words it's about, and what you thought.
// Ctrl+Enter keeps it; Escape leaves it as it was.
FocusScope {
    id: editor

    property bool open: false
    property var look
    property int markId: 0
    property string quote: ""

    signal closed()

    function show(id) {
        const mark = marks.mark(id)
        if (!mark.id)
            return
        markId = id
        quote = mark.text
        input.text = mark.note
        inputScroll.contentY = 0
        open = true
        input.forceActiveFocus()
        input.cursorPosition = input.length
    }
    function save() {
        marks.setNote(markId, input.text)
        close()
    }
    function close() {
        if (!open)
            return
        open = false
        closed()
    }

    visible: open
    width: Math.min(parent ? parent.width - 48 : 520, 520)
    height: Math.min(parent ? Math.max(220, parent.height - 48) : 520, column.implicitHeight + 36)

    // Soft elevation shadow.
    Rectangle {
        z: -2
        anchors.fill: parent
        anchors.leftMargin: -4
        anchors.rightMargin: -4
        anchors.topMargin: 10
        anchors.bottomMargin: -16
        radius: cardBg.radius > 0 ? cardBg.radius + 4 : 0
        color: "black"
        opacity: editor.look.dark ? 0.42 : 0.14
    }
    Rectangle {
        z: -1
        anchors.fill: parent
        anchors.topMargin: 4
        anchors.bottomMargin: -6
        radius: cardBg.radius
        color: "black"
        opacity: editor.look.dark ? 0.30 : 0.08
    }

    Rectangle {
        id: cardBg
        anchors.fill: parent
        radius: Ui.radius(12, theme.radius)
        color: editor.look.popover
        border.width: 1
        border.color: editor.look.line
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    ColumnLayout {
        id: column
        x: 18
        y: 18
        width: parent.width - 36
        spacing: 12

        Label {
            Layout.fillWidth: true
            text: "“" + editor.quote + "”"
            color: editor.look.muted
            font.family: "Literata"
            font.italic: true
            font.pixelSize: 14
            wrapMode: Text.Wrap
            maximumLineCount: 4
            elide: Text.ElideRight
        }
        Rectangle {
            Layout.fillWidth: true
            readonly property real maxEditorHeight: Math.max(120, Math.min(320, (editor.parent ? editor.parent.height : 640) - 240))
            implicitHeight: Math.min(maxEditorHeight, Math.max(108, input.implicitHeight + 20))
            radius: Ui.radius(7, theme.radius)
            color: Ui.tint(editor.look.fg, 0.05)
            border.width: input.activeFocus ? 1.5 : 1
            border.color: input.activeFocus ? editor.look.link : editor.look.line
            clip: true

            Flickable {
                id: inputScroll
                anchors.fill: parent
                anchors.margins: 10
                clip: true
                contentWidth: width
                contentHeight: input.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                TextEdit {
                    id: input
                    width: inputScroll.width
                    height: Math.max(inputScroll.height, implicitHeight)
                    color: editor.look.fg
                    selectionColor: Ui.tint(editor.look.link, 0.4)
                    selectedTextColor: editor.look.fg
                    font.pixelSize: 13
                    wrapMode: TextEdit.Wrap
                    onCursorRectangleChanged: {
                        if (cursorRectangle.y < inputScroll.contentY)
                            inputScroll.contentY = cursorRectangle.y
                        else if (cursorRectangle.y + cursorRectangle.height > inputScroll.contentY + inputScroll.height)
                            inputScroll.contentY = cursorRectangle.y + cursorRectangle.height - inputScroll.height
                    }
                    Keys.onPressed: (event) => {
                        if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && (event.modifiers & Qt.ControlModifier)) {
                            editor.save()
                            event.accepted = true
                        } else if (event.key === Qt.Key_Escape) {
                            editor.close()
                            event.accepted = true
                        }
                    }
                    Label {
                        visible: !parent.text
                        text: qsTr("What you thought…")
                        color: editor.look.muted
                        font.pixelSize: 13
                    }
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Row {
                Layout.fillWidth: true
                spacing: 6
                KeyHint {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Ctrl ↵")
                    keycap: true
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("to keep it")
                    color: editor.look.muted
                    font.pixelSize: 12
                }
            }
            TextButton {
                ghost: true
                look: editor.look
                text: qsTr("Cancel")
                onClicked: editor.close()
            }
            TextButton {
                primary: true
                look: editor.look
                text: qsTr("Keep")
                onClicked: editor.save()
            }
        }
    }
}
