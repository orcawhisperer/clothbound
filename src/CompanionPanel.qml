import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

import "Icons.js" as Icons
import "Ui.js" as Ui

// The reading companion, beside the page: Ask about the book, Previously…,
// Who's who, Pictures, and what was asked of a selection (Define, Explain,
// Translate). Nothing past the reader's place is ever seen.
FocusScope {
    id: panel

    property bool open: false
    property var look
    // "ask", "recap", "whoswho", "pictures", or "selection" (define, explain, translate).
    property string tab: "ask"

    signal closed()
    signal jumpRequested(int chapter, int offset)
    signal saveAsNote(string quote, string note)

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
        if (picturePreview.opened)
            picturePreview.close()
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
    function placeLabel(chapter) {
        const ch = chapter >= 0 ? chapter : reader.chapter
        const title = reader.titleOfChapter(ch)
        if (title && title !== "")
            return qsTr("“%1”").arg(title)
        if (reader.format === "pdf") {
            const pageLbl = reader.pdfPageLabel(ch)
            return qsTr("Page %1").arg(pageLbl && pageLbl !== "" ? pageLbl : (ch + 1))
        }
        return qsTr("Chapter %1").arg(ch + 1)
    }
    function send() {
        if (question.text.trim() === "" || companion.busy)
            return
        talk.stickToBottom = true
        companion.ask(question.text)
        question.text = ""
        talk.scrollToBottom()
    }

    readonly property bool selectionFeature: ["define", "explain", "translate"].indexOf(companion.feature) >= 0
    readonly property string featureName: ({ define: qsTr("Define"), explain: qsTr("Explain"), translate: qsTr("Translate") })[companion.feature] || ""
    readonly property color errorColor: panel.look.dark ? (theme.colors.red || "#ff6b6b") : "#b42318"

    Keys.onEscapePressed: {
        if (picturePreview.opened)
            picturePreview.close()
        else
            close()
    }

    width: Math.min(440, parent.width - 16)
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
    component JumpChip: AbstractButton {
        id: jumpChip
        property string tipText: ""
        implicitHeight: 24
        implicitWidth: Math.min(260, jumpLabel.implicitWidth + 18)
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        ToolTip.visible: hovered && tipText !== ""
        ToolTip.delay: 450
        ToolTip.text: tipText
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        background: Rectangle {
            radius: 12
            color: jumpChip.hovered ? Ui.tint(panel.look.link, 0.22) : Ui.tint(panel.look.link, 0.11)
            border.width: 1
            border.color: Ui.tint(panel.look.link, jumpChip.hovered ? 0.55 : 0.28)
        }
        contentItem: Label {
            id: jumpLabel
            text: jumpChip.text
            color: panel.look.link
            font.pixelSize: 11
            font.weight: Font.Medium
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            leftPadding: 8
            rightPadding: 8
        }
    }
    component SmallAction: AbstractButton {
        id: smallAction
        implicitHeight: 26
        implicitWidth: smallActionLabel.implicitWidth + 16
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        opacity: enabled ? 1 : 0.42
        background: Rectangle {
            radius: Math.min(6, theme.radius)
            color: smallAction.pressed ? Ui.tint(panel.look.fg, 0.14)
                 : smallAction.hovered ? Ui.tint(panel.look.fg, 0.08) : Ui.tint(panel.look.fg, 0.04)
        }
        contentItem: Label {
            id: smallActionLabel
            text: smallAction.text
            color: panel.look.muted
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
        property bool active: true
        Layout.fillWidth: true
        visible: active && companion.busy
        spacing: 8
        BusyIndicator { running: parent.visible; implicitWidth: 20; implicitHeight: 20 }
        Label {
            Layout.fillWidth: true
            text: companion.status
            color: panel.look.muted
            font.pixelSize: 13
            elide: Text.ElideRight
        }
        TextButton { look: panel.look; text: qsTr("Stop"); onClicked: companion.stop() }
    }
    component Problem: Label {
        property bool active: true
        Layout.fillWidth: true
        visible: active && companion.problem !== ""
        text: companion.problem
        color: panel.errorColor
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
            Chip { text: qsTr("Pictures"); value: "pictures" }
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
                property bool stickToBottom: true
                property bool autoScrolling: false
                function scrollToBottom() {
                    autoScrolling = true
                    contentY = Math.max(0, contentHeight - height)
                    autoScrolling = false
                }
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                contentHeight: talkColumn.implicitHeight
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar {}
                // Respect manual upward wheel, drag, or scrollbar scrolling during streaming.
                onContentYChanged: {
                    if (!autoScrolling)
                        stickToBottom = (contentY + height >= contentHeight - 24)
                }
                onContentHeightChanged: {
                    if (companion.feature === "ask" && companion.busy && stickToBottom)
                        scrollToBottom()
                }

                ColumnLayout {
                    id: talkColumn
                    width: talk.width
                    spacing: 12
                    Label {
                        Layout.fillWidth: true
                        visible: companion.conversation.length === 0 && !(companion.feature === "ask" && companion.busy)
                        text: qsTr("Ask anything about the book so far: who someone is, why something happened, what a word meant then. Nothing past your page is read, so nothing is spoiled.")
                        color: panel.look.muted
                        font.pixelSize: 14
                        wrapMode: Text.Wrap
                        lineHeight: 1.3
                    }
                    Repeater {
                        model: companion.conversation
                        delegate: ColumnLayout {
                            id: turnBox
                            required property var modelData
                            required property int index
                            Layout.fillWidth: true
                            spacing: 6

                            Rectangle {
                                Layout.fillWidth: !turnBox.modelData.fromUser
                                Layout.maximumWidth: talk.width
                                Layout.alignment: turnBox.modelData.fromUser ? Qt.AlignRight : Qt.AlignLeft
                                implicitWidth: turnBox.modelData.fromUser ? Math.min(talk.width * 0.85, said.implicitWidth + 24) : talk.width
                                implicitHeight: said.implicitHeight + (turnBox.modelData.fromUser ? 16 : 0)
                                radius: Math.min(12, theme.radius + 4)
                                color: turnBox.modelData.fromUser ? Ui.tint(panel.look.link, 0.16) : "transparent"
                                TextEdit {
                                    id: said
                                    x: turnBox.modelData.fromUser ? 12 : 0
                                    y: turnBox.modelData.fromUser ? 8 : 0
                                    width: parent.width - (turnBox.modelData.fromUser ? 24 : 0)
                                    text: turnBox.modelData.fromUser ? turnBox.modelData.text : companion.sanitizeMarkdown(turnBox.modelData.text)
                                    readOnly: true
                                    selectByMouse: true
                                    wrapMode: Text.Wrap
                                    textFormat: turnBox.modelData.fromUser ? TextEdit.PlainText : TextEdit.MarkdownText
                                    color: panel.look.fg
                                    selectionColor: Ui.tint(panel.look.link, 0.35)
                                    font.pixelSize: turnBox.modelData.fromUser ? 14 : 15
                                    font.family: turnBox.modelData.fromUser ? Qt.application.font.family : "Literata"
                                }
                            }
                            Flow {
                                Layout.fillWidth: true
                                visible: !turnBox.modelData.fromUser && !!turnBox.modelData.sources && turnBox.modelData.sources.length > 0
                                spacing: 6
                                Repeater {
                                    model: (!turnBox.modelData.fromUser && turnBox.modelData.sources) ? turnBox.modelData.sources : []
                                    delegate: JumpChip {
                                        required property var modelData
                                        text: modelData.label || panel.placeLabel(modelData.chapter)
                                        tipText: modelData.excerpt || ""
                                        onClicked: panel.jumpRequested(modelData.chapter, modelData.offset || 0)
                                    }
                                }
                            }
                            RowLayout {
                                visible: !turnBox.modelData.fromUser && turnBox.modelData.text !== ""
                                spacing: 6
                                SmallAction {
                                    text: qsTr("Copy")
                                    onClicked: reader.copyText(turnBox.modelData.text)
                                }
                                SmallAction {
                                    text: qsTr("Save as note")
                                    onClicked: {
                                        const turns = companion.conversation
                                        const prev = turnBox.index > 0 ? turns[turnBox.index - 1] : null
                                        const quote = prev && prev.fromUser ? prev.text : panel.placeLabel(reader.chapter)
                                        panel.saveAsNote(quote, turnBox.modelData.text)
                                    }
                                }
                            }
                        }
                    }
                    Prose {
                        visible: companion.feature === "ask" && companion.busy && companion.answer !== ""
                        text: companion.sanitizeMarkdown(companion.answer)
                    }
                    Flow {
                        Layout.fillWidth: true
                        visible: companion.feature === "ask" && companion.busy && companion.currentSources.length > 0
                        spacing: 6
                        Repeater {
                            model: companion.currentSources
                            delegate: JumpChip {
                                required property var modelData
                                text: modelData.label || panel.placeLabel(modelData.chapter)
                                tipText: modelData.excerpt || ""
                                onClicked: panel.jumpRequested(modelData.chapter, modelData.offset || 0)
                            }
                        }
                    }
                    Working { active: panel.tab === "ask" && companion.feature === "ask" }
                    Problem { active: panel.tab === "ask" && companion.feature === "ask" }
                }
            }
            // What asking sends, and where.
            Where {
                readonly property var estimate: {
                    void(reader.chapter); void(reader.pageEnd); void(reader.progress)
                    void(ai.preset); void(ai.localUrl); void(ai.localTextModel); void(ai.hasKey)
                    return panel.open && panel.tab === "ask" ? companion.askEstimate() : ({})
                }
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
                    Layout.preferredHeight: Math.min(160, Math.max(40, question.implicitHeight + 18))
                    radius: Math.min(10, theme.radius + 2)
                    color: Ui.tint(panel.look.fg, 0.06)
                    border.width: question.activeFocus ? 1.5 : 0
                    border.color: panel.look.link
                    clip: true
                    Flickable {
                        id: questionScroll
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        anchors.topMargin: 9
                        anchors.bottomMargin: 9
                        contentWidth: width
                        contentHeight: question.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds
                        TextEdit {
                            id: question
                            width: parent.width
                            wrapMode: Text.Wrap
                            color: panel.look.fg
                            font.pixelSize: 14
                            selectByMouse: true
                            onCursorRectangleChanged: {
                                if (cursorRectangle.y < questionScroll.contentY)
                                    questionScroll.contentY = cursorRectangle.y
                                else if (cursorRectangle.y + cursorRectangle.height > questionScroll.contentY + questionScroll.height)
                                    questionScroll.contentY = cursorRectangle.y + cursorRectangle.height - questionScroll.height
                            }
                            Keys.onReturnPressed: (event) => {
                                if (event.modifiers & Qt.ShiftModifier)
                                    event.accepted = false
                                else
                                    panel.send()
                            }
                            Keys.onEnterPressed: (event) => {
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
                look: panel.look
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
                readonly property bool matchesTab: panel.tab === "recap" ? companion.feature === "recap" : panel.selectionFeature

                RowLayout {
                    Layout.fillWidth: true
                    visible: panel.tab === "selection" && companion.feature === "translate"
                    spacing: 8
                    Label {
                        text: qsTr("Translate into")
                        color: panel.look.muted
                        font.pixelSize: 13
                    }
                    ComboBox {
                        id: languagePicker
                        Layout.fillWidth: true
                        model: companion.targetLanguages
                        currentIndex: Math.max(0, model.indexOf(companion.targetLanguage))
                        onActivated: (index) => {
                            const chosen = model[index]
                            if (chosen && chosen !== companion.targetLanguage) {
                                companion.targetLanguage = chosen
                                companion.retranslate()
                            }
                        }
                    }
                }
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
                    visible: proseColumn.matchesTab
                    text: companion.sanitizeMarkdown(companion.answer)
                }
                Working { active: proseColumn.matchesTab && (panel.tab === "recap" || panel.tab === "selection") }
                Problem { active: proseColumn.matchesTab && (panel.tab === "recap" || panel.tab === "selection") }
                Where {
                    visible: proseColumn.matchesTab && !companion.busy && companion.answer !== ""
                    text: companion.where
                }
                Flow {
                    Layout.fillWidth: true
                    visible: proseColumn.matchesTab && !companion.busy && companion.answer !== ""
                    spacing: 8
                    TextButton { look: panel.look; text: qsTr("Copy"); onClicked: reader.copyText(companion.answer) }
                    TextButton {
                        look: panel.look
                        text: qsTr("Save as note")
                        onClicked: {
                            const quote = panel.tab === "recap"
                                ? qsTr("Previously… (%1)").arg(panel.placeLabel(reader.chapter))
                                : (companion.subject || panel.placeLabel(reader.chapter))
                            panel.saveAsNote(quote, companion.answer)
                        }
                    }
                    TextButton {
                        look: panel.look
                        visible: panel.tab === "recap"
                        text: qsTr("Again")
                        onClicked: companion.recap()
                    }
                }
            }
        }

        // --- Pictures --------------------------------------------------------
        ListView {
            id: pictureList
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 22
            Layout.rightMargin: 22
            visible: panel.tab === "pictures"
            clip: true
            spacing: 18
            model: illustrator.pictures
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}
            header: ColumnLayout {
                width: ListView.view.width
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    text: qsTr("An illustration of the page in view (or of words you select and right-click), in the style you choose.")
                    color: panel.look.muted
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    Repeater {
                        model: illustrator.styles
                        delegate: AbstractButton {
                            id: styleChip
                            required property var modelData
                            readonly property bool selected: illustrator.style === modelData.id
                            implicitHeight: 26
                            implicitWidth: styleChipLabel.implicitWidth + 18
                            hoverEnabled: true
                            focusPolicy: Qt.NoFocus
                            onClicked: illustrator.style = modelData.id
                            background: Rectangle {
                                radius: 13
                                color: styleChip.selected ? Ui.tint(panel.look.link, 0.2)
                                     : styleChip.hovered ? Ui.tint(panel.look.fg, 0.08) : Ui.tint(panel.look.fg, 0.04)
                                border.width: styleChip.selected ? 1 : 0
                                border.color: panel.look.link
                            }
                            contentItem: Label {
                                id: styleChipLabel
                                text: styleChip.modelData.label
                                color: styleChip.selected ? panel.look.fg : panel.look.muted
                                font.pixelSize: 12
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }
                RowLayout {
                    spacing: 10
                    TextButton {
                        look: panel.look
                        primary: true
                        enabled: illustrator.available && !illustrator.busy
                        text: qsTr("Illustrate this page")
                        onClicked: illustrator.illustrate("", reader.chapter, reader.pageEnd)
                    }
                    Label {
                        visible: illustrator.available
                        text: qsTr("about %1, with Gemini").arg(Number(illustrator.cost).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
                        color: panel.look.muted
                        font.pixelSize: 12
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: illustrator.busy
                    spacing: 8
                    BusyIndicator { running: parent.visible; implicitWidth: 20; implicitHeight: 20 }
                    Label {
                        Layout.fillWidth: true
                        text: illustrator.status
                        color: panel.look.muted
                        font.pixelSize: 13
                        elide: Text.ElideRight
                    }
                    TextButton { look: panel.look; text: qsTr("Stop"); onClicked: illustrator.stop() }
                }
                Label {
                    Layout.fillWidth: true
                    visible: illustrator.problem !== "" || !illustrator.available
                    text: illustrator.problem !== "" ? illustrator.problem
                        : qsTr("Pictures are made with Gemini: add your key in Intelligence (✦).")
                    color: illustrator.problem !== "" ? panel.errorColor : panel.look.muted
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }
                Item { implicitHeight: 4 }
            }
            delegate: ColumnLayout {
                id: picItem
                required property var modelData
                readonly property bool fileMissing: modelData.exists === false || pictureImage.status === Image.Error
                width: ListView.view.width
                spacing: 6
                Item {
                    visible: !picItem.fileMissing
                    Layout.preferredWidth: parent.width
                    Layout.maximumWidth: parent.width
                    Layout.preferredHeight: parent.width * (pictureImage.implicitHeight > 0 ? pictureImage.implicitHeight / Math.max(1, pictureImage.implicitWidth) : 4 / 3)
                    Image {
                        id: pictureImage
                        anchors.fill: parent
                        source: picItem.modelData.exists === false ? "" : picItem.modelData.url
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        sourceSize.width: 880
                        mipmap: true
                    }
                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: picturePreview.show(picItem.modelData)
                        ToolTip.visible: containsMouse
                        ToolTip.delay: 500
                        ToolTip.text: qsTr("Click to view full size")
                    }
                }
                Rectangle {
                    visible: picItem.fileMissing
                    Layout.fillWidth: true
                    Layout.preferredHeight: 108
                    radius: Math.min(8, theme.radius)
                    color: Ui.tint(panel.look.fg, 0.05)
                    border.color: panel.look.border
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 4
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Picture file is missing")
                            color: panel.look.muted
                            font.pixelSize: 13
                            font.weight: Font.Medium
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Use Regenerate to draw it again.")
                            color: panel.look.muted
                            font.pixelSize: 12
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: picItem.modelData.caption
                    color: panel.look.fg
                    font.pixelSize: 14
                    font.italic: true
                    font.family: "Literata"
                    wrapMode: Text.Wrap
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    TextButton {
                        look: panel.look
                        text: qsTr("Go to %1").arg(panel.placeLabel(picItem.modelData.chapter))
                        onClicked: panel.jumpRequested(picItem.modelData.chapter, picItem.modelData.offset || 0)
                    }
                    TextButton {
                        look: panel.look
                        enabled: illustrator.available && !illustrator.busy
                        text: qsTr("Regenerate")
                        onClicked: illustrator.regenerate(picItem.modelData.id)
                    }
                    TextButton {
                        look: panel.look
                        enabled: !picItem.fileMissing
                        text: qsTr("Save a copy")
                        onClicked: {
                            const path = illustrator.saveCopy(picItem.modelData.id)
                            if (path)
                                toast.show(qsTr("Saved in %1").arg(path.replace(/^.*\/(Pictures\/)/, "$1")))
                            else
                                toast.show(qsTr("Couldn't save a copy of this picture"))
                        }
                    }
                    TextButton { look: panel.look; text: qsTr("Remove"); onClicked: illustrator.remove(picItem.modelData.id) }
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
                Working { active: panel.tab === "whoswho" && companion.feature === "whoswho" }
                Problem { active: panel.tab === "whoswho" && companion.feature === "whoswho" }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: (companion.feature === "whoswho" || companion.peopleLoaded)
                             && !companion.busy && companion.problem === "" && companion.people.length === 0
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("No named characters found up to %1.").arg(panel.placeLabel(reader.chapter))
                        color: panel.look.muted
                        font.pixelSize: 14
                        wrapMode: Text.Wrap
                    }
                    TextButton {
                        look: panel.look
                        text: qsTr("Try again")
                        onClicked: companion.refreshWhosWho()
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: companion.people.length > 0 && !companion.busy
                    spacing: 8
                    Where {
                        readonly property string place: panel.placeLabel(companion.peopleChapter >= 0 ? companion.peopleChapter : reader.chapter)
                        text: companion.where ? qsTr("Up to %1 · %2").arg(place).arg(companion.where)
                                              : qsTr("Up to %1").arg(place)
                        bottomPadding: 6
                    }
                    TextButton {
                        look: panel.look
                        text: qsTr("Again")
                        onClicked: companion.refreshWhosWho()
                    }
                }
            }
            delegate: ColumnLayout {
                id: personItem
                required property var modelData
                width: ListView.view.width
                spacing: 4
                Label {
                    Layout.fillWidth: true
                    text: personItem.modelData.name + (personItem.modelData.also ? "  ·  " + personItem.modelData.also : "")
                    color: panel.look.fg
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    text: personItem.modelData.who
                    color: panel.look.fg
                    font.pixelSize: 14
                    font.family: "Literata"
                    wrapMode: Text.Wrap
                    lineHeight: 1.2
                }
                JumpChip {
                    visible: personItem.modelData.chapterIndex !== undefined && personItem.modelData.chapterIndex >= 0
                    text: qsTr("First seen: %1").arg(personItem.modelData.chapter || panel.placeLabel(personItem.modelData.chapterIndex))
                    tipText: qsTr("Jump to first appearance")
                    onClicked: panel.jumpRequested(personItem.modelData.chapterIndex, personItem.modelData.offset || 0)
                }
                Label {
                    Layout.fillWidth: true
                    visible: !(personItem.modelData.chapterIndex !== undefined && personItem.modelData.chapterIndex >= 0)
                             && !!personItem.modelData.chapter
                    text: qsTr("First seen: %1").arg(personItem.modelData.chapter || "")
                    color: panel.look.muted
                    font.pixelSize: 12
                }
            }
        }
    }

    // Inline full-size picture preview modal (renders over the whole window).
    Popup {
        id: picturePreview
        property var item: null
        function show(modelData) {
            item = modelData
            open()
        }
        parent: Overlay.overlay
        x: 0
        y: 0
        width: Overlay.overlay ? Overlay.overlay.width : 800
        height: Overlay.overlay ? Overlay.overlay.height : 600
        modal: true
        focus: true
        padding: 0
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        Connections {
            target: illustrator
            function onPicturesChanged() {
                if (picturePreview.opened && picturePreview.item) {
                    const id = picturePreview.item.id
                    if (!illustrator.pictures.some(p => p.id === id))
                        picturePreview.close()
                }
            }
        }

        background: Rectangle {
            color: Ui.tint("black", 0.82)
            TapHandler { onTapped: picturePreview.close() }
        }

        contentItem: Item {
            anchors.fill: parent
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 32
                spacing: 14

                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: picturePreview.item && picturePreview.item.styleLabel
                              ? qsTr("%1 · %2").arg(panel.placeLabel(picturePreview.item.chapter)).arg(picturePreview.item.styleLabel)
                              : picturePreview.item ? panel.placeLabel(picturePreview.item.chapter) : ""
                        color: "white"
                        opacity: 0.8
                        font.pixelSize: 13
                    }
                    IconButton {
                        glyph: Icons.close
                        tone: "white"
                        tip: qsTr("Close (Esc)")
                        onClicked: picturePreview.close()
                    }
                }
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Image {
                        id: fullPreviewImage
                        anchors.fill: parent
                        source: picturePreview.item ? picturePreview.item.url : ""
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        mipmap: true
                        smooth: true
                    }
                }
                Label {
                    Layout.fillWidth: true
                    Layout.maximumWidth: 720
                    Layout.alignment: Qt.AlignHCenter
                    visible: !!picturePreview.item && !!picturePreview.item.caption
                    text: picturePreview.item ? picturePreview.item.caption : ""
                    color: "white"
                    font.pixelSize: 15
                    font.italic: true
                    font.family: "Literata"
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    Layout.alignment: Qt.AlignHCenter
                    spacing: 10
                    TextButton {
                        look: panel.look
                        text: picturePreview.item ? qsTr("Go to %1").arg(panel.placeLabel(picturePreview.item.chapter)) : ""
                        visible: !!picturePreview.item
                        onClicked: {
                            const target = picturePreview.item
                            picturePreview.close()
                            if (target)
                                panel.jumpRequested(target.chapter, target.offset || 0)
                        }
                    }
                    TextButton {
                        look: panel.look
                        visible: !!picturePreview.item
                        enabled: illustrator.available && !illustrator.busy
                        text: qsTr("Regenerate")
                        onClicked: {
                            const target = picturePreview.item
                            picturePreview.close()
                            if (target)
                                illustrator.regenerate(target.id)
                        }
                    }
                    TextButton {
                        look: panel.look
                        visible: !!picturePreview.item
                        enabled: fullPreviewImage.status !== Image.Error
                        text: qsTr("Save a copy")
                        onClicked: {
                            if (!picturePreview.item)
                                return
                            const path = illustrator.saveCopy(picturePreview.item.id)
                            if (path)
                                toast.show(qsTr("Saved in %1").arg(path.replace(/^.*\/(Pictures\/)/, "$1")))
                            else
                                toast.show(qsTr("Couldn't save a copy of this picture"))
                        }
                    }
                }
            }
        }
    }
}
