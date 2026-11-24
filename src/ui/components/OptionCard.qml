import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// A choice with a title and a sentence, as a card: one of a few exclusive
// options (role radio), the accent edge when chosen. `tone` colours its dot.
AbstractButton {
    id: card

    property string title
    property string description
    property color tone: palette.mid

    checkable: true
    autoExclusive: true
    hoverEnabled: true
    focusPolicy: Qt.TabFocus
    implicitWidth: 320
    implicitHeight: column.implicitHeight + 24
    Accessible.role: Accessible.RadioButton
    Accessible.name: title
    Accessible.description: description
    Accessible.checked: checked

    background: Rectangle {
        radius: Ui.radius(10, theme.radius)
        color: card.checked ? Ui.tint(card.palette.highlight, 0.12)
             : card.hovered ? Ui.tint(card.palette.windowText, 0.05) : Ui.tint(card.palette.windowText, 0.03)
        border.width: 1
        border.color: card.checked ? Ui.tint(card.palette.highlight, 0.6)
                                   : Ui.tint(card.palette.windowText, 0.08)
        Behavior on color { ColorAnimation { duration: 100 } }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            visible: card.visualFocus
            radius: Ui.radius(13, theme.radius)
            color: "transparent"
            border.width: 2
            border.color: card.palette.highlight
        }
    }

    contentItem: Item {
        Rectangle {
            id: dot
            anchors.left: parent.left
            anchors.leftMargin: 12
            y: 17
            width: 8
            height: 8
            radius: 4
            color: card.tone
        }
        Column {
            id: column
            anchors.left: dot.right
            anchors.leftMargin: 12
            anchors.right: parent.right
            anchors.rightMargin: 12
            y: 12
            spacing: 3
            Label {
                width: parent.width
                text: card.title
                font.pixelSize: 13
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }
            Label {
                width: parent.width
                visible: text !== ""
                text: card.description
                color: card.palette.mid
                font.pixelSize: 12
                lineHeight: 1.4
                wrapMode: Text.Wrap
            }
        }
    }
}
