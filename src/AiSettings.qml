import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Icons.js" as Icons
import "Ui.js" as Ui

// Gemini for Dogear: the reader's own key, where text goes, and what it
// has cost this month. Nothing here is needed to read; it's what listening
// and the companion run on.
FocusScope {
    id: sheet

    property bool open: false

    signal closed()

    function show() {
        open = true
        keyInput.text = ""
        replacing = false
        if (ai.hasKey && !ai.connected && !ai.checking)
            ai.check()
        if (!ai.localServers.length && !ai.findingLocal)
            ai.findLocal()
        localSetup.refresh()
        systemCheck.refresh()
        forceActiveFocus()
        if (!ai.hasKey)
            keyInput.forceActiveFocus()
    }
    function close() {
        if (!open)
            return
        open = false
        closed()
    }
    function save() {
        if (keyInput.text.trim() !== "")
            ai.setKey(keyInput.text)
    }

    property bool replacing: false
    property bool routingExpanded: false
    readonly property bool askingForKey: !ai.hasKey || replacing

    visible: open
    Keys.onEscapePressed: close()

    Connections {
        target: ai
        function onKeyChecked(ok, message) {
            if (ok) {
                sheet.replacing = false
                keyInput.text = ""
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Ui.tint("black", theme.light ? 0.25 : 0.55)
        TapHandler { onTapped: sheet.close() }
        // The page underneath doesn't turn while the sheet is open.
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.NoButton
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(580, parent.width - 48)
        height: Math.min(parent.height - 48, content.implicitHeight + header.height + 90)
        radius: Math.min(12, theme.radius)
        color: theme.ground
        border.color: Ui.tint(theme.ink1, 0.12)
        TapHandler {}

        // The title and close stay put while the rest scrolls.
        RowLayout {
            id: header
            x: 28
            y: 24
            width: parent.width - 56
            Glyph { Layout.preferredWidth: 22; Layout.preferredHeight: 22; path: Icons.sparkle; color: theme.accentInk }
            Label {
                Layout.fillWidth: true
                text: qsTr("Intelligence")
                color: theme.ink1
                font.pixelSize: 20
                font.weight: Font.DemiBold
            }
            IconButton { glyph: Icons.close; tip: qsTr("Close (Esc)"); onClicked: sheet.close() }
        }

        Flickable {
            anchors.fill: parent
            anchors.margins: 28
            anchors.topMargin: header.y + header.height + 10
            contentHeight: content.implicitHeight
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            ColumnLayout {
                id: content
                width: parent.width
                spacing: 10

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Listen to any book in an expressive voice, and ask about what you're reading. Run models privately on this computer, or use your own Gemini key with your own budget.")
                    color: theme.ink2
                    font.pixelSize: 14
                    wrapMode: Text.Wrap
                    lineHeight: 1.3
                }

                // Status: a dot and a sentence.
                RowLayout {
                    Layout.topMargin: 10
                    Layout.fillWidth: true
                    spacing: 10
                    Rectangle {
                        width: 10
                        height: 10
                        radius: 5
                        color: ai.checking ? theme.colors.yellow
                             : ai.connected ? theme.colors.green
                             : ai.hasKey ? theme.colors.yellow : theme.ink3
                    }
                    Label {
                        Layout.fillWidth: true
                        text: ai.providerName + " · " + ai.status
                        color: theme.ink1
                        font.pixelSize: 14
                        wrapMode: Text.Wrap
                    }
                    BusyIndicator { visible: ai.checking; running: visible; implicitWidth: 22; implicitHeight: 22 }
                }
                Label {
                    Layout.fillWidth: true
                    visible: ai.connected && ai.textModel !== ""
                    text: qsTr("Using %1 to understand and %2 to speak.").arg(ai.textModel).arg(ai.speechModel)
                    color: theme.ink3
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }

                // The key.
                RowLayout {
                    Layout.fillWidth: true
                    visible: !sheet.askingForKey
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Your key is in %1.").arg(ai.keyLocation)
                        color: theme.ink2
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }
                    TextButton {
                        text: qsTr("Replace")
                        visible: !ai.keyFromEnvironment
                        onClicked: { sheet.replacing = true; keyInput.forceActiveFocus() }
                    }
                    TextButton {
                        text: qsTr("Remove")
                        visible: !ai.keyFromEnvironment
                        onClicked: ai.forgetKey()
                    }
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: sheet.askingForKey
                    spacing: 8
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 40
                            radius: Math.min(8, theme.radius)
                            color: Ui.tint(theme.ink1, 0.06)
                            border.width: keyInput.activeFocus ? 1.5 : 0
                            border.color: theme.accent
                            TextInput {
                                id: keyInput
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                verticalAlignment: TextInput.AlignVCenter
                                echoMode: TextInput.Password
                                color: theme.ink1
                                font.pixelSize: 14
                                selectByMouse: true
                                Keys.onReturnPressed: sheet.save()
                                Keys.onEnterPressed: sheet.save()
                                Keys.onEscapePressed: sheet.close()
                                Text {
                                    anchors.fill: parent
                                    verticalAlignment: Text.AlignVCenter
                                    visible: keyInput.text === ""
                                    text: qsTr("Paste your Gemini API key")
                                    color: theme.ink3
                                    font: keyInput.font
                                }
                            }
                        }
                        TextButton {
                            primary: true
                            text: qsTr("Save")
                            enabled: keyInput.text.trim() !== "" && !ai.checking
                            onClicked: sheet.save()
                        }
                    }
                    Label {
                        Layout.fillWidth: true
                        textFormat: Text.StyledText
                        text: qsTr("Get a key at <a href=\"https://aistudio.google.com/apikey\">aistudio.google.com/apikey</a>. It's checked, then kept in your keyring, never in a file.")
                        color: theme.ink3
                        linkColor: theme.accentInk
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                        onLinkActivated: (link) => Qt.openUrlExternally(link)
                        HoverHandler { cursorShape: parent.hoveredLink ? Qt.PointingHandCursor : Qt.ArrowCursor }
                    }
                }

                component Heading: Label {
                    Layout.topMargin: 16
                    color: theme.ink3
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 0.8
                }

                WhereItRuns { Layout.fillWidth: true }

                // Per-feature routing overrides (collapsible).
                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        AbstractButton {
                            id: routingToggle
                            Layout.fillWidth: true
                            implicitHeight: 32
                            hoverEnabled: true
                            focusPolicy: Qt.NoFocus
                            onClicked: sheet.routingExpanded = !sheet.routingExpanded
                            background: Rectangle {
                                radius: Math.min(8, theme.radius)
                                color: routingToggle.hovered ? Ui.tint(theme.ink1, 0.06) : "transparent"
                            }
                            contentItem: RowLayout {
                                spacing: 6
                                Glyph {
                                    Layout.leftMargin: 4
                                    Layout.preferredWidth: 14
                                    Layout.preferredHeight: 14
                                    path: sheet.routingExpanded ? Icons.chevronDown : Icons.chevronRight
                                    color: theme.ink3
                                }
                                Label {
                                    text: qsTr("Advanced routing")
                                    color: theme.ink3
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                    font.capitalization: Font.AllUppercase
                                    font.letterSpacing: 0.8
                                }
                                Rectangle {
                                    visible: ai.overrideCount > 0
                                    implicitHeight: 18
                                    implicitWidth: customCountLabel.implicitWidth + 10
                                    radius: 9
                                    color: Ui.tint(theme.accent, 0.2)
                                    Label {
                                        id: customCountLabel
                                        anchors.centerIn: parent
                                        text: qsTr("%1 custom").arg(ai.overrideCount.toLocaleString(Qt.locale(), 'f', 0))
                                        color: theme.accentInk
                                        font.pixelSize: 11
                                        font.weight: Font.Medium
                                    }
                                }
                                Item { Layout.fillWidth: true }
                            }
                        }
                        TextButton {
                            visible: sheet.routingExpanded && ai.overrideCount > 0
                            text: qsTr("Reset")
                            onClicked: ai.clearOverrides()
                        }
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        visible: sheet.routingExpanded
                        radius: Math.min(10, theme.radius)
                        color: Ui.tint(theme.ink1, 0.04)
                        border.color: Ui.tint(theme.ink1, 0.1)
                        implicitHeight: routingColumn.implicitHeight + 24

                        ColumnLayout {
                            id: routingColumn
                            x: 12
                            y: 12
                            width: parent.width - 24
                            spacing: 8

                            Label {
                                Layout.fillWidth: true
                                text: qsTr("Override where each feature runs, or leave on Auto to follow the preset above. Local overrides never fall back to Gemini.")
                                color: theme.ink2
                                font.pixelSize: 12
                                wrapMode: Text.Wrap
                                lineHeight: 1.25
                            }

                            Repeater {
                                model: [
                                    { id: "ask",       label: qsTr("Ask about the book") },
                                    { id: "explain",   label: qsTr("Explain passage") },
                                    { id: "recap",     label: qsTr("Previously… recaps") },
                                    { id: "whoswho",   label: qsTr("Who's who") },
                                    { id: "define",    label: qsTr("Define word") },
                                    { id: "translate", label: qsTr("Translate") },
                                    { id: "search",    label: qsTr("Search by meaning") },
                                    { id: "director",  label: qsTr("Narration styles") },
                                    { id: "cast",      label: qsTr("Full cast") }
                                ]
                                delegate: RowLayout {
                                    id: routeRow
                                    required property var modelData
                                    readonly property string currentOverride: {
                                        void(ai.overrideCount); void(ai.preset)
                                        return ai.overrideFor(modelData.id)
                                    }
                                    readonly property string destination: {
                                        void(ai.overrideCount); void(ai.preset); void(ai.hasKey); void(ai.hasLocal); void(ai.localUrl); void(ai.localEmbeddingModel)
                                        return ai.whereFor(modelData.id)
                                    }
                                    Layout.fillWidth: true
                                    spacing: 8

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 1
                                        Label {
                                            Layout.fillWidth: true
                                            text: routeRow.modelData.label
                                            color: theme.ink1
                                            font.pixelSize: 13
                                            elide: Text.ElideRight
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: routeRow.destination
                                            color: theme.ink3
                                            font.pixelSize: 11
                                            elide: Text.ElideRight
                                        }
                                    }

                                    Row {
                                        spacing: 4
                                        Repeater {
                                            model: [
                                                { value: "",      label: qsTr("Auto") },
                                                { value: "cloud", label: qsTr("Gemini") },
                                                { value: "local", label: qsTr("Local") }
                                            ]
                                            delegate: AbstractButton {
                                                id: routePill
                                                required property var modelData
                                                readonly property bool active: routeRow.currentOverride === modelData.value
                                                implicitHeight: 26
                                                implicitWidth: routePillLabel.implicitWidth + 14
                                                hoverEnabled: true
                                                focusPolicy: Qt.NoFocus
                                                onClicked: ai.setOverride(routeRow.modelData.id, modelData.value)
                                                background: Rectangle {
                                                    radius: Math.min(6, theme.radius)
                                                    color: routePill.active ? Ui.tint(theme.accent, 0.22)
                                                         : routePill.hovered ? Ui.tint(theme.ink1, 0.1) : Ui.tint(theme.ink1, 0.05)
                                                    border.width: routePill.active ? 1 : 0
                                                    border.color: theme.accent
                                                }
                                                contentItem: Label {
                                                    id: routePillLabel
                                                    text: routePill.modelData.label
                                                    color: routePill.active ? theme.ink1 : theme.ink2
                                                    font.pixelSize: 11
                                                    font.weight: routePill.active ? Font.DemiBold : Font.Normal
                                                    horizontalAlignment: Text.AlignHCenter
                                                    verticalAlignment: Text.AlignVCenter
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }

                Heading { text: qsTr("Privacy") }
                Label {
                    Layout.fillWidth: true
                    text: (ai.preset === "local" && ai.hasLocal && ai.localIsLoopback
                           ? qsTr("Running on this computer with %1: nothing is sent anywhere.").arg(ai.localName)
                           : ai.privacyNote)
                          + " " + qsTr("Dogear sends only what a feature needs, and never text past where you're reading.")
                    color: theme.ink2
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    lineHeight: 1.3
                }

                Heading { text: qsTr("This month") }
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: ai.unlimitedBudget
                              ? qsTr("About %1 this month · no limit").arg(Number(ai.spentThisMonth).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
                              : ai.budget <= 0
                              ? qsTr("About %1 this month · cloud spending paused ($0 budget)").arg(Number(ai.spentThisMonth).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
                              : qsTr("About %1 of your %2 budget")
                                    .arg(Number(ai.spentThisMonth).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
                                    .arg(Number(ai.budget).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
                        color: theme.ink1
                        font.pixelSize: 14
                        wrapMode: Text.Wrap
                    }
                    IconButton {
                        tip: qsTr("Lower the budget")
                        enabled: !ai.unlimitedBudget && ai.budget > 0
                        contentItem: Label { text: "−"; color: theme.ink1; font.pixelSize: 18; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        onClicked: ai.budget = Math.max(0, ai.budget - (ai.budget > 20 ? 10 : 5))
                    }
                    IconButton {
                        tip: qsTr("Raise the budget")
                        contentItem: Label { text: "+"; color: theme.ink1; font.pixelSize: 18; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        onClicked: {
                            if (ai.unlimitedBudget)
                                ai.unlimitedBudget = false
                            ai.budget = ai.budget + (ai.budget >= 20 ? 10 : 5)
                        }
                    }
                    TextButton {
                        primary: ai.unlimitedBudget
                        text: qsTr("No limit")
                        onClicked: ai.unlimitedBudget = !ai.unlimitedBudget
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 6
                    radius: 3
                    visible: !ai.unlimitedBudget
                    color: Ui.tint(theme.ink1, 0.1)
                    Rectangle {
                        width: parent.width * Math.min(1, ai.budget > 0 ? ai.spentThisMonth / ai.budget : (ai.spentThisMonth > 0 ? 1 : 0))
                        height: parent.height
                        radius: 3
                        color: ai.budget <= 0 || ai.spentThisMonth > ai.budget * 0.8 ? theme.colors.orange : theme.accent
                    }
                }
                Repeater {
                    model: ai.usageThisMonth
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: {
                                const names = { listen: qsTr("Listening"), ask: qsTr("Asking"), explain: qsTr("Explaining"),
                                                define: qsTr("Defining"), translate: qsTr("Translating"), whoswho: qsTr("Who's who"),
                                                director: qsTr("Narration notes"), cast: qsTr("Full cast"), embed: qsTr("Search by meaning"),
                                                recap: qsTr("Recaps"), "voice design": qsTr("Designing voices"),
                                                voices: qsTr("Voice lists"), illustrate: qsTr("Illustrations") }
                                return names[modelData.feature] || modelData.feature
                            }
                            color: theme.ink2
                            font.pixelSize: 13
                        }
                        Label {
                            text: modelData.audioMinutes > 0.5
                                  ? qsTr("%1 · %n min of audio", "", Math.round(modelData.audioMinutes)).arg(Number(modelData.cost).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
                                  : qsTr("%1 · %n requests", "", modelData.requests).arg(Number(modelData.cost).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
                            color: theme.ink3
                            font.pixelSize: 13
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: ai.usageThisMonth.length === 0
                    text: qsTr("Nothing yet.")
                    color: theme.ink3
                    font.pixelSize: 13
                }

                Heading { text: qsTr("What things cost") }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Listening is about %1 an hour, or half that for books you download for offline. Asking about a book costs a few cents. Free keys work within Google's limits. These are estimates; Google's bill is the final word.")
                          .arg(Number(ai.listeningCost(60)).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
                    color: theme.ink2
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    lineHeight: 1.3
                }

                // Voices made on this computer, and what they take up.
                Heading {
                    visible: voicePacks.supported
                    text: qsTr("Built-in voices")
                }
                Label {
                    Layout.fillWidth: true
                    visible: voicePacks.supported
                    text: qsTr("Voices made on this computer: free, private, and offline, without a key. They're plainer than Gemini's and read without styles.")
                    color: theme.ink2
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    lineHeight: 1.3
                }
                Repeater {
                    model: voicePacks.megabytes >= 0 ? voicePacks.installedPacks() : []
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: modelData.title
                            color: theme.ink1
                            font.pixelSize: 13
                            elide: Text.ElideRight
                        }
                        Label {
                            text: qsTr("%1 MB").arg(Math.round(modelData.megabytes).toLocaleString(Qt.locale(), 'f', 0))
                            color: theme.ink3
                            font.pixelSize: 12
                        }
                        TextButton {
                            text: qsTr("Remove")
                            onClicked: voicePacks.remove(modelData.id)
                        }
                    }
                }
                Label {
                    visible: voicePacks.supported && voicePacks.megabytes < 1
                    text: qsTr("None yet. Choose one in the Narrator panel while reading (V).")
                    color: theme.ink3
                    font.pixelSize: 13
                }

                Heading { text: qsTr("Credits") }
                Label {
                    Layout.fillWidth: true
                    textFormat: Text.StyledText
                    text: qsTr("Built-in voices use <b>sherpa-onnx</b> (Apache 2.0) with <b>ONNX Runtime</b> (MIT) and <b>espeak-ng</b> (GPL 3.0), downloaded with the voices; <b>Kokoro-82M</b> (Apache 2.0); <b>Pocket TTS</b> by Kyutai (CC BY 4.0, its voices from Kyutai's recordings); and <b>Piper</b> voices (MIT, each voice under its recording's own license, in its folder). The reading face is <b>Literata</b> and the interface face <b>Inter</b> (both OFL).")
                    color: theme.ink2
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    lineHeight: 1.3
                }
            }
        }
    }
}
