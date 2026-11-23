import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// One setting: its name and a sentence on the left, its control on the right.
RowLayout {
    id: row

    property string title
    property string description
    default property alias control: slot.data

    Layout.fillWidth: true
    spacing: 16

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 2
        Label {
            Layout.fillWidth: true
            text: row.title
            color: theme.ink1
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }
        Label {
            visible: row.description !== ""
            Layout.fillWidth: true
            text: row.description
            color: theme.ink3
            font.pixelSize: 12
            wrapMode: Text.Wrap
            lineHeight: 1.15
        }
    }
    Item {
        id: slot
        Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
        implicitWidth: childrenRect.width
        implicitHeight: childrenRect.height
    }
}
