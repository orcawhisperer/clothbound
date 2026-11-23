import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

import "Icons.js" as Icons
import "Ui.js" as Ui

// Everything marked in the book: highlights, notes, and bookmarks, in the
// book's order, by chapter. One goes to its place; all of them can be
// copied as Markdown to keep elsewhere.
FocusScope {
    id: panel

    property bool open: false
    property var look
    property string filter: "all"
    property int revision: 0
    readonly property var filteredMarks: revision >= 0 ? marks.filtered(filter) : []

    signal chosen(int chapter, var offset)
    signal closed()

    Connections {
        target: marks
        function onChanged() { panel.revision++ }
    }

    function show() {
        open = true
        forceActiveFocus()
    }
    function close() {
        if (!open)
            return
        open = false
        closed()
    }

    readonly property var swatches: ({ yellow: "#f2c94c", green: "#6fcf97", blue: "#56ccf2", pink: "#f78fb3" })

    Keys.onEscapePressed: close()

    width: 400
    x: open ? parent.width - width : parent.width + 24
    Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    visible: x < parent.width + 20

    Material.theme: look.dark ? Material.Dark : Material.Light
    Material.accent: look.link

    Rectangle {
        anchors.fill: parent
        color: panel.look.popover
        Rectangle { width: 1; height: parent.height; color: panel.look.border }
    }
    // The panel keeps its clicks and the wheel: nothing falls through to the page.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }
    Rectangle {
        anchors.right: parent.left
        width: 18
        height: parent.height
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: "transparent" }
            GradientStop { position: 1; color: Ui.tint("black", panel.look.dark ? 0.35 : 0.12) }
        }
    }

    component Chip: AbstractButton {
        id: chip
        property string value
        implicitHeight: 30
        implicitWidth: chipLabel.implicitWidth + 22
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        onClicked: panel.filter = value
        background: Rectangle {
            radius: Math.min(15, theme.radius + 6)
            color: panel.filter === chip.value ? Ui.tint(panel.look.link, 0.2) : chip.hovered ? Ui.tint(panel.look.fg, 0.08) : Ui.tint(panel.look.fg, 0.04)
            border.width: panel.filter === chip.value ? 1 : 0
            border.color: panel.look.link
        }
        contentItem: Label {
            id: chipLabel
            text: chip.text
            color: panel.look.fg
            font.pixelSize: 12
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 22
        anchors.bottomMargin: 16
        spacing: 12

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 22
            Layout.rightMargin: 14
            Label {
                Layout.fillWidth: true
                text: qsTr("Marks")
                color: panel.look.fg
                font.pixelSize: 18
                font.weight: Font.DemiBold
            }
            IconButton {
                glyph: Icons.close
                tone: panel.look.fg
                tip: qsTr("Close (Esc)")
                onClicked: panel.close()
            }
        }

        Flow {
            Layout.fillWidth: true
            Layout.leftMargin: 22
            Layout.rightMargin: 22
            spacing: 6
            Chip { text: qsTr("All"); value: "all" }
            Chip { text: qsTr("Highlights"); value: "highlights" }
            Chip { text: qsTr("Notes"); value: "notes" }
            Chip { text: qsTr("Bookmarks"); value: "bookmarks" }
        }

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 22
            Layout.rightMargin: 22
            Layout.topMargin: 20
            visible: marks.count === 0 || panel.filteredMarks.length === 0
            text: marks.count === 0
                ? qsTr("Select words on the page to highlight them or add a note. B bookmarks the page.")
                : panel.filter === "highlights" ? qsTr("No highlights yet.")
                : panel.filter === "notes" ? qsTr("No notes yet.")
                : panel.filter === "bookmarks" ? qsTr("No bookmarks yet.")
                : qsTr("No marks yet.")
            color: panel.look.muted
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: panel.filteredMarks.length > 0
            clip: true
            model: panel.filteredMarks
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            delegate: Column {
                id: row
                required property var modelData
                required property int index
                readonly property bool firstInSection: modelData.label !== "" && (index === 0 || panel.filteredMarks[index - 1].label !== modelData.label)
                width: ListView.view.width

                Label {
                    visible: row.firstInSection
                    width: parent.width
                    leftPadding: 22
                    rightPadding: 22
                    topPadding: row.index === 0 ? 4 : 14
                    bottomPadding: 6
                    text: row.modelData.label
                    textFormat: Text.PlainText
                    color: panel.look.muted
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 0.6
                    elide: Text.ElideRight
                }

                AbstractButton {
                    id: entry
                    width: parent.width
                    height: content.implicitHeight + 16
                    hoverEnabled: true
                    focusPolicy: Qt.NoFocus
                    onClicked: {
                        panel.chosen(row.modelData.chapter, row.modelData.start)
                        panel.close()
                    }
                    background: Rectangle {
                        color: entry.hovered ? Ui.tint(panel.look.fg, 0.05) : "transparent"
                    }
                    contentItem: Item {
                        RowLayout {
                            id: content
                            x: 22
                            y: 8
                            width: parent.width - 22 - 12
                            spacing: 12
                            Rectangle {
                                Layout.fillHeight: true
                                Layout.preferredWidth: 3
                                visible: row.modelData.kind === "highlight"
                                radius: 1.5
                                color: panel.swatches[row.modelData.color] || panel.swatches.yellow
                            }
                            Glyph {
                                Layout.alignment: Qt.AlignTop
                                visible: row.modelData.kind === "bookmark"
                                width: 16
                                height: 16
                                path: Icons.bookmark
                                color: panel.look.link
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 4
                                Label {
                                    Layout.fillWidth: true
                                    text: row.modelData.kind === "bookmark" ? row.modelData.quote + "…" : "“" + row.modelData.quote + "”"
                                    textFormat: Text.PlainText
                                    color: panel.look.fg
                                    font.family: "Literata"
                                    font.pixelSize: 14
                                    wrapMode: Text.Wrap
                                    maximumLineCount: 4
                                    elide: Text.ElideRight
                                }
                                Label {
                                    Layout.fillWidth: true
                                    visible: row.modelData.note !== ""
                                    text: row.modelData.note
                                    textFormat: Text.PlainText
                                    color: panel.look.muted
                                    font.pixelSize: 13
                                    wrapMode: Text.Wrap
                                    maximumLineCount: 6
                                    elide: Text.ElideRight
                                }
                            }
                            IconButton {
                                Layout.alignment: Qt.AlignTop
                                opacity: entry.hovered ? 1 : 0
                                glyph: Icons.trash
                                tone: panel.look.muted
                                tip: qsTr("Remove")
                                onClicked: marks.remove(row.modelData.markId)
                            }
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 22
            Layout.rightMargin: 22
            visible: marks.count > 0
            Label {
                Layout.fillWidth: true
                text: qsTr("%Ln marks", "", marks.count)
                color: panel.look.muted
                font.pixelSize: 12
            }
            Button {
                flat: true
                text: qsTr("Copy as Markdown")
                onClicked: {
                    reader.copyText(marks.markdown(reader.title, reader.author))
                    toast.show(qsTr("Your marks are copied"))
                }
            }
        }
    }
}
