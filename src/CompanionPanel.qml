import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

import "Icons.js" as Icons
import "Ui.js" as Ui

// The reading companion, beside the page: Ask about the book, Previously…,
// Who's who, and what was asked of a selection (Define, Explain,
// Translate). Nothing past the reader's place is ever seen.
FocusScope {
    id: panel

    property bool open: false
    property var look
    // "ask", "recap", "whoswho", or "selection" (define, explain, translate).
    property string tab: "ask"

    signal closed()

    function show(which) {
        if (which)
            tab = which
        open = true
        forceActiveFocus()
        if (tab === "ask")
            question.forceActiveFocus()
    }
    function close() {
        if (!open)
            return
        open = false
        closed()
    }
    function showTab(which) {
        tab = which
        if (which === "recap" && (companion.feature !== "recap" || companion.problem !== ""))
            companion.recap()
        else if (which === "whoswho")
            companion.whosWho()
        else if (which === "ask")
            question.forceActiveFocus()
    }
    function send() {
        if (question.text.trim() === "" || companion.busy)
            return
        companion.ask(question.text)
        question.text = ""
    }

    readonly property bool selectionFeature: ["define", "explain", "translate"].indexOf(companion.feature) >= 0
    readonly property string featureName: ({ define: qsTr("Define"), explain: qsTr("Explain"), translate: qsTr("Translate") })[companion.feature] || ""

    Keys.onEscapePressed: close()

    width: 440
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
        onClicked: panel.showTab(value)
        background: Rectangle {
            radius: Math.min(15, theme.radius + 6)
            color: panel.tab === chip.value ? Ui.tint(panel.look.link, 0.2) : chip.hovered ? Ui.tint(panel.look.fg, 0.08) : Ui.tint(panel.look.fg, 0.04)
            border.width: panel.tab === chip.value ? 1 : 0
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
    // Words to read, selectable for copying.
    component Prose: TextEdit {
        Layout.fillWidth: true
        readOnly: true
        selectByMouse: true
        wrapMode: Text.Wrap
        textFormat: TextEdit.MarkdownText
        color: panel.look.fg
        selectionColor: Ui.tint(panel.look.link, 0.35)
        font.pixelSize: 15
        font.family: "Literata"
    }
    component Where: Label {
        Layout.fillWidth: true
        color: panel.look.muted
        font.pixelSize: 12
        wrapMode: Text.Wrap
    }
    component Working: RowLayout {
        visible: companion.busy
        spacing: 8
        BusyIndicator { running: parent.visible; implicitWidth: 20; implicitHeight: 20 }
        Label { text: companion.status; color: panel.look.muted; font.pixelSize: 13 }
        Item { Layout.fillWidth: true }
        TextButton { text: qsTr("Stop"); onClicked: companion.stop() }
    }
    component Problem: Label {
        Layout.fillWidth: true
        visible: companion.problem !== ""
        text: companion.problem
        color: theme.colors.red || panel.look.fg
        font.pixelSize: 13
        wrapMode: Text.Wrap
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
            Glyph { Layout.preferredWidth: 20; Layout.preferredHeight: 20; path: Icons.sparkle; color: panel.look.link }
            Label {
                Layout.fillWidth: true
                text: qsTr("Companion")
                color: panel.look.fg
                font.pixelSize: 18
                font.weight: Font.DemiBold
            }
            IconButton { glyph: Icons.close; tone: panel.look.fg; tip: qsTr("Close (Esc)"); onClicked: panel.close() }
        }
        Flow {
            Layout.fillWidth: true
            Layout.leftMargin: 22
            Layout.rightMargin: 22
            spacing: 6
            Chip { text: qsTr("Ask"); value: "ask" }
            Chip { text: qsTr("Previously…"); value: "recap" }
            Chip { text: qsTr("Who's who"); value: "whoswho" }
            Chip { visible: panel.selectionFeature; text: panel.featureName; value: "selection" }
        }

        // --- Ask ---------------------------------------------------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 22
            Layout.rightMargin: 22
            visible: panel.tab === "ask"
            spacing: 10

            Flickable {
                id: talk
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentHeight: talkColumn.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                onContentHeightChanged: if (companion.busy) contentY = Math.max(0, contentHeight - height)

                ColumnLayout {
                    id: talkColumn
                    width: talk.width
                    spacing: 12
                    Label {
                        Layout.fillWidth: true
                        visible: companion.conversation.length === 0
                        text: qsTr("Ask anything about the book so far: who someone is, why something happened, what a word meant then. Nothing past your page is read, so nothing is spoiled.")
                        color: panel.look.muted
                        font.pixelSize: 14
                        wrapMode: Text.Wrap
                        lineHeight: 1.3
                    }
                    Repeater {
                        model: companion.feature === "ask" ? companion.conversation : []
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: !modelData.fromUser
                            Layout.maximumWidth: talk.width
                            Layout.alignment: modelData.fromUser ? Qt.AlignRight : Qt.AlignLeft
                            implicitWidth: modelData.fromUser ? Math.min(talk.width * 0.85, said.implicitWidth + 24) : talk.width
                            implicitHeight: said.implicitHeight + (modelData.fromUser ? 16 : 0)
                            radius: Math.min(12, theme.radius + 4)
                            color: modelData.fromUser ? Ui.tint(panel.look.link, 0.16) : "transparent"
                            TextEdit {
                                id: said
                                x: modelData.fromUser ? 12 : 0
                                y: modelData.fromUser ? 8 : 0
                                width: parent.width - (modelData.fromUser ? 24 : 0)
                                text: modelData.text
                                readOnly: true
                                selectByMouse: true
                                wrapMode: Text.Wrap
                                textFormat: modelData.fromUser ? TextEdit.PlainText : TextEdit.MarkdownText
                                color: panel.look.fg
                                selectionColor: Ui.tint(panel.look.link, 0.35)
                                font.pixelSize: modelData.fromUser ? 14 : 15
                                font.family: modelData.fromUser ? Qt.application.font.family : "Literata"
                            }
                        }
                    }
                    Working {}
                    Problem {}
                }
            }
            // What asking sends, and where.
            Where {
                readonly property var estimate: panel.open && panel.tab === "ask" ? companion.askEstimate() : ({})
                text: !estimate.where ? ""
                    : estimate.local ? qsTr("%1 · the passages that matter, found in the book so far").arg(estimate.where)
                    : qsTr("%1 · the book so far, about %Ln words · about %2 a question", "", estimate.words)
                        .arg(estimate.where).arg(Number(estimate.cost).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.max(40, question.implicitHeight + 18)
                    radius: Math.min(10, theme.radius + 2)
                    color: Ui.tint(panel.look.fg, 0.06)
                    border.width: question.activeFocus ? 1.5 : 0
                    border.color: panel.look.link
                    TextEdit {
                        id: question
                        x: 12
                        width: parent.width - 24
                        anchors.verticalCenter: parent.verticalCenter
                        wrapMode: Text.Wrap
                        color: panel.look.fg
                        font.pixelSize: 14
                        selectByMouse: true
                        Keys.onReturnPressed: (event) => {
                            if (event.modifiers & Qt.ShiftModifier)
                                event.accepted = false
                            else
                                panel.send()
                        }
                        Keys.onEscapePressed: panel.close()
                        Text {
                            anchors.fill: parent
                            visible: question.text === ""
                            text: qsTr("Ask about the book…")
                            color: panel.look.muted
                            font: question.font
                        }
                    }
                }
                IconButton {
                    glyph: Icons.chevronRight
                    tone: panel.look.link
                    tip: qsTr("Ask (Enter)")
                    enabled: question.text.trim() !== "" && !companion.busy
                    onClicked: panel.send()
                }
            }
            TextButton {
                visible: companion.conversation.length > 0 && !companion.busy
                text: qsTr("New conversation")
                onClicked: companion.newConversation()
            }
        }

        // --- Previously…, and what was asked of a selection --------------------
        Flickable {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 22
            Layout.rightMargin: 22
            visible: panel.tab === "recap" || panel.tab === "selection"
            clip: true
            contentHeight: proseColumn.implicitHeight
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            ColumnLayout {
                id: proseColumn
                width: parent.width
                spacing: 12
                Label {
                    Layout.fillWidth: true
                    visible: panel.tab === "selection" && companion.subject !== ""
                    text: "“" + companion.subject + "”"
                    color: panel.look.muted
                    font.pixelSize: 14
                    font.italic: true
                    font.family: "Literata"
                    wrapMode: Text.Wrap
                    maximumLineCount: 6
                    elide: Text.ElideRight
                }
                Prose {
                    visible: panel.tab === "recap" ? companion.feature === "recap" : panel.selectionFeature
                    text: companion.answer
                }
                Working {}
                Problem {}
                Where {
                    visible: !companion.busy && companion.answer !== ""
                    text: companion.where
                }
                RowLayout {
                    visible: !companion.busy && companion.answer !== ""
                    spacing: 8
                    TextButton { text: qsTr("Copy"); onClicked: reader.copyText(companion.answer) }
                    TextButton {
                        visible: panel.tab === "recap"
                        text: qsTr("Again")
                        onClicked: companion.recap()
                    }
                }
            }
        }

        // --- Who's who -----------------------------------------------------
        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 22
            Layout.rightMargin: 22
            visible: panel.tab === "whoswho"
            clip: true
            spacing: 14
            model: companion.people
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            header: ColumnLayout {
                width: ListView.view.width
                spacing: 8
                Working {}
                Problem {}
                Where {
                    visible: companion.people.length > 0 && !companion.busy
                    text: qsTr("Up to “%1” · %2").arg(reader.chapterTitle).arg(companion.where)
                    bottomPadding: 6
                }
            }
            delegate: ColumnLayout {
                required property var modelData
                width: ListView.view.width
                spacing: 2
                Label {
                    Layout.fillWidth: true
                    text: modelData.name + (modelData.also ? "  ·  " + modelData.also : "")
                    color: panel.look.fg
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    text: modelData.who
                    color: panel.look.fg
                    font.pixelSize: 14
                    font.family: "Literata"
                    wrapMode: Text.Wrap
                    lineHeight: 1.2
                }
                Label {
                    Layout.fillWidth: true
                    visible: !!modelData.chapter
                    text: qsTr("First seen: %1").arg(modelData.chapter)
                    color: panel.look.muted
                    font.pixelSize: 12
                }
            }
        }
    }
}
