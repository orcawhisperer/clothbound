import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui
import "../components/Themes.js" as Themes

// Everything marked in the book: highlights, notes, and bookmarks, in the
// book's order, by chapter. One goes to its place; all of them can be
// exported or copied as Markdown.
SidePanel {
    id: panel

    persistKey: "marksWidth"
    title: qsTr("Highlights & notes")
    glyph: Icons.bookmark

    property string filter: "all"
    property int revision: 0
    readonly property var filteredMarks: revision >= 0 ? marks.filtered(filter) : []
    readonly property var swatches: Themes.swatches

    signal chosen(int chapter, var offset)

    Connections {
        target: marks
        function onChanged() { panel.revision++ }
    }

    function show() {
        open = true
        list.currentIndex = panel.filteredMarks.length > 0 ? 0 : -1
        list.forceActiveFocus()
    }
    function activate(idx) {
        if (idx < 0 || idx >= panel.filteredMarks.length)
            return
        const item = panel.filteredMarks[idx]
        panel.chosen(item.chapter, item.start)
        panel.close()
    }
    function removeMark(item) {
        if (!item)
            return
        const isBookmark = item.kind === "bookmark"
        marks.remove(item.markId)
        toast.show(isBookmark ? qsTr("Bookmark removed") : qsTr("Highlight removed"),
                   qsTr("Undo"), () => marks.undo())
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.topMargin: 12
        anchors.bottomMargin: 12
        spacing: 10

        Segmented {
            id: filterTabs
            objectName: "marksFilterTabs"
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            compact: true
            model: [{ value: "all", label: qsTr("All") },
                    { value: "highlights", label: qsTr("Highlights") },
                    { value: "notes", label: qsTr("Notes") },
                    { value: "bookmarks", label: qsTr("Bookmarks") }]
            current: panel.filter
            onActivated: (value) => panel.filter = value
        }

        // Fill the remaining height whenever the list is empty so the tab bar
        // stays pinned to the top and the footer stays pinned to the bottom.
        Item {
            id: emptyState
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: marks.count === 0 || panel.filteredMarks.length === 0

            Label {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.leftMargin: 18
                anchors.rightMargin: 18
                anchors.topMargin: 18
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
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            visible: panel.filteredMarks.length > 0
            clip: true
            focus: true
            keyNavigationEnabled: true
            keyNavigationWraps: false
            currentIndex: count > 0 ? 0 : -1
            model: panel.filteredMarks
            boundsBehavior: Flickable.StopAtBounds
            maximumFlickVelocity: 4500
            flickDeceleration: 2200
            pixelAligned: true
            ScrollBar.vertical: ScrollBar {}
            SmoothWheel { stepSize: 110 }

            onCountChanged: {
                if (count === 0)
                    currentIndex = -1
                else if (currentIndex < 0 || currentIndex >= count)
                    currentIndex = Math.min(Math.max(0, currentIndex), count - 1)
            }

            Keys.onReturnPressed: panel.activate(currentIndex)
            Keys.onEnterPressed: panel.activate(currentIndex)
            Keys.onDeletePressed: {
                if (currentIndex >= 0 && currentIndex < panel.filteredMarks.length)
                    panel.removeMark(panel.filteredMarks[currentIndex])
            }
            Keys.onPressed: (event) => {
                if (event.key === Qt.Key_Backspace && currentIndex >= 0 && currentIndex < panel.filteredMarks.length) {
                    panel.removeMark(panel.filteredMarks[currentIndex])
                    event.accepted = true
                }
            }
            Keys.onEscapePressed: panel.close()

            delegate: Column {
                id: row
                required property var modelData
                required property int index
                readonly property bool firstInSection: modelData.label !== "" && (index === 0 || panel.filteredMarks[index - 1].label !== modelData.label)
                readonly property bool selected: list.currentIndex === index && list.activeFocus
                width: ListView.view.width

                Label {
                    visible: row.firstInSection
                    width: parent.width
                    leftPadding: 18
                    rightPadding: 18
                    topPadding: row.index === 0 ? 4 : 14
                    bottomPadding: 5
                    text: row.modelData.label
                    textFormat: Text.PlainText
                    color: panel.look.muted
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.22
                    elide: Text.ElideRight
                }

                AbstractButton {
                    id: entry
                    width: parent.width
                    height: content.implicitHeight + 16
                    hoverEnabled: true
                    focusPolicy: Qt.NoFocus
                    onClicked: {
                        list.currentIndex = row.index
                        panel.activate(row.index)
                    }
                    background: Rectangle {
                        anchors.fill: parent
                        anchors.leftMargin: 8
                        anchors.rightMargin: 8
                        radius: Ui.radius(7, theme.radius)
                        color: row.selected ? Ui.tint(panel.look.link, 0.16)
                             : entry.hovered ? Ui.tint(panel.look.fg, 0.05) : "transparent"
                    }
                    contentItem: Item {
                        RowLayout {
                            id: content
                            x: 18
                            y: 8
                            width: parent.width - 30
                            spacing: 10
                            Rectangle {
                                Layout.fillHeight: true
                                Layout.preferredWidth: 3
                                visible: row.modelData.kind === "highlight"
                                radius: 1.5
                                color: panel.swatches[row.modelData.color] || panel.swatches.yellow
                            }
                            Glyph {
                                Layout.alignment: Qt.AlignTop
                                Layout.topMargin: 2
                                visible: row.modelData.kind === "bookmark"
                                Layout.preferredWidth: 15
                                Layout.preferredHeight: 15
                                path: Icons.bookmark
                                color: panel.look.accentInk || panel.look.link
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
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                    maximumLineCount: 6
                                    elide: Text.ElideRight
                                }
                            }
                            IconButton {
                                Layout.alignment: Qt.AlignTop
                                implicitWidth: 28
                                implicitHeight: 28
                                opacity: entry.hovered || row.selected ? 1 : 0
                                look: panel.look
                                glyph: Icons.trash
                                tone: panel.look.muted
                                tip: qsTr("Remove")
                                onClicked: panel.removeMark(row.modelData)
                            }
                        }
                    }
                }
            }
        }

        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            visible: marks.count > 0
            color: panel.look.line
        }

        RowLayout {
            id: footerRow
            objectName: "marksFooterRow"
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            visible: marks.count > 0
            spacing: 6
            Label {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: qsTr("%Ln marks", "", marks.count)
                color: panel.look.muted
                font.pixelSize: 12
                font.features: ({ "tnum": 1 })
                elide: Text.ElideRight
            }
            TextButton {
                size: panel.width < 340 ? "compact" : "normal"
                ghost: true
                look: panel.look
                text: qsTr("Export .md")
                onClicked: {
                    const saved = marks.exportMarkdownToFile("", reader.title, reader.author, "obsidian")
                    if (saved !== "")
                        toast.show(qsTr("Saved notes to %1").arg(saved))
                    else
                        toast.show(qsTr("Could not save notes"))
                }
            }
            TextButton {
                size: panel.width < 340 ? "compact" : "normal"
                look: panel.look
                text: qsTr("Copy as Markdown")
                onClicked: {
                    reader.copyText(marks.markdown(reader.title, reader.author))
                    toast.show(qsTr("Your marks are copied"))
                }
            }
        }
    }
}
