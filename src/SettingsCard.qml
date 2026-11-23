import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A group of settings on a raised card, with an optional title and a
// sentence under it. Rows go inside.
Rectangle {
    id: card

    property string title
    property string description
    property string glyph
    property color glyphColor: theme.accentInk
    property alias headerExtra: extra.data
    default property alias content: body.data

    Layout.fillWidth: true
    implicitHeight: column.implicitHeight + 32
    radius: Math.min(12, theme.radius)
    color: theme.raised
    border.width: 1
    border.color: theme.hairline

    ColumnLayout {
        id: column
        x: 18
        y: 16
        width: parent.width - 36
        spacing: 12

        RowLayout {
            visible: card.title !== ""
            Layout.fillWidth: true
            spacing: 8
            Glyph {
                visible: card.glyph !== ""
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                path: card.glyph
                color: card.glyphColor
            }
            Label {
                Layout.fillWidth: true
                text: card.title
                color: theme.ink1
                font.pixelSize: 14
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Row { id: extra; spacing: 6 }
        }
        Label {
            visible: card.description !== ""
            Layout.fillWidth: true
            Layout.topMargin: card.title !== "" ? -6 : 0
            text: card.description
            color: theme.ink3
            font.pixelSize: 12
            wrapMode: Text.Wrap
            lineHeight: 1.2
        }
        ColumnLayout {
            id: body
            Layout.fillWidth: true
            spacing: 10
        }
    }
}
