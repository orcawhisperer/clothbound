import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Icons.js" as Icons
import "Ui.js" as Ui

// Finding words in the book: type, and every place they're found is listed
// by chapter; Enter goes to the next. Once you're reading at one, the list
// folds away and n and N go on stepping through them.
FocusScope {
    id: bar

    property bool open: false
    // The list is showing (while typing or choosing); folded otherwise.
    property bool expanded: false
    property var look

    signal jumped(var hit)
    signal closed()

    readonly property bool editing: open && expanded

    function show() {
        open = true
        expanded = true
        input.forceActiveFocus()
        input.selectAll()
    }
    function close() {
        if (!open)
            return
        open = false
        expanded = false
        bookSearch.clear()
        closed()
    }
    function fold() {
        expanded = false
        input.focus = false
    }
    function jump(index) {
        if (bookSearch.count === 0)
            return
        bookSearch.current = index
        const hit = bookSearch.results[bookSearch.current]
        if (hit)
            jumped(hit)
    }
    function submit(backwards) {
        typing.stop()
        bookSearch.find(input.text)
        if (backwards)
            previous()
        else
            next()
        fold()
    }
    function next() { jump(bookSearch.current + 1) }
    function previous() { jump(bookSearch.current < 0 ? bookSearch.count - 1 : bookSearch.current - 1) }

    function escaped(text) {
        return text.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;")
    }

    visible: open
    width: Math.min(parent.width - 48, 600)
    height: card.height

    Timer {
        id: typing
        interval: 220
        onTriggered: bookSearch.find(input.text)
    }

    Rectangle {
        id: card
        width: parent.width
        height: header.height + (bar.expanded && list.count > 0 ? list.height + 10 : 0)
                + (bar.expanded && note.visible ? note.height + 8 : 0)
        radius: Math.min(12, theme.radius)
        color: bar.look.popover
        border.color: bar.look.border
        clip: true

        RowLayout {
            id: header
            width: parent.width
            height: 50
            spacing: 6

            Glyph {
                Layout.leftMargin: 16
                width: 17
                height: 17
                path: Icons.search
                color: bar.look.muted
            }
            TextInput {
                id: input
                Layout.fillWidth: true
                Layout.leftMargin: 6
                color: bar.look.fg
                selectionColor: Ui.tint(bar.look.link, 0.4)
                selectedTextColor: bar.look.fg
                font.pixelSize: 15
                clip: true
                onTextEdited: {
                    bar.expanded = true
                    typing.restart()
                }
                onActiveFocusChanged: if (activeFocus) bar.expanded = true
                Keys.onReturnPressed: (event) => bar.submit(event.modifiers & Qt.ShiftModifier)
                Keys.onEnterPressed: (event) => bar.submit(event.modifiers & Qt.ShiftModifier)
                Keys.onEscapePressed: bar.close()
                Keys.onDownPressed: list.forceActiveFocus()

                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    visible: !parent.text
                    text: bookSearch.byMeaning ? qsTr("Describe what you're looking for") : qsTr("Search in the book")
                    color: bar.look.muted
                    font.pixelSize: 15
                }
            }
            // Words, or meaning.
            AbstractButton {
                id: meaning
                visible: bookSearch.canSearchByMeaning
                implicitHeight: 26
                implicitWidth: meaningLabel.implicitWidth + 18
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                onClicked: bookSearch.byMeaning = !bookSearch.byMeaning
                ToolTip.visible: hovered
                ToolTip.delay: 500
                ToolTip.text: qsTr("Find passages by what they're about, not their words. %1.").arg(bookSearch.where)
                background: Rectangle {
                    radius: 13
                    color: bookSearch.byMeaning ? Ui.tint(bar.look.link, 0.22) : meaning.hovered ? Ui.tint(bar.look.fg, 0.08) : "transparent"
                    border.width: 1
                    border.color: bookSearch.byMeaning ? bar.look.link : Ui.tint(bar.look.fg, 0.2)
                }
                contentItem: Label {
                    id: meaningLabel
                    text: qsTr("By meaning")
                    color: bookSearch.byMeaning ? bar.look.fg : bar.look.muted
                    font.pixelSize: 12
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
            Label {
                text: bookSearch.preparing !== "" ? bookSearch.preparing
                    : bookSearch.searching ? qsTr("Searching…")
                    : bookSearch.query.length < 2 ? ""
                    : bookSearch.count === 0 ? (bookSearch.byMeaning && bookSearch.query.length < 3 ? "" : qsTr("Not found"))
                    : bookSearch.current >= 0 && translator.language !== ""
                      ? qsTr("%1 of %2").arg((bookSearch.current + 1).toLocaleString(Qt.locale(), 'f', 0))
                                        .arg(bookSearch.count.toLocaleString(Qt.locale(), 'f', 0) + (bookSearch.capped ? "+" : ""))
                    : qsTr("%Ln found", "", bookSearch.count) + (bookSearch.capped ? "+" : "")
                color: bar.look.muted
                font.pixelSize: 13
            }
            IconButton {
                glyph: Icons.chevronLeft
                rotation: 90
                tone: bar.look.fg
                enabled: bookSearch.count > 0
                tip: qsTr("Previous (⇧N)")
                onClicked: bar.previous()
            }
            IconButton {
                glyph: Icons.chevronRight
                rotation: 90
                tone: bar.look.fg
                enabled: bookSearch.count > 0
                tip: qsTr("Next (N)")
                onClicked: bar.next()
            }
            IconButton {
                Layout.rightMargin: 8
                glyph: Icons.close
                tone: bar.look.muted
                tip: qsTr("Close (Esc)")
                onClicked: bar.close()
            }
        }

        Label {
            id: note
            anchors.top: header.bottom
            x: 18
            width: parent.width - 36
            visible: bar.expanded && bookSearch.capped && !bookSearch.searching
            text: qsTr("Showing the first %Ln. A few more words will narrow it down.", "", bookSearch.count)
            color: bar.look.muted
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }

        ListView {
            id: list
            anchors.top: note.visible ? note.bottom : header.bottom
            anchors.topMargin: note.visible ? 6 : 0
            x: 6
            width: parent.width - 12
            height: Math.min(contentHeight, bar.parent ? bar.parent.height * 0.55 : 400)
            visible: bar.expanded
            clip: true
            model: bookSearch.results
            currentIndex: bookSearch.current
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            keyNavigationEnabled: true
            Keys.onReturnPressed: { bar.jump(currentIndex); bar.fold() }
            Keys.onEscapePressed: bar.close()
            Keys.onUpPressed: currentIndex <= 0 ? input.forceActiveFocus() : decrementCurrentIndex()

            delegate: Column {
                id: row
                required property var modelData
                required property int index
                readonly property bool firstInChapter: index === 0
                    || bookSearch.results[index - 1].chapter !== modelData.chapter
                width: ListView.view.width

                Label {
                    visible: row.firstInChapter
                    width: parent.width
                    topPadding: row.index === 0 ? 4 : 12
                    bottomPadding: 4
                    leftPadding: 12
                    text: reader.titleOfChapter(row.modelData.chapter) || qsTr("Chapter %1").arg(row.modelData.chapter + 1)
                    color: bar.look.muted
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 0.6
                    elide: Text.ElideRight
                }
                AbstractButton {
                    id: hitButton
                    width: parent.width
                    implicitHeight: snippet.implicitHeight + 14
                    hoverEnabled: true
                    focusPolicy: Qt.NoFocus
                    onClicked: { bar.jump(row.index); bar.fold() }
                    background: Rectangle {
                        radius: Math.min(6, theme.radius)
                        color: row.index === bookSearch.current || row.ListView.isCurrentItem && list.activeFocus
                               ? Ui.tint(bar.look.link, 0.16) : hitButton.hovered ? Ui.tint(bar.look.fg, 0.06) : "transparent"
                    }
                    contentItem: Label {
                        id: snippet
                        leftPadding: 12
                        rightPadding: 12
                        textFormat: Text.StyledText
                        text: bar.escaped(row.modelData.before) + "<b>" + bar.escaped(row.modelData.match) + "</b>"
                              + bar.escaped(row.modelData.after)
                        color: bar.look.fg
                        font.pixelSize: 13
                        font.family: "Literata"
                        wrapMode: Text.Wrap
                        maximumLineCount: 2
                        elide: Text.ElideRight
                    }
                }
            }
        }
    }
}
