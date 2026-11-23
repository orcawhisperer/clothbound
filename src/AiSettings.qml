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
        color: theme.background
        border.color: Ui.tint(theme.text, 0.12)
        TapHandler {}

        // The title and close stay put while the rest scrolls.
        RowLayout {
            id: header
            x: 28
            y: 24
            width: parent.width - 56
            Glyph { Layout.preferredWidth: 22; Layout.preferredHeight: 22; path: Icons.sparkle; color: theme.accent }
            Label {
                Layout.fillWidth: true
                text: qsTr("Intelligence")
                color: theme.text
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

            ColumnLayout {
                id: content
                width: parent.width
                spacing: 10

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Listen to any book in an expressive voice, and ask about what you're reading. Dogear uses your own Gemini key, so you choose what it costs.")
                    color: theme.textSecondary
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
                             : ai.hasKey ? theme.colors.yellow : theme.textTertiary
                    }
                    Label {
                        Layout.fillWidth: true
                        text: ai.providerName + " · " + ai.status
                        color: theme.text
                        font.pixelSize: 14
                        wrapMode: Text.Wrap
                    }
                    BusyIndicator { visible: ai.checking; running: visible; implicitWidth: 22; implicitHeight: 22 }
                }
                Label {
                    Layout.fillWidth: true
                    visible: ai.connected && ai.textModel !== ""
                    text: qsTr("Using %1 to understand and %2 to speak.").arg(ai.textModel).arg(ai.speechModel)
                    color: theme.textTertiary
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
                        color: theme.textSecondary
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
                            color: Ui.tint(theme.text, 0.06)
                            border.width: keyInput.activeFocus ? 1.5 : 0
                            border.color: theme.accent
                            TextInput {
                                id: keyInput
                                anchors.fill: parent
                                anchors.leftMargin: 12
                                anchors.rightMargin: 12
                                verticalAlignment: TextInput.AlignVCenter
                                echoMode: TextInput.Password
                                color: theme.text
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
                                    color: theme.textTertiary
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
                        color: theme.textTertiary
                        linkColor: theme.accent
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                        onLinkActivated: (link) => Qt.openUrlExternally(link)
                        HoverHandler { cursorShape: parent.hoveredLink ? Qt.PointingHandCursor : Qt.ArrowCursor }
                    }
                }

                component Heading: Label {
                    Layout.topMargin: 16
                    color: theme.textTertiary
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    font.capitalization: Font.AllUppercase
                    font.letterSpacing: 0.8
                }

                WhereItRuns { Layout.fillWidth: true }

                Heading { text: qsTr("Privacy") }
                Label {
                    Layout.fillWidth: true
                    text: ai.privacyNote + " " + qsTr("Dogear sends only what a feature needs, and never text past where you're reading.")
                    color: theme.textSecondary
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    lineHeight: 1.3
                }

                Heading { text: qsTr("This month") }
                RowLayout {
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("About %1 of your %2 budget")
                              .arg(Number(ai.spentThisMonth).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
                              .arg(Number(ai.budget).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
                        color: theme.text
                        font.pixelSize: 14
                        wrapMode: Text.Wrap
                    }
                    IconButton {
                        tip: qsTr("Lower the budget")
                        contentItem: Label { text: "−"; color: theme.text; font.pixelSize: 18; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        onClicked: ai.budget = Math.max(0, ai.budget - (ai.budget > 20 ? 10 : 5))
                    }
                    IconButton {
                        tip: qsTr("Raise the budget")
                        contentItem: Label { text: "+"; color: theme.text; font.pixelSize: 18; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                        onClicked: ai.budget = ai.budget + (ai.budget >= 20 ? 10 : 5)
                    }
                }
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 6
                    radius: 3
                    color: Ui.tint(theme.text, 0.1)
                    Rectangle {
                        width: parent.width * Math.min(1, ai.budget > 0 ? ai.spentThisMonth / ai.budget : 0)
                        height: parent.height
                        radius: 3
                        color: ai.spentThisMonth > ai.budget * 0.8 ? theme.colors.orange : theme.accent
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
                                                director: qsTr("Narration notes"), recap: qsTr("Recaps"), "voice design": qsTr("Designing voices"),
                                                voices: qsTr("Voice lists"), illustrate: qsTr("Illustrations") }
                                return names[modelData.feature] || modelData.feature
                            }
                            color: theme.textSecondary
                            font.pixelSize: 13
                        }
                        Label {
                            text: modelData.audioMinutes > 0.5
                                  ? qsTr("%1 · %n min of audio", "", Math.round(modelData.audioMinutes)).arg(Number(modelData.cost).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
                                  : qsTr("%1 · %n requests", "", modelData.requests).arg(Number(modelData.cost).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
                            color: theme.textTertiary
                            font.pixelSize: 13
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: ai.usageThisMonth.length === 0
                    text: qsTr("Nothing yet.")
                    color: theme.textTertiary
                    font.pixelSize: 13
                }

                Heading { text: qsTr("What things cost") }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Listening is about %1 an hour, or half that for books you download for offline. Asking about a book costs a few cents. Free keys work within Google's limits. These are estimates; Google's bill is the final word.")
                          .arg(Number(ai.listeningCost(60)).toLocaleCurrencyString(Qt.locale("en_US"), "$"))
                    color: theme.textSecondary
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
                    color: theme.textSecondary
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
                            color: theme.text
                            font.pixelSize: 13
                            elide: Text.ElideRight
                        }
                        Label {
                            text: qsTr("%1 MB").arg(Math.round(modelData.megabytes).toLocaleString(Qt.locale(), 'f', 0))
                            color: theme.textTertiary
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
                    color: theme.textTertiary
                    font.pixelSize: 13
                }

                Heading { text: qsTr("Credits") }
                Label {
                    Layout.fillWidth: true
                    textFormat: Text.StyledText
                    text: qsTr("Built-in voices use <b>sherpa-onnx</b> (Apache 2.0) with <b>ONNX Runtime</b> (MIT) and <b>espeak-ng</b> (GPL 3.0), downloaded with the voices; <b>Kokoro-82M</b> (Apache 2.0); <b>Pocket TTS</b> by Kyutai (CC BY 4.0, its voices from Kyutai's recordings); and <b>Piper</b> voices (MIT, each voice under its recording's own license, in its folder). The reading face is <b>Literata</b> (OFL).")
                    color: theme.textSecondary
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    lineHeight: 1.3
                }
            }
        }
    }
}
