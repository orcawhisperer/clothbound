import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui

// What to do with words on the page: over a selection, highlight it in a
// color, add a note, copy it, or listen from there; over a highlight, change
// its color or note, copy it, or remove it.
Rectangle {
    id: bar

    property var look
    // A selection: {chapter, start, end, text}; or a highlight: its mark.
    property var target: ({})
    property bool forMark: false

    signal highlight(string color)
    signal note()
    signal copy()
    signal listen()
    // "define", "explain" or "translate", for the companion.
    signal companion(string feature)
    signal remove()

    readonly property var colors: ["yellow", "green", "blue", "pink"]
    readonly property var swatches: ({ yellow: "#f2c94c", green: "#6fcf97", blue: "#56ccf2", pink: "#f78fb3" })

    // Above the words, or below them when there's no room.
    function place(rect, area) {
        x = Math.max(8, Math.min(area.width - width - 8, rect.x + rect.width / 2 - width / 2))
        y = rect.y - height - 10 >= 8 ? rect.y - height - 10 : Math.min(area.height - height - 8, rect.y + rect.height + 10)
    }

    width: row.implicitWidth + 16
    height: 44
    radius: Math.min(10, theme.radius)
    color: look.popover
    border.color: look.border

    Rectangle {
        z: -1
        anchors.fill: parent
        anchors.topMargin: 3
        anchors.bottomMargin: -3
        radius: parent.radius
        color: "black"
        opacity: bar.look.dark ? 0.35 : 0.12
    }

    Row {
        id: row
        anchors.centerIn: parent
        spacing: 2

        Repeater {
            model: bar.colors
            delegate: AbstractButton {
                id: swatch
                required property string modelData
                readonly property bool current: bar.forMark && bar.target.color === modelData
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: 30
                implicitHeight: 32
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                background: Item {}
                contentItem: Item {
                    Rectangle {
                        anchors.centerIn: parent
                        width: swatch.hovered || swatch.current ? 20 : 16
                        height: width
                        radius: width / 2
                        color: bar.swatches[swatch.modelData]
                        border.width: swatch.current ? 2 : 0
                        border.color: bar.look.fg
                        Behavior on width { NumberAnimation { duration: 90 } }
                    }
                }
                onClicked: bar.highlight(modelData)
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: bar.forMark ? qsTr("Change the color") : qsTr("Highlight")
            }
        }

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 1
            height: 22
            color: bar.look.border
        }

        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            glyph: Icons.note
            tone: bar.look.fg
            active: bar.forMark && (bar.target.note || "") !== ""
            tip: bar.forMark && (bar.target.note || "") !== "" ? qsTr("Edit the note") : qsTr("Add a note")
            onClicked: bar.note()
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            glyph: Icons.copy
            tone: bar.look.fg
            tip: qsTr("Copy")
            onClicked: bar.copy()
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 1
            height: 22
            color: bar.look.border
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: (bar.target.text || bar.target.quote || "").trim().split(/\s+/).length <= 3
            glyph: Icons.dictionary
            tone: bar.look.fg
            tip: qsTr("Define")
            onClicked: bar.companion("define")
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            glyph: Icons.sparkle
            tone: bar.look.fg
            tip: qsTr("Explain")
            onClicked: bar.companion("explain")
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            glyph: Icons.translate
            tone: bar.look.fg
            tip: qsTr("Translate")
            onClicked: bar.companion("translate")
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: !bar.forMark
            glyph: Icons.headphones
            tone: bar.look.fg
            tip: qsTr("Listen from here")
            onClicked: bar.listen()
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: bar.forMark
            glyph: Icons.trash
            tone: bar.look.fg
            tip: qsTr("Remove the highlight")
            onClicked: bar.remove()
        }
    }
}
