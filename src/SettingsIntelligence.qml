import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Icons.js" as Icons
import "Ui.js" as Ui

// Settings › Intelligence: where listening and the companion do their
// thinking. A preset (Cloud, Hybrid, This computer) decides it for every
// feature, the table shows the result and lets one feature differ, and the
// two cards set up each side: Gemini with the reader's key and budget, and a
// model on this computer.
ColumnLayout {
    id: page

    spacing: 16

    property bool replacing: false
    readonly property bool askingForKey: !ai.hasKey || replacing

    // Called when the page is shown: fresh status for both sides.
    function prepare() {
        keyInput.text = ""
        replacing = false
        if (ai.hasKey && !ai.connected && !ai.checking)
            ai.check()
        if (!ai.localServers.length && !ai.findingLocal)
            ai.findLocal()
        localSetup.refresh()
        systemCheck.refresh()
    }
    function save() {
        if (keyInput.text.trim() !== "")
            ai.setKey(keyInput.text)
    }
    function money(value) { return Number(value).toLocaleCurrencyString(Qt.locale("en_US"), "$") }

    Connections {
        target: ai
        function onKeyChecked(ok, message) {
            if (ok) {
                page.replacing = false
                keyInput.text = ""
            }
        }
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("Nothing here is needed to read. It's what listening and the companion run on: Gemini with your own key, a model on this computer, or both.")
        color: theme.ink2
        font.pixelSize: 14
        wrapMode: Text.Wrap
        lineHeight: 1.3
    }

    // --- Where it runs ---------------------------------------------------
    SettingsCard {
        title: qsTr("Where it runs")
        Segmented {
            Layout.fillWidth: true
            model: [{ value: "cloud", label: qsTr("Cloud") },
                    { value: "hybrid", label: qsTr("Hybrid") },
                    { value: "local", label: qsTr("This computer") }]
            current: ai.preset
            onActivated: (value) => ai.preset = value
        }
        Label {
            Layout.fillWidth: true
            text: ai.preset === "cloud" ? qsTr("Gemini does the thinking: the best answers, with your key. A model on this computer stands in when there's no key.")
                : ai.preset === "hybrid" ? qsTr("Quick work (styles, definitions, translations, who's who) on this computer; careful work (explanations, recaps, Ask) with Gemini.")
                : qsTr("Everything that can run here does, and nothing is sent anywhere. What can't run here says so.")
            color: theme.ink2
            font.pixelSize: 13
            wrapMode: Text.Wrap
            lineHeight: 1.25
        }

        // Each feature, where it runs now, and a way to send it elsewhere.
        Rectangle { Layout.fillWidth: true; Layout.topMargin: 4; height: 1; color: theme.hairline }
        Repeater {
            model: [
                { id: "ask",       label: qsTr("Ask about the book") },
                { id: "explain",   label: qsTr("Explain a passage") },
                { id: "recap",     label: qsTr("Previously… recaps") },
                { id: "whoswho",   label: qsTr("Who's who") },
                { id: "define",    label: qsTr("Define a word") },
                { id: "translate", label: qsTr("Translate") },
                { id: "search",    label: qsTr("Search by meaning") },
                { id: "director",  label: qsTr("Narration styles") },
                { id: "cast",      label: qsTr("Full cast") }
            ]
            delegate: RowLayout {
                id: feature
                required property var modelData
                readonly property string route: {
                    void(ai.overrideCount); void(ai.preset); void(ai.hasKey); void(ai.hasLocal); void(ai.localUrl); void(ai.localEmbeddingModel)
                    return ai.routeFor(modelData.id)
                }
                readonly property string where: {
                    void(ai.overrideCount); void(ai.preset); void(ai.hasKey); void(ai.hasLocal); void(ai.localUrl); void(ai.localEmbeddingModel)
                    return ai.whereFor(modelData.id)
                }
                readonly property string override: {
                    void(ai.overrideCount); void(ai.preset)
                    return ai.overrideFor(modelData.id)
                }
                Layout.fillWidth: true
                spacing: 10
                Label {
                    Layout.fillWidth: true
                    text: feature.modelData.label
                    color: theme.ink1
                    font.pixelSize: 13
                    elide: Text.ElideRight
                }
                Badge {
                    where: feature.route === "local" && ai.localIsLoopback ? "local"
                         : feature.route === "" ? "none" : "cloud"
                    text: feature.route === "" ? qsTr("Not available")
                        : feature.route === "local" && ai.localIsLoopback ? qsTr("On this computer")
                        : feature.where
                    ToolTip.visible: badgeHover.hovered
                    ToolTip.text: feature.where
                    ToolTip.delay: 500
                    HoverHandler { id: badgeHover }
                }
                ComboBox {
                    Layout.preferredWidth: 104
                    model: [qsTr("Auto"), qsTr("Gemini"), qsTr("Local")]
                    currentIndex: feature.override === "cloud" ? 1 : feature.override === "local" ? 2 : 0
                    onActivated: (index) => ai.setOverride(feature.modelData.id, ["", "cloud", "local"][index])
                }
            }
        }
        RowLayout {
            visible: ai.overrideCount > 0
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("%n features differ from the preset.", "", ai.overrideCount)
                color: theme.ink3
                font.pixelSize: 12
            }
            TextButton { text: qsTr("Reset to preset"); onClicked: ai.clearOverrides() }
        }
    }

    // --- Gemini ----------------------------------------------------------
    SettingsCard {
        title: qsTr("Gemini")
        glyph: Icons.sparkle
        glyphColor: theme.cloud
        description: qsTr("Your own key, from Google AI Studio. It's checked, kept in the system keyring, and never written to a file.")

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Rectangle {
                width: 8; height: 8; radius: 4
                color: ai.checking ? theme.cloud : ai.connected ? theme.positive : ai.hasKey ? theme.cloud : theme.ink4
            }
            Label {
                Layout.fillWidth: true
                text: ai.status
                color: theme.ink1
                font.pixelSize: 13
                wrapMode: Text.Wrap
            }
            BusyIndicator { visible: ai.checking; running: visible; implicitWidth: 20; implicitHeight: 20 }
        }
        Label {
            Layout.fillWidth: true
            visible: ai.connected && ai.textModel !== ""
            text: qsTr("Using %1 to understand and %2 to speak.").arg(ai.textModel).arg(ai.speechModel)
            color: theme.ink3
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        RowLayout {
            Layout.fillWidth: true
            visible: !page.askingForKey
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
                onClicked: { page.replacing = true; keyInput.forceActiveFocus() }
            }
            TextButton {
                text: qsTr("Remove")
                visible: !ai.keyFromEnvironment
                onClicked: ai.forgetKey()
            }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: page.askingForKey
            spacing: 8
            TextField {
                id: keyInput
                Layout.fillWidth: true
                echoMode: TextInput.Password
                placeholderText: qsTr("Paste your Gemini API key")
                Keys.onReturnPressed: page.save()
                Keys.onEnterPressed: page.save()
            }
            TextButton {
                primary: true
                text: qsTr("Save")
                enabled: keyInput.text.trim() !== "" && !ai.checking
                onClicked: page.save()
            }
        }
        Label {
            Layout.fillWidth: true
            visible: page.askingForKey
            textFormat: Text.StyledText
            text: qsTr("Get a key at <a href=\"https://aistudio.google.com/apikey\">aistudio.google.com/apikey</a>.")
            color: theme.ink3
            linkColor: theme.accentInk
            font.pixelSize: 12
            wrapMode: Text.Wrap
            onLinkActivated: (link) => Qt.openUrlExternally(link)
            HoverHandler { cursorShape: parent.hoveredLink ? Qt.PointingHandCursor : Qt.ArrowCursor }
        }

        Rectangle { Layout.fillWidth: true; height: 1; color: theme.hairline }

        // The month's spending against the reader's budget.
        RowLayout {
            Layout.fillWidth: true
            spacing: 6
            Label {
                Layout.fillWidth: true
                text: ai.unlimitedBudget
                      ? qsTr("About %1 this month · no limit").arg(page.money(ai.spentThisMonth))
                      : ai.budget <= 0
                      ? qsTr("About %1 this month · cloud spending paused ($0 budget)").arg(page.money(ai.spentThisMonth))
                      : qsTr("About %1 of your %2 budget").arg(page.money(ai.spentThisMonth)).arg(page.money(ai.budget))
                color: theme.ink1
                font.pixelSize: 13
                wrapMode: Text.Wrap
            }
            IconButton {
                tip: qsTr("Lower the budget")
                enabled: !ai.unlimitedBudget && ai.budget > 0
                contentItem: Label { text: "−"; color: theme.ink1; font.pixelSize: 17; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                onClicked: ai.budget = Math.max(0, ai.budget - (ai.budget > 20 ? 10 : 5))
            }
            IconButton {
                tip: qsTr("Raise the budget")
                contentItem: Label { text: "+"; color: theme.ink1; font.pixelSize: 17; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
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
            Layout.preferredHeight: 4
            radius: 2
            visible: !ai.unlimitedBudget
            color: theme.separator
            Rectangle {
                width: parent.width * Math.min(1, ai.budget > 0 ? ai.spentThisMonth / ai.budget : (ai.spentThisMonth > 0 ? 1 : 0))
                height: parent.height
                radius: 2
                color: ai.budget <= 0 || ai.spentThisMonth > ai.budget * 0.8 ? theme.cloud : theme.accent
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
                    font.pixelSize: 12
                }
                Label {
                    text: modelData.audioMinutes > 0.5
                          ? qsTr("%1 · %n min of audio", "", Math.round(modelData.audioMinutes)).arg(page.money(modelData.cost))
                          : qsTr("%1 · %n requests", "", modelData.requests).arg(page.money(modelData.cost))
                    color: theme.ink3
                    font.pixelSize: 12
                    font.features: { "tnum": 1 }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Listening is about %1 an hour, or half that for books you download for offline. Asking about a book costs a few cents. These are estimates; Google's bill is the final word.")
                  .arg(page.money(ai.listeningCost(60)))
            color: theme.ink3
            font.pixelSize: 12
            wrapMode: Text.Wrap
            lineHeight: 1.25
        }
    }

    // --- This computer ---------------------------------------------------
    SettingsCard {
        title: qsTr("This computer")
        glyph: Icons.sparkle
        glyphColor: theme.positive
        description: qsTr("A model running here works offline, for free, and keeps your reading private.")
        WhereItRuns { Layout.fillWidth: true }
    }

    // --- Privacy ---------------------------------------------------------
    Label {
        Layout.fillWidth: true
        text: (ai.preset === "local" && ai.hasLocal && ai.localIsLoopback
               ? qsTr("Running on this computer with %1: nothing is sent anywhere.").arg(ai.localName)
               : ai.privacyNote)
              + " " + qsTr("Dogear sends only what a feature needs, and never text past where you're reading.")
        color: theme.ink3
        font.pixelSize: 12
        wrapMode: Text.Wrap
        lineHeight: 1.3
    }
}
