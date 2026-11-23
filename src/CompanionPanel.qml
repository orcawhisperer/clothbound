import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts

import "Icons.js" as Icons
import "Ui.js" as Ui

// The reading companion, beside the page: Ask about the book, Previously…,
// Who's who, Pictures, and what was asked of a selection (Define, Explain,
// Translate). Nothing past the reader's place is ever seen.
FocusScope {
    id: panel

    property bool open: false
    // Kept for EpubReader binding; Companion wears app chrome (theme.drawer + theme.accent).
    property var look
    // "ask", "recap", "whoswho", "pictures", or "selection" (define, explain, translate).
    property string tab: "ask"

    signal closed()
    signal jumpRequested(int chapter, int offset)
    signal saveAsNote(string quote, string note)

    // App-chrome palette independent of Paper/Sepia reading themes.
    palette.window: theme.drawer
    palette.windowText: theme.ink1
    palette.base: theme.fill
    palette.alternateBase: theme.fillHover
    palette.text: theme.ink1
    palette.placeholderText: theme.ink4
    palette.button: theme.fill
    palette.buttonText: theme.ink1
    palette.mid: theme.ink3
    palette.midlight: theme.hairline
    palette.dark: theme.separator
    palette.light: theme.raised
    palette.shadow: theme.shadow
    palette.highlight: theme.accent
    palette.highlightedText: theme.accentOn
    palette.link: theme.accentInk
    palette.linkVisited: theme.accentSoft
    palette.toolTipBase: theme.overlay
    palette.toolTipText: theme.ink1

    Settings {
        id: companionChoice
        location: settingsUrl
        category: "companion"
        property bool notNow: false
    }

    function show(which) {
        if (which)
            tab = which
        open = true
        forceActiveFocus()
        if (!choosing && tab === "ask")
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
    function formatCents(dollars) {
        const value = Number(dollars) || 0
        if (value <= 0)
            return qsTr("less than 1¢")
        if (value < 0.01)
            return qsTr("less than 1¢")
        if (value < 1.0)
            return qsTr("%1¢").arg(Math.max(1, Math.round(value * 100)))
        return value.toLocaleCurrencyString(Qt.locale("en_US"), "$")
    }
    function personInitial(name) {
        const clean = String(name || "").trim().replace(/^(?:the|an?)\s+/i, "")
        return clean.length > 0 ? clean.charAt(0).toUpperCase() : "?"
    }
    function leftOffLabel() {
        const prev = Number(reader.previousOpenedAt) || 0
        if (prev > 0) {
            const elapsedSec = Math.max(0, Math.floor(Date.now() / 1000) - prev)
            const days = Math.floor(elapsedSec / 86400)
            if (days >= 2)
                return qsTr("Where you left off · %n days ago", "", days)
            if (days === 1)
                return qsTr("Where you left off · yesterday")
            const hours = Math.floor(elapsedSec / 3600)
            if (hours >= 2)
                return qsTr("Where you left off · %n hours ago", "", hours)
        }
        return qsTr("Where you left off")
    }
    function sendText(raw) {
        const asked = String(raw || "").trim()
        if (asked === "" || companion.busy)
            return
        if (panel.tab !== "ask")
            panel.tab = "ask"
        talk.stickToBottom = true
        companion.ask(asked)
        talk.scrollToBottom()
    }
    function send() {
        if (question.text.trim() === "" || companion.busy)
            return
        const asked = question.text
        question.text = ""
        sendText(asked)
    }

    readonly property bool selectionFeature: ["define", "explain", "translate"].indexOf(companion.feature) >= 0
    // The routing feature behind the tab shown, for where its answers are made.
    readonly property string routeFeature: tab === "selection" ? companion.feature : tab
    // With no key and no model, the panel first asks where the companion
    // should run, unless the reader said "Not now".
    readonly property bool choosing: !ai.hasKey && !ai.hasLocal && !companionChoice.notNow
    readonly property string featureName: ({ define: qsTr("Define"), explain: qsTr("Explain"), translate: qsTr("Translate") })[companion.feature] || ""
    readonly property var tryPrompts: {
        void(reader.bookId); void(reader.chapter); void(reader.chapterTitle)
        void(companion.conversation); void(companion.people); void(companion.peopleLoaded)
        return panel.open ? companion.suggestions() : []
    }
    readonly property var askEstimateInfo: {
        void(reader.chapter); void(reader.pageEnd); void(reader.progress)
        void(ai.preset); void(ai.localUrl); void(ai.localTextModel); void(ai.hasKey)
        return panel.open ? companion.askEstimate() : ({})
    }

    Keys.onEscapePressed: {
        if (picturePreview.opened)
            picturePreview.close()
        else
            close()
    }

    width: Math.min(384, parent.width - 16)
    x: theme.reducedMotion ? parent.width - width : (open ? parent.width - width : parent.width + 24)
    Behavior on x {
        enabled: !theme.reducedMotion
        NumberAnimation { duration: 350; easing.type: Easing.OutQuart }
    }
    opacity: theme.reducedMotion ? (open ? 1 : 0) : 1
    Behavior on opacity {
        enabled: theme.reducedMotion
        NumberAnimation { duration: 150 }
    }
    visible: theme.reducedMotion ? opacity > 0 : x < parent.width + 20

    // Soft shadow toward the page.
    Rectangle {
        anchors.right: parent.left
        width: 18
        height: parent.height
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: "transparent" }
            GradientStop { position: 1; color: Ui.tint("#000000", theme.light ? 0.08 : 0.30) }
        }
    }
    Rectangle {
        anchors.fill: parent
        color: theme.drawer
        Rectangle {
            width: 1
            height: parent.height
            color: panel.look && !panel.look.dark && !theme.light ? Ui.tint("#000000", 0.20) : theme.hairline
        }
    }
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    component JumpChip: AbstractButton {
        id: jumpChip
        property string tipText: ""
        implicitHeight: 22
        implicitWidth: Math.min(320, jumpLabel.implicitWidth + 16)
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        ToolTip.visible: hovered && tipText !== ""
        ToolTip.delay: 450
        ToolTip.text: tipText
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        background: Rectangle {
            radius: Ui.radius(10, theme.radius)
            color: jumpChip.hovered ? Ui.tint(theme.accent, 0.28) : theme.accentSoft
            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                visible: jumpChip.visualFocus
                radius: Ui.radius(13, theme.radius)
                color: "transparent"
                border.width: 2
                border.color: theme.accent
            }
        }
        contentItem: Label {
            id: jumpLabel
            text: jumpChip.text
            color: theme.accentInk
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
        implicitHeight: 22
        implicitWidth: smallActionLabel.implicitWidth + 12
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        opacity: enabled ? 1 : 0.42
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        background: Rectangle {
            radius: Ui.radius(5, theme.radius)
            color: smallAction.pressed ? theme.fillPressed
                 : smallAction.hovered ? theme.fillHover : "transparent"
            Rectangle {
                anchors.fill: parent
                anchors.margins: -2
                visible: smallAction.visualFocus
                radius: Ui.radius(7, theme.radius)
                color: "transparent"
                border.width: 2
                border.color: theme.accent
            }
        }
        contentItem: Label {
            id: smallActionLabel
            text: smallAction.text
            color: smallAction.hovered ? theme.ink1 : theme.ink4
            font.pixelSize: 11
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }
    // Words to read, selectable for copying (Literata for Previously… & selection prose).
    component Prose: TextEdit {
        Layout.fillWidth: true
        readOnly: true
        selectByMouse: true
        wrapMode: Text.Wrap
        textFormat: TextEdit.MarkdownText
        color: theme.ink2
        selectionColor: theme.accentSoft
        font.pixelSize: 15
        font.family: "Literata"
    }
    // Provenance line with computer/cloud glyph under each result.
    component ProvenanceRow: RowLayout {
        id: prov
        property string text: ""
        property bool local: false
        Layout.fillWidth: true
        visible: text !== ""
        spacing: 6
        Glyph {
            Layout.preferredWidth: 12
            Layout.preferredHeight: 12
            stroke: 2.0
            path: prov.local ? Icons.computer : Icons.cloud
            color: theme.ink4
        }
        Label {
            Layout.fillWidth: true
            text: prov.text
            color: theme.ink4
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }
    }
    component Working: RowLayout {
        property bool active: true
        Layout.fillWidth: true
        visible: active && companion.busy
        spacing: 8
        BusyIndicator { running: parent.visible; implicitWidth: 18; implicitHeight: 18 }
        Label {
            Layout.fillWidth: true
            text: companion.status
            color: theme.ink3
            font.pixelSize: 12
            elide: Text.ElideRight
        }
        TextButton { size: "compact"; text: qsTr("Stop"); onClicked: companion.stop() }
    }
    component Problem: Label {
        property bool active: true
        Layout.fillWidth: true
        visible: active && companion.problem !== ""
        text: companion.problem
        color: theme.danger
        font.pixelSize: 12
        wrapMode: Text.Wrap
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // --- Header block (title, segmented tabs, spoiler shield) ----------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: 14
            Layout.bottomMargin: 12
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            spacing: 12

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Glyph { Layout.preferredWidth: 16; Layout.preferredHeight: 16; stroke: 1.8; path: Icons.sparkle; color: theme.accent }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Companion")
                    color: theme.ink1
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                }
                IconButton {
                    visible: !panel.choosing && panel.tab === "ask" && companion.conversation.length > 0 && !companion.busy
                    implicitWidth: 28
                    implicitHeight: 28
                    glyph: Icons.plus
                    tone: theme.ink3
                    tip: qsTr("New conversation")
                    onClicked: companion.newConversation()
                }
                IconButton {
                    implicitWidth: 28
                    implicitHeight: 28
                    glyph: Icons.close
                    tone: theme.ink3
                    tip: qsTr("Close companion (C)")
                    onClicked: panel.close()
                }
            }

            Segmented {
                visible: !panel.choosing
                Layout.fillWidth: true
                Layout.preferredHeight: 30
                compact: true
                model: {
                    const tabs = [{ value: "ask", label: qsTr("Ask") }, { value: "recap", label: qsTr("Previously") },
                                  { value: "whoswho", label: qsTr("Who's who") }, { value: "pictures", label: qsTr("Pictures") }]
                    if (panel.selectionFeature)
                        tabs.push({ value: "selection", label: panel.featureName })
                    return tabs
                }
                current: panel.tab
                onActivated: (value) => panel.showTab(value)
            }

            // The spoiler shield: what the companion may know of the book.
            RowLayout {
                visible: !panel.choosing
                Layout.fillWidth: true
                spacing: 8
                Glyph { Layout.preferredWidth: 14; Layout.preferredHeight: 14; stroke: 1.9; path: Icons.shieldCheck; color: theme.positive }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Knows the book up to your page (%1%). Never ahead.").arg(Math.round(reader.progress * 100))
                    color: theme.ink3
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }

        // --- First time (J5): where the companion runs ---------------------
        Flickable {
            visible: panel.choosing
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentHeight: chooserItem.implicitHeight + 36
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            CompanionChooser {
                id: chooserItem
                x: 16
                y: 20
                width: parent.width - 32
                look: panel.look
                onNotNow: { companionChoice.notNow = true; panel.close() }
            }
        }

        // --- Ask -----------------------------------------------------------
        Flickable {
            id: talk
            property bool stickToBottom: true
            property bool autoScrolling: false
            function scrollToBottom() {
                autoScrolling = true
                contentY = Math.max(0, contentHeight - height)
                autoScrolling = false
            }
            visible: !panel.choosing && panel.tab === "ask"
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentHeight: Math.max(height, talkColumn.implicitHeight + 36)
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
                x: 16
                y: 18
                width: talk.width - 32
                height: Math.max(implicitHeight, talk.height - 36)
                spacing: 16

                // Re-entry card if the reader chose "Not now" earlier and still has no model.
                Rectangle {
                    Layout.fillWidth: true
                    visible: companionChoice.notNow && !ai.hasKey && !ai.hasLocal && companion.conversation.length === 0
                    implicitHeight: setupCardCol.implicitHeight + 24
                    radius: Ui.radius(10, theme.radius)
                    color: Ui.tint(theme.ink1, 0.04)
                    border.width: 1
                    border.color: theme.hairline
                    ColumnLayout {
                        id: setupCardCol
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Companion isn't set up yet")
                            color: theme.ink1
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Choose a model on this computer or connect Gemini with your key to ask about the book.")
                            color: theme.ink3
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                            lineHeight: 1.4
                        }
                        TextButton {
                            size: "compact"
                            primary: true
                            text: qsTr("Choose where it runs")
                            onClicked: companionChoice.notNow = false
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: !(companionChoice.notNow && !ai.hasKey && !ai.hasLocal)
                             && companion.conversation.length === 0 && !(companion.feature === "ask" && companion.busy)
                    text: qsTr("Ask anything about the book so far: who someone is, why something happened, or what a phrase means. Nothing past your page is read.")
                    color: theme.ink3
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    lineHeight: 1.5
                }

                Repeater {
                    model: companion.conversation
                    delegate: ColumnLayout {
                        id: turnBox
                        required property var modelData
                        required property int index
                        Layout.fillWidth: true
                        spacing: 10

                        // User bubble (neutral 8%, right-aligned, max 280px) or Assistant reply (Inter 13px).
                        Rectangle {
                            Layout.fillWidth: !turnBox.modelData.fromUser
                            Layout.maximumWidth: turnBox.modelData.fromUser ? Math.min(280, talkColumn.width * 0.88) : talkColumn.width
                            Layout.alignment: turnBox.modelData.fromUser ? Qt.AlignRight : Qt.AlignLeft
                            implicitWidth: turnBox.modelData.fromUser ? Math.min(280, said.implicitWidth + 24) : talkColumn.width
                            implicitHeight: said.implicitHeight + (turnBox.modelData.fromUser ? 18 : 0)
                            radius: Ui.radius(12, theme.radius)
                            color: turnBox.modelData.fromUser ? Ui.tint(theme.ink1, 0.08) : "transparent"

                            TextEdit {
                                id: said
                                x: turnBox.modelData.fromUser ? 12 : 0
                                y: turnBox.modelData.fromUser ? 9 : 0
                                width: parent.width - (turnBox.modelData.fromUser ? 24 : 0)
                                text: turnBox.modelData.fromUser ? turnBox.modelData.text : companion.sanitizeMarkdown(turnBox.modelData.text)
                                readOnly: true
                                selectByMouse: true
                                wrapMode: Text.Wrap
                                textFormat: turnBox.modelData.fromUser ? TextEdit.PlainText : TextEdit.MarkdownText
                                color: turnBox.modelData.fromUser ? theme.ink1 : theme.ink2
                                selectionColor: theme.accentSoft
                                font.pixelSize: 13
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

                        ColumnLayout {
                            Layout.fillWidth: true
                            visible: !turnBox.modelData.fromUser && turnBox.modelData.text !== ""
                            spacing: 4

                            ProvenanceRow {
                                local: !!turnBox.modelData.local
                                text: turnBox.modelData.provenance || ""
                            }

                            RowLayout {
                                spacing: 4
                                SmallAction {
                                    text: qsTr("Copy")
                                    onClicked: {
                                        reader.copyText(turnBox.modelData.text)
                                        toast.show(qsTr("Copied"))
                                    }
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
                }

                // Live streaming reply.
                TextEdit {
                    Layout.fillWidth: true
                    visible: companion.feature === "ask" && companion.busy && companion.answer !== ""
                    readOnly: true
                    selectByMouse: true
                    wrapMode: Text.Wrap
                    textFormat: TextEdit.MarkdownText
                    text: companion.sanitizeMarkdown(companion.answer)
                    color: theme.ink2
                    selectionColor: theme.accentSoft
                    font.pixelSize: 13
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

                Item { Layout.fillHeight: true }

                // Suggested starter / follow-up prompts ("Try").
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: !companion.busy && (ai.hasKey || ai.hasLocal) && panel.tryPrompts.length > 0
                    spacing: 6
                    Label {
                        text: qsTr("Try")
                        color: theme.ink4
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                    }
                    Repeater {
                        model: panel.tryPrompts
                        delegate: AbstractButton {
                            id: tryBtn
                            required property string modelData
                            Layout.fillWidth: true
                            implicitHeight: tryLabel.implicitHeight + 16
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            onClicked: panel.sendText(modelData)
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            background: Rectangle {
                                radius: Ui.radius(7, theme.radius)
                                color: tryBtn.pressed ? theme.fillPressed
                                     : tryBtn.hovered ? theme.fill : "transparent"
                                border.width: 1
                                border.color: theme.hairline
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -3
                                    visible: tryBtn.visualFocus
                                    radius: Ui.radius(10, theme.radius)
                                    color: "transparent"
                                    border.width: 2
                                    border.color: theme.accent
                                }
                            }
                            contentItem: Label {
                                id: tryLabel
                                text: tryBtn.modelData
                                color: tryBtn.hovered ? theme.ink1 : theme.ink2
                                font.pixelSize: 13
                                wrapMode: Text.Wrap
                                leftPadding: 10
                                rightPadding: 10
                                topPadding: 8
                                bottomPadding: 8
                            }
                        }
                    }
                }
            }
        }

        // --- Previously…, and what was asked of a selection ----------------
        Flickable {
            visible: !panel.choosing && (panel.tab === "recap" || panel.tab === "selection")
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            contentHeight: proseColumn.implicitHeight + 36
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            ColumnLayout {
                id: proseColumn
                x: 16
                y: 18
                width: parent.width - 32
                spacing: 14
                readonly property bool matchesTab: panel.tab === "recap" ? companion.feature === "recap" : panel.selectionFeature

                // Recap eyebrow: "WHERE YOU LEFT OFF · 3 DAYS AGO".
                Label {
                    Layout.fillWidth: true
                    visible: panel.tab === "recap"
                    text: panel.leftOffLabel()
                    color: theme.ink4
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.45
                    font.capitalization: Font.AllUppercase
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: panel.tab === "selection" && companion.feature === "translate"
                    spacing: 8
                    Label {
                        text: qsTr("Translate into")
                        color: theme.ink3
                        font.pixelSize: 12
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
                    color: theme.ink3
                    font.pixelSize: 14
                    font.italic: true
                    font.family: "Literata"
                    wrapMode: Text.Wrap
                    maximumLineCount: 6
                    elide: Text.ElideRight
                }

                Prose {
                    visible: proseColumn.matchesTab && companion.answer !== ""
                    text: companion.sanitizeMarkdown(companion.answer)
                }
                Working { active: proseColumn.matchesTab && (panel.tab === "recap" || panel.tab === "selection") }
                Problem { active: proseColumn.matchesTab && (panel.tab === "recap" || panel.tab === "selection") }

                ProvenanceRow {
                    visible: proseColumn.matchesTab && !companion.busy && companion.answer !== "" && companion.where !== ""
                    local: companion.whereLocal
                    text: companion.where
                }

                Flow {
                    Layout.fillWidth: true
                    visible: proseColumn.matchesTab && !companion.busy && companion.answer !== ""
                    spacing: 8
                    TextButton {
                        size: "compact"
                        text: qsTr("Copy")
                        onClicked: {
                            reader.copyText(companion.answer)
                            toast.show(qsTr("Copied"))
                        }
                    }
                    TextButton {
                        size: "compact"
                        text: qsTr("Save as note")
                        onClicked: {
                            const quote = panel.tab === "recap"
                                ? qsTr("Previously… (%1)").arg(panel.placeLabel(reader.chapter))
                                : (companion.subject || panel.placeLabel(reader.chapter))
                            panel.saveAsNote(quote, companion.answer)
                        }
                    }
                    TextButton {
                        size: "compact"
                        visible: panel.tab === "recap"
                        text: qsTr("Again")
                        onClicked: companion.recap()
                    }
                }
            }
        }

        // --- Pictures ------------------------------------------------------
        ListView {
            id: pictureList
            visible: !panel.choosing && panel.tab === "pictures"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            topMargin: 16
            bottomMargin: 16
            clip: true
            spacing: 16
            model: illustrator.pictures
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            header: ColumnLayout {
                width: ListView.view.width
                spacing: 12

                // Style chips (preserved cleanly above hero/gallery).
                Flow {
                    Layout.fillWidth: true
                    spacing: 6
                    Repeater {
                        model: illustrator.styles
                        delegate: AbstractButton {
                            id: styleChip
                            required property var modelData
                            readonly property bool selected: illustrator.style === modelData.id
                            implicitHeight: 24
                            implicitWidth: styleChipLabel.implicitWidth + 16
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            onClicked: illustrator.style = modelData.id
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            background: Rectangle {
                                radius: Ui.radius(12, theme.radius)
                                color: styleChip.selected ? theme.accentSoft
                                     : styleChip.hovered ? theme.fillHover : theme.fill
                                border.width: styleChip.selected ? 1 : 0
                                border.color: theme.accent
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -3
                                    visible: styleChip.visualFocus
                                    radius: Ui.radius(15, theme.radius)
                                    color: "transparent"
                                    border.width: 2
                                    border.color: theme.accent
                                }
                            }
                            contentItem: Label {
                                id: styleChipLabel
                                text: styleChip.modelData.label
                                color: styleChip.selected ? theme.accentInk : theme.ink3
                                font.pixelSize: 11
                                font.weight: styleChip.selected ? Font.Medium : Font.Normal
                                horizontalAlignment: Text.AlignHCenter
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }

                // 4:3 dashed empty state card when no illustrations exist for this book yet.
                Item {
                    visible: illustrator.pictures.length === 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.round(width * 0.75)

                    Canvas {
                        id: dashedFrame
                        anchors.fill: parent
                        readonly property real corner: Ui.radius(12, theme.radius)
                        readonly property color strokeColor: Ui.tint(theme.ink1, 0.16)
                        onWidthChanged: requestPaint()
                        onHeightChanged: requestPaint()
                        onCornerChanged: requestPaint()
                        onStrokeColorChanged: requestPaint()
                        onPaint: {
                            const g = getContext("2d")
                            g.reset()
                            const w = width - 1
                            const h = height - 1
                            const r = Math.min(corner, w / 2, h / 2)
                            g.strokeStyle = strokeColor
                            g.lineWidth = 1
                            g.setLineDash([5, 4])
                            g.beginPath()
                            g.moveTo(0.5 + r, 0.5)
                            g.lineTo(0.5 + w - r, 0.5)
                            if (r > 0) g.arcTo(0.5 + w, 0.5, 0.5 + w, 0.5 + r, r)
                            g.lineTo(0.5 + w, 0.5 + h - r)
                            if (r > 0) g.arcTo(0.5 + w, 0.5 + h, 0.5 + w - r, 0.5 + h, r)
                            g.lineTo(0.5 + r, 0.5 + h)
                            if (r > 0) g.arcTo(0.5, 0.5 + h, 0.5, 0.5 + h - r, r)
                            g.lineTo(0.5, 0.5 + r)
                            if (r > 0) g.arcTo(0.5, 0.5, 0.5 + r, 0.5, r)
                            g.closePath()
                            g.stroke()
                        }
                    }

                    ColumnLayout {
                        anchors.centerIn: parent
                        width: parent.width - 40
                        spacing: 8
                        Glyph {
                            Layout.alignment: Qt.AlignHCenter
                            Layout.preferredWidth: 28
                            Layout.preferredHeight: 28
                            stroke: 1.5
                            path: Icons.picture
                            color: theme.ink3
                        }
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: qsTr("Picture this scene")
                            color: theme.ink2
                            font.pixelSize: 13
                            font.weight: Font.Medium
                        }
                        Label {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            text: qsTr("Drawn from the page you’re on, or from words you select and right-click.")
                            color: theme.ink3
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                            lineHeight: 1.45
                        }
                    }
                }

                // Full-width 34px accent CTA with embedded cost.
                TextButton {
                    Layout.fillWidth: true
                    primary: true
                    size: "large"
                    enabled: illustrator.available && !illustrator.busy
                    text: illustrator.available && illustrator.cost > 0
                          ? qsTr("Illustrate · about %1").arg(panel.formatCents(illustrator.cost))
                          : qsTr("Illustrate this page")
                    onClicked: illustrator.illustrate("", reader.chapter, reader.pageEnd)
                }

                RowLayout {
                    Layout.fillWidth: true
                    visible: illustrator.busy
                    spacing: 8
                    BusyIndicator { running: parent.visible; implicitWidth: 18; implicitHeight: 18 }
                    Label {
                        Layout.fillWidth: true
                        text: illustrator.status
                        color: theme.ink3
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    TextButton { size: "compact"; text: qsTr("Stop"); onClicked: illustrator.stop() }
                }

                Label {
                    Layout.fillWidth: true
                    visible: illustrator.problem !== "" || !illustrator.available
                    text: illustrator.problem !== "" ? illustrator.problem
                        : ai.hasKey ? qsTr("Pictures need Gemini: switch to Hybrid or Cloud in Intelligence settings.")
                                    : qsTr("Pictures are made with Gemini: add your key in Intelligence settings.")
                    color: illustrator.problem !== "" ? theme.danger : theme.ink3
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }

                Item { Layout.preferredHeight: illustrator.pictures.length > 0 ? 4 : 0 }
            }

            delegate: ColumnLayout {
                id: picItem
                required property var modelData
                readonly property bool fileMissing: modelData.exists === false || pictureImage.status === Image.Error
                readonly property int picRadius: Ui.radius(12, theme.radius)
                width: ListView.view.width
                spacing: 10

                Item {
                    id: picFrame
                    visible: !picItem.fileMissing
                    Layout.fillWidth: true
                    Layout.preferredHeight: Math.round(width * 0.75)

                    Item {
                        anchors.fill: parent
                        layer.enabled: picItem.picRadius > 0
                        layer.effect: MultiEffect {
                            maskEnabled: true
                            maskSource: picMask
                            maskThresholdMin: 0.5
                            maskSpreadAtMin: 1.0
                        }
                        Image {
                            id: pictureImage
                            anchors.fill: parent
                            source: picItem.modelData.exists === false ? "" : picItem.modelData.url
                            fillMode: Image.PreserveAspectCrop
                            asynchronous: true
                            sourceSize.width: 768
                            mipmap: true
                            smooth: true
                        }
                    }
                    Item {
                        id: picMask
                        anchors.fill: parent
                        visible: false
                        layer.enabled: true
                        Rectangle {
                            anchors.fill: parent
                            radius: picItem.picRadius
                        }
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
                    Layout.preferredHeight: 112
                    radius: picItem.picRadius
                    color: theme.fill
                    border.width: 1
                    border.color: theme.hairline
                    ColumnLayout {
                        anchors.centerIn: parent
                        spacing: 4
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Picture file is missing")
                            color: theme.ink3
                            font.pixelSize: 13
                            font.weight: Font.Medium
                        }
                        Label {
                            Layout.alignment: Qt.AlignHCenter
                            text: qsTr("Use Another take to draw it again.")
                            color: theme.ink4
                            font.pixelSize: 12
                        }
                    }
                }

                Label {
                    Layout.fillWidth: true
                    visible: !!picItem.modelData.caption
                    text: picItem.modelData.caption
                    color: theme.quote
                    font.pixelSize: 13
                    font.italic: true
                    font.family: "Literata"
                    lineHeight: 1.5
                    wrapMode: Text.Wrap
                }

                ProvenanceRow {
                    local: false
                    text: {
                        const base = picItem.modelData.where || illustrator.where
                        const place = panel.placeLabel(picItem.modelData.chapter)
                        return picItem.modelData.style && picItem.modelData.style !== "auto" && picItem.modelData.styleLabel
                             ? qsTr("%1 · %2 · %3").arg(base).arg(place).arg(picItem.modelData.styleLabel)
                             : qsTr("%1 · %2").arg(base).arg(place)
                    }
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 8
                    TextButton {
                        size: "normal"
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
                    TextButton {
                        size: "normal"
                        enabled: illustrator.available && !illustrator.busy
                        text: qsTr("Another take")
                        onClicked: illustrator.regenerate(picItem.modelData.id)
                    }
                    TextButton {
                        size: "compact"
                        ghost: true
                        text: qsTr("Go to %1").arg(panel.placeLabel(picItem.modelData.chapter))
                        onClicked: panel.jumpRequested(picItem.modelData.chapter, picItem.modelData.offset || 0)
                    }
                    TextButton {
                        size: "compact"
                        ghost: true
                        text: qsTr("Remove")
                        onClicked: illustrator.remove(picItem.modelData.id)
                    }
                }
            }
        }

        // --- Who's who -----------------------------------------------------
        ListView {
            visible: !panel.choosing && panel.tab === "whoswho"
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.leftMargin: 8
            Layout.rightMargin: 8
            topMargin: 10
            bottomMargin: 10
            clip: true
            spacing: 0
            model: companion.people
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            header: ColumnLayout {
                width: ListView.view.width
                spacing: 8
                Working {
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    active: panel.tab === "whoswho" && companion.feature === "whoswho"
                }
                Problem {
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    active: panel.tab === "whoswho" && companion.feature === "whoswho"
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    Layout.topMargin: 6
                    visible: (companion.feature === "whoswho" || companion.peopleLoaded)
                             && !companion.busy && companion.problem === "" && companion.people.length === 0
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("No named characters found up to %1.").arg(panel.placeLabel(reader.chapter))
                        color: theme.ink3
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }
                    TextButton {
                        size: "compact"
                        text: qsTr("Try again")
                        onClicked: companion.refreshWhosWho()
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    Layout.bottomMargin: 4
                    visible: companion.people.length > 0 && !companion.busy
                    spacing: 8
                    ProvenanceRow {
                        local: companion.whereLocal
                        readonly property string place: panel.placeLabel(companion.peopleChapter >= 0 ? companion.peopleChapter : reader.chapter)
                        text: companion.where ? qsTr("Up to %1 · %2").arg(place).arg(companion.where)
                                              : qsTr("Up to %1").arg(place)
                    }
                    TextButton {
                        size: "compact"
                        ghost: true
                        text: qsTr("Again")
                        onClicked: companion.refreshWhosWho()
                    }
                }
            }

            delegate: ColumnLayout {
                id: personItem
                required property var modelData
                required property int index
                width: ListView.view.width
                spacing: 0

                RowLayout {
                    Layout.fillWidth: true
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    Layout.topMargin: 10
                    Layout.bottomMargin: 10
                    spacing: 12

                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        radius: 16
                        color: Ui.tint(theme.ink1, 0.08)
                        Label {
                            anchors.centerIn: parent
                            text: panel.personInitial(personItem.modelData.name)
                            color: theme.ink2
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 3
                        Label {
                            Layout.fillWidth: true
                            text: personItem.modelData.name + (personItem.modelData.also ? " · " + personItem.modelData.also : "")
                            color: theme.ink1
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            wrapMode: Text.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            text: personItem.modelData.who
                            color: theme.ink3
                            font.pixelSize: 13
                            wrapMode: Text.Wrap
                            lineHeight: 1.45
                        }
                        JumpChip {
                            Layout.topMargin: 3
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
                            color: theme.ink4
                            font.pixelSize: 11
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.leftMargin: 8
                    Layout.rightMargin: 8
                    Layout.preferredHeight: 1
                    visible: personItem.index < companion.people.length - 1
                    color: Ui.tint(theme.ink1, 0.05)
                }
            }
        }

        // --- Shared bottom composer (always reachable across tabs) ----------
        Rectangle {
            visible: !panel.choosing
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: theme.hairline
        }
        ColumnLayout {
            visible: !panel.choosing
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.topMargin: 12
            Layout.bottomMargin: 16
            spacing: 6

            // Quiet pre-send cost transparency line while composing.
            ProvenanceRow {
                visible: question.text.trim() !== "" && !!panel.askEstimateInfo.where
                local: !!panel.askEstimateInfo.local
                text: !panel.askEstimateInfo.where ? ""
                    : panel.askEstimateInfo.local
                      ? qsTr("%1 · passages found in the book so far").arg(panel.askEstimateInfo.where)
                      : qsTr("%1 · about %Ln words · %2", "", panel.askEstimateInfo.words)
                            .arg(panel.askEstimateInfo.where).arg(panel.formatCents(panel.askEstimateInfo.cost))
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.min(140, Math.max(40, question.implicitHeight + 20))
                radius: Ui.radius(12, theme.radius)
                color: Ui.tint(theme.ink1, 0.06)
                border.width: 1
                border.color: question.activeFocus ? theme.accent : theme.hairline

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 6
                    spacing: 8

                    Flickable {
                        id: questionScroll
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.topMargin: 10
                        Layout.bottomMargin: 10
                        clip: true
                        contentWidth: width
                        contentHeight: question.implicitHeight
                        boundsBehavior: Flickable.StopAtBounds

                        TextEdit {
                            id: question
                            width: parent.width
                            wrapMode: Text.Wrap
                            color: theme.ink1
                            selectionColor: theme.accentSoft
                            font.pixelSize: 13
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
                                text: qsTr("Ask about the book so far…")
                                color: theme.ink4
                                font: question.font
                                elide: Text.ElideRight
                            }
                        }
                    }

                    AbstractButton {
                        id: sendBtn
                        Layout.alignment: Qt.AlignBottom
                        Layout.bottomMargin: 6
                        implicitWidth: 28
                        implicitHeight: 28
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        enabled: question.text.trim() !== "" && !companion.busy
                        opacity: enabled ? 1 : 0.35
                        Accessible.name: qsTr("Ask")
                        onClicked: panel.send()
                        HoverHandler { enabled: sendBtn.enabled; cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: qsTr("Ask (Enter)")
                        background: Rectangle {
                            radius: Ui.radius(7, theme.radius)
                            color: sendBtn.enabled && sendBtn.pressed ? Qt.darker(theme.accent, 1.12)
                                 : sendBtn.enabled && sendBtn.hovered ? Qt.lighter(theme.accent, 1.06)
                                 : theme.accent
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -3
                                visible: sendBtn.visualFocus
                                radius: Ui.radius(10, theme.radius)
                                color: "transparent"
                                border.width: 2
                                border.color: theme.accent
                            }
                        }
                        contentItem: Item {
                            Glyph {
                                anchors.centerIn: parent
                                width: 14
                                height: 14
                                stroke: 2.4
                                path: Icons.arrowUp
                                color: theme.accentOn
                            }
                        }
                    }
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
            color: Ui.tint("#000000", 0.84)
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
                        visible: !!picturePreview.item
                        enabled: illustrator.available && !illustrator.busy
                        text: qsTr("Another take")
                        onClicked: {
                            const target = picturePreview.item
                            picturePreview.close()
                            if (target)
                                illustrator.regenerate(target.id)
                        }
                    }
                    TextButton {
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
