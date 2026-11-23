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
    Layout.minimumHeight: 28
    spacing: 16
    opacity: enabled ? 1.0 : 0.48
    Behavior on opacity { NumberAnimation { duration: theme.reducedMotion ? 0 : 120 } }

    ColumnLayout {
        Layout.fillWidth: true
        Layout.alignment: Qt.AlignVCenter
        spacing: 2
        Label {
            Layout.fillWidth: true
            text: row.title
            color: theme.ink1
            font.pixelSize: 13
            font.weight: Font.Medium
            wrapMode: Text.Wrap
        }
        Label {
            visible: row.description !== ""
            Layout.fillWidth: true
            text: row.description
            color: theme.ink3
            font.pixelSize: 12
            wrapMode: Text.Wrap
            lineHeight: 1.3
        }
    }
    Row {
        id: slot
        Layout.alignment: Qt.AlignVCenter | Qt.AlignRight
        spacing: 8
    }
}
