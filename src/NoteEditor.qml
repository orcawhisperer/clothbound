import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Ui.js" as Ui

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
    width: Math.min(parent.width - 48, 520)
    height: column.implicitHeight + 36


    Rectangle {
        anchors.fill: parent
        radius: Math.min(12, theme.radius)
        color: editor.look.popover
        border.color: editor.look.line
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
            implicitHeight: Math.max(110, input.implicitHeight + 20)
            radius: Math.min(8, theme.radius)
            color: Ui.tint(editor.look.fg, 0.05)
            border.width: input.activeFocus ? 1.5 : 0
            border.color: editor.look.link
            TextEdit {
                id: input
                anchors.fill: parent
                anchors.margins: 10
                color: editor.look.fg
                selectionColor: Ui.tint(editor.look.link, 0.4)
                selectedTextColor: editor.look.fg
                font.pixelSize: 15
                wrapMode: TextEdit.Wrap
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
                    font.pixelSize: 15
                }
            }
        }
        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Ctrl+Enter to keep it")
                color: editor.look.muted
                font.pixelSize: 12
            }
            Button {
                flat: true
                text: qsTr("Cancel")
                onClicked: editor.close()
            }
            Button {
                highlighted: true
                text: qsTr("Keep")
                onClicked: editor.save()
            }
        }
    }
}
