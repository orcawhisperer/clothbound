import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui
import "Themes.js" as Themes

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
    signal askCompanion(string feature)
    signal remove()
    // Define answered in place, under the buttons; "More" opens the companion.
    signal defineHere()
    signal moreInCompanion()
    property bool defining: false
    onVisibleChanged: if (!visible) defining = false

    readonly property var colors: ["yellow", "green", "blue", "pink"]
    readonly property var swatches: Themes.swatches

    property var anchorRect: null
    property var anchorArea: null

    // Above the words, or below them when there's no room.
    function place(rect, area) {
        anchorRect = rect
        anchorArea = area
        const w = row.implicitWidth + 16
        x = Math.max(8, Math.min(area.width - w - 8, rect.x + rect.width / 2 - w / 2))
        y = rect.y - height - 10 >= 8 ? rect.y - height - 10 : Math.min(area.height - height - 8, rect.y + rect.height + 10)
    }
    onWidthChanged: if (visible && anchorRect && anchorArea) place(anchorRect, anchorArea)
    onHeightChanged: if (visible && anchorRect && anchorArea) place(anchorRect, anchorArea)

    width: defining ? Math.max(row.implicitWidth + 16, 360) : row.implicitWidth + 16
    height: defining ? 44 + definition.implicitHeight + 14 : 44
    radius: Math.min(10, theme.radius)
    color: look.popover
    border.color: look.line

    // Keep clicks on padding or dividers from turning the page underneath.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    Rectangle {
        z: -1
        anchors.fill: parent
        anchors.topMargin: 3
        anchors.bottomMargin: -3
        radius: parent.radius
        color: "black"
        opacity: bar.look.dark ? 0.35 : 0.12
    }

    // A word on the popover: Note, Define, Explain, Translate.
    component Word: AbstractButton {
        id: word
        property bool on: false
        anchors.verticalCenter: parent.verticalCenter
        implicitHeight: 30
        implicitWidth: wordLabel.implicitWidth + 16
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        background: Rectangle {
            radius: Math.min(6, theme.radius)
            color: word.on ? Ui.tint(bar.look.link, 0.16) : word.pressed ? Ui.tint(bar.look.fg, 0.14) : word.hovered ? Ui.tint(bar.look.fg, 0.08) : "transparent"
        }
        contentItem: Label {
            id: wordLabel
            text: word.text
            color: word.on ? bar.look.link : bar.look.fg
            font.pixelSize: 13
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    Row {
        id: row
        anchors.horizontalCenter: parent.horizontalCenter
        y: (44 - height) / 2
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
            color: bar.look.line
        }

        Word {
            on: bar.forMark && (bar.target.note || "") !== ""
            text: qsTr("Note")
            onClicked: bar.note()
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            glyph: Icons.copy
            look: bar.look
            tip: qsTr("Copy")
            onClicked: bar.copy()
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: 1
            height: 22
            color: bar.look.line
        }
        Word {
            visible: (bar.target.text || bar.target.quote || "").trim().split(/\s+/).length <= 3
            on: bar.defining
            text: qsTr("Define")
            onClicked: { bar.defining = true; bar.defineHere() }
        }
        Word { text: qsTr("Explain"); onClicked: bar.askCompanion("explain") }
        Word { text: qsTr("Translate"); onClicked: bar.askCompanion("translate") }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: !bar.forMark
            glyph: Icons.headphones
            look: bar.look
            tip: qsTr("Listen from here")
            onClicked: bar.listen()
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            visible: bar.forMark
            glyph: Icons.trash
            look: bar.look
            tip: qsTr("Remove the highlight")
            onClicked: bar.remove()
        }
    }

    // The definition, in place.
    Column {
        id: definition
        visible: bar.defining
        x: 14
        y: 46
        width: bar.width - 28
        spacing: 8
        Rectangle { width: parent.width; height: 1; color: bar.look.line }
        Row {
            visible: companion.busy && companion.feature === "define"
            spacing: 8
            BusyIndicator { width: 16; height: 16; padding: 0; running: parent.visible }
            Label { text: qsTr("Looking it up…"); color: bar.look.muted; font.pixelSize: 12 }
        }
        Label {
            width: parent.width
            visible: companion.feature === "define" && companion.problem !== ""
            text: companion.problem
            color: bar.look.muted
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        TextEdit {
            width: parent.width
            visible: companion.feature === "define" && companion.answer !== ""
            readOnly: true
            selectByMouse: true
            wrapMode: Text.Wrap
            textFormat: TextEdit.MarkdownText
            text: companion.feature === "define" ? companion.sanitizeMarkdown(companion.answer) : ""
            color: bar.look.fg
            selectionColor: Ui.tint(bar.look.link, 0.35)
            font.pixelSize: 14
            font.family: "Literata"
            height: Math.min(implicitHeight, 180)
            clip: true
        }
        Row {
            width: parent.width
            visible: companion.feature === "define" && !companion.busy && companion.answer !== ""
            spacing: 8
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 6; height: 6; radius: 3
                color: ai.routeFor("define") === "local" && ai.localIsLoopback ? theme.positive : theme.cloud
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                width: parent.width - 6 - moreLink.width - 24
                text: companion.where
                color: bar.look.muted
                font.pixelSize: 11
                elide: Text.ElideRight
            }
            Label {
                id: moreLink
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("More in Companion")
                color: bar.look.link
                font.pixelSize: 11
                font.weight: Font.Medium
                TapHandler { onTapped: bar.moreInCompanion() }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
            }
        }
    }
}
