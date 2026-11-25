import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

import "../components/Format.js" as Format
import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui

// Right-column "This computer" card on Settings › Intelligence: hardware spec,
// local model server status (with live Test connection, model pickers, custom
// server URL, Ollama setup & one-click pull with progress/cancel), compact
// hardware summary verdicts, and full benchmark with cancel & refresh.
Rectangle {
    id: section

    property string address: ""
    property bool configureOpen: false
    property bool otherServerOpen: false
    property bool detailsOpen: false

    function normUrl(u) {
        return String(u || "").replace(/\/+$/, "").replace("://127.0.0.1", "://localhost").replace("://[::1]", "://localhost")
    }
    function modelsWithSaved(list, saved) {
        const out = [qsTr("None")].concat(list || [])
        if (saved && saved !== "" && out.indexOf(saved) < 0)
            out.push(saved)
        return out
    }

    readonly property bool reachable: ai.localUrl !== "" && ai.localServers.some(
        s => normUrl(s.url) === normUrl(ai.localUrl))
    readonly property var currentServer: {
        const current = normUrl(ai.localUrl)
        for (const s of ai.localServers)
            if (normUrl(s.url) === current)
                return s
        return null
    }
    readonly property bool currentIsOllama: currentServer ? currentServer.kind === "ollama"
                                          : (ai.localName === "Ollama" && localSetup.running)
    readonly property string modelsLine: {
        void(ai.localTextModel); void(ai.localEmbeddingModel); void(ai.localSpeechModel)
        const summary = ai.localModelsSummary()
        if (summary !== "")
            return summary
        if (ai.localUrl !== "")
            return ai.localUrl.replace(/^https?:\/\//, "")
        if (localSetup.textModel.model !== undefined)
            return qsTr("Recommended: %1").arg(localSetup.textModel.title)
        return qsTr("Not enough memory for a local model")
    }
    readonly property string hardwareLine: {
        const m = systemCheck.machine
        if (!m.memoryGb)
            return ""
        const parts = [qsTr("%1 GB").arg(Math.round(m.memoryGb)), qsTr("%n cores", "", m.cores)]
        if (m.discreteGraphics && m.graphics)
            parts.push(m.graphicsMemoryGb > 0 ? qsTr("%1 GB GPU").arg(Math.round(m.graphicsMemoryGb)) : m.graphics)
        return parts.join(" · ")
    }

    implicitHeight: bodyCol.implicitHeight + 32
    radius: Ui.radius(10, theme.radius)
    color: Ui.tint(theme.ink1, 0.03)
    border.width: 1
    border.color: Ui.tint(theme.ink1, 0.07)

    ColumnLayout {
        id: bodyCol
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        spacing: 12

        // Card header: "This computer" + "16 GB · 8 cores"
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            Label {
                Layout.fillWidth: true
                text: qsTr("This computer")
                color: theme.ink1
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }
            Label {
                visible: section.hardwareLine !== ""
                text: section.hardwareLine
                color: theme.ink3
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        // Nested status box: status dot, server running state, active models
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: statusRow.implicitHeight + 20
            radius: Ui.radius(7, theme.radius)
            color: Ui.tint(theme.ink1, 0.04)

            RowLayout {
                id: statusRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 10
                spacing: 10

                Rectangle {
                    Layout.alignment: Qt.AlignVCenter
                    Layout.preferredWidth: 7
                    Layout.preferredHeight: 7
                    radius: 3.5
                    color: ai.localUrl === "" ? (localSetup.running ? theme.positive : theme.ink4)
                         : section.reachable ? theme.positive
                         : (ai.findingLocal || ai.testingLocal) ? theme.cloud
                         : theme.danger
                }
                ColumnLayout {
                    Layout.fillWidth: true
                    spacing: 1
                    Label {
                        Layout.fillWidth: true
                        text: ai.localUrl !== ""
                              ? (section.reachable || ai.findingLocal
                                 ? qsTr("%1 is running").arg(ai.localName)
                                 : qsTr("%1 is not running").arg(ai.localName))
                              : ai.findingLocal ? qsTr("Looking for model servers…")
                              : localSetup.running ? qsTr("Ollama is running")
                              : qsTr("No local model yet")
                        color: theme.ink1
                        font.pixelSize: 13
                        font.weight: Font.Medium
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        text: section.modelsLine
                        color: theme.ink3
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }
                BusyIndicator {
                    visible: ai.findingLocal || ai.testingLocal
                    running: visible
                    implicitWidth: 16
                    implicitHeight: 16
                }
                TextButton {
                    visible: ai.localUrl !== ""
                    size: "compact"
                    ghost: true
                    text: section.configureOpen ? qsTr("Done") : qsTr("Configure")
                    onClicked: section.configureOpen = !section.configureOpen
                }
            }
        }

        // Live test result or local server probe error.
        Label {
            Layout.fillWidth: true
            visible: ai.localError !== "" || ai.localTestResult !== ""
            text: ai.localError !== "" ? ai.localError : ai.localTestResult
            color: ai.localError !== "" ? theme.danger : theme.positiveInk
            font.pixelSize: 11
            wrapMode: Text.Wrap
        }

        // Active Ollama pull / install progress (shown whether already configured or not).
        ColumnLayout {
            Layout.fillWidth: true
            visible: localSetup.waiting || localSetup.pulling || localSetup.status !== "" || localSetup.problem !== ""
            spacing: 6

            Label {
                Layout.fillWidth: true
                visible: localSetup.waiting
                text: qsTr("Finish in the terminal (it asks for your password), and Clothbound carries on by itself.")
                color: theme.ink3
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
            RowLayout {
                Layout.fillWidth: true
                visible: localSetup.pulling || localSetup.waiting
                spacing: 8
                BusyIndicator {
                    visible: localSetup.waiting
                    running: visible
                    implicitWidth: 14
                    implicitHeight: 14
                }
                ProgressBar {
                    Layout.fillWidth: true
                    visible: localSetup.pulling
                    value: localSetup.progress
                }
                TextButton {
                    size: "compact"
                    text: localSetup.pulling ? qsTr("Stop") : qsTr("Cancel")
                    onClicked: localSetup.cancel()
                }
            }
            Label {
                Layout.fillWidth: true
                visible: localSetup.status !== "" || localSetup.problem !== ""
                text: localSetup.problem !== "" ? localSetup.problem : localSetup.status
                color: localSetup.problem !== "" ? theme.danger : theme.ink3
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
        }

        // --- Expanded local model & server controls (when configured) ------
        ColumnLayout {
            Layout.fillWidth: true
            visible: ai.localUrl !== "" && section.configureOpen
            spacing: 10

            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 10
                rowSpacing: 6

                Label { text: qsTr("Text"); color: theme.ink3; font.pixelSize: 12 }
                Label {
                    Layout.fillWidth: true
                    visible: !section.currentServer
                    text: ai.localTextModel || qsTr("None")
                    color: ai.localTextModel ? theme.ink1 : theme.ink4
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                ComboBox {
                    Layout.fillWidth: true
                    visible: !!section.currentServer
                    model: section.currentServer ? section.modelsWithSaved(section.currentServer.textModels, ai.localTextModel) : []
                    currentIndex: Math.max(0, model.indexOf(ai.localTextModel))
                    onActivated: (index) => ai.useLocal(section.currentServer.kind, ai.localUrl,
                                                         index === 0 ? "" : model[index], ai.localEmbeddingModel, ai.localSpeechModel)
                }

                Label {
                    visible: !section.currentServer || section.currentServer.speechModels.length > 0 || ai.localSpeechModel !== ""
                    text: qsTr("Voices")
                    color: theme.ink3
                    font.pixelSize: 12
                }
                Label {
                    Layout.fillWidth: true
                    visible: !section.currentServer || (section.currentServer.speechModels.length === 0 && ai.localSpeechModel === "")
                    text: ai.localSpeechModel || qsTr("None")
                    color: ai.localSpeechModel ? theme.ink1 : theme.ink4
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                ComboBox {
                    Layout.fillWidth: true
                    visible: !!section.currentServer && (section.currentServer.speechModels.length > 0 || ai.localSpeechModel !== "")
                    model: section.currentServer ? section.modelsWithSaved(section.currentServer.speechModels, ai.localSpeechModel) : []
                    currentIndex: Math.max(0, model.indexOf(ai.localSpeechModel))
                    onActivated: (index) => ai.useLocal(section.currentServer.kind, ai.localUrl, ai.localTextModel,
                                                         ai.localEmbeddingModel, index === 0 ? "" : model[index])
                }

                Label { text: qsTr("Search"); color: theme.ink3; font.pixelSize: 12 }
                Label {
                    Layout.fillWidth: true
                    visible: !section.currentServer
                    text: ai.localEmbeddingModel || qsTr("None")
                    color: ai.localEmbeddingModel ? theme.ink1 : theme.ink4
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                ComboBox {
                    Layout.fillWidth: true
                    visible: !!section.currentServer
                    model: section.currentServer ? section.modelsWithSaved(section.currentServer.embeddingModels, ai.localEmbeddingModel) : []
                    currentIndex: Math.max(0, model.indexOf(ai.localEmbeddingModel))
                    onActivated: (index) => ai.useLocal(section.currentServer.kind, ai.localUrl, ai.localTextModel,
                                                         index === 0 ? "" : model[index], ai.localSpeechModel)
                }
            }

            // One-click pull for missing Ollama embedding model or any Ollama model tag.
            ColumnLayout {
                Layout.fillWidth: true
                visible: section.currentIsOllama
                spacing: 6

                RowLayout {
                    Layout.fillWidth: true
                    visible: ai.localEmbeddingModel === "" && localSetup.searchModel.model !== undefined
                    spacing: 8
                    Label {
                        Layout.fillWidth: true
                        text: (localSetup.searchModel.bytes || 0) > 0
                              ? qsTr("Search model not installed (%1)").arg(Format.size(localSetup.searchModel.bytes, Qt.locale()))
                              : qsTr("Search model not installed")
                        color: theme.ink3
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                    TextButton {
                        size: "compact"
                        enabled: !localSetup.pulling
                        text: qsTr("Pull %1").arg(localSetup.searchModel.title || localSetup.searchModel.model || "")
                        onClicked: localSetup.pullModel(localSetup.searchModel.model)
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    TextField {
                        id: pullModelInput
                        Layout.fillWidth: true
                        placeholderText: qsTr("Pull Ollama model (e.g. qwen3.5:4b)")
                        Keys.onReturnPressed: if (text.trim() !== "" && !localSetup.pulling) { localSetup.pullModel(text.trim()); text = "" }
                        Keys.onEnterPressed: if (text.trim() !== "" && !localSetup.pulling) { localSetup.pullModel(text.trim()); text = "" }
                    }
                    TextButton {
                        size: "compact"
                        text: qsTr("Pull")
                        enabled: pullModelInput.text.trim() !== "" && !localSetup.pulling
                        onClicked: {
                            localSetup.pullModel(pullModelInput.text.trim())
                            pullModelInput.text = ""
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: ai.localContextWindow > 0
                text: qsTr("Context window: %1 tokens · %2")
                      .arg(ai.localContextWindow.toLocaleString(Qt.locale(), 'f', 0))
                      .arg(ai.localUrl.replace(/^https?:\/\//, ""))
                color: theme.ink4
                font.pixelSize: 11
                elide: Text.ElideRight
            }

            // Other servers found, not yet in use.
            Repeater {
                model: ai.localServers.filter(s => section.normUrl(s.url) !== section.normUrl(ai.localUrl))
                delegate: RowLayout {
                    required property var modelData
                    readonly property int totalModels: modelData.textModels.length + modelData.embeddingModels.length + modelData.speechModels.length
                    Layout.fillWidth: true
                    spacing: 6
                    Label {
                        Layout.fillWidth: true
                        text: parent.totalModels > 0
                              ? qsTr("%1 at %2 · %n models", "", parent.totalModels).arg(modelData.name).arg(modelData.url.replace(/^https?:\/\//, ""))
                              : qsTr("%1 at %2 · no models yet").arg(modelData.name).arg(modelData.url.replace(/^https?:\/\//, ""))
                        color: theme.ink3
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    TextButton {
                        size: "compact"
                        text: qsTr("Use")
                        enabled: parent.totalModels > 0
                        onClicked: ai.useLocal(modelData.kind, modelData.url, modelData.textModels[0] || "",
                                               modelData.embeddingModels[0] || "", modelData.speechModels[0] || "")
                    }
                }
            }

            // Probe or connect to another OpenAI-compatible URL without forgetting current server first.
            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                TextField {
                    id: customUrlInput
                    Layout.fillWidth: true
                    placeholderText: qsTr("Custom server URL (e.g. localhost:1234)")
                    onTextChanged: section.address = text
                    Keys.onReturnPressed: if (text.trim() !== "" && !ai.findingLocal) ai.findLocal(text.trim())
                    Keys.onEnterPressed: if (text.trim() !== "" && !ai.findingLocal) ai.findLocal(text.trim())
                }
                TextButton {
                    size: "compact"
                    text: qsTr("Probe")
                    enabled: customUrlInput.text.trim() !== "" && !ai.findingLocal
                    onClicked: ai.findLocal(customUrlInput.text.trim())
                }
            }

            Flow {
                Layout.fillWidth: true
                spacing: 8
                TextButton {
                    size: "compact"
                    text: ai.testingLocal ? qsTr("Testing…") : qsTr("Test connection")
                    enabled: !ai.testingLocal && !ai.findingLocal
                    onClicked: ai.testLocal()
                }
                TextButton {
                    size: "compact"
                    text: qsTr("Find again")
                    enabled: !ai.findingLocal
                    onClicked: ai.findLocal(section.address)
                }
                TextButton {
                    size: "compact"
                    ghost: true
                    text: qsTr("Stop using it")
                    onClicked: {
                        section.configureOpen = false
                        ai.forgetLocal()
                    }
                }
            }
        }

        // --- Setup flow (when no local model server is configured yet) -----
        ColumnLayout {
            Layout.fillWidth: true
            visible: ai.localUrl === "" && !ai.findingLocal
            spacing: 8

            // Discovered servers waiting to be used.
            Repeater {
                model: ai.localServers
                delegate: RowLayout {
                    required property var modelData
                    readonly property int totalModels: modelData.textModels.length + modelData.embeddingModels.length + modelData.speechModels.length
                    Layout.fillWidth: true
                    spacing: 6
                    Label {
                        Layout.fillWidth: true
                        text: parent.totalModels > 0
                              ? qsTr("%1 at %2 · %n models", "", parent.totalModels).arg(modelData.name).arg(modelData.url.replace(/^https?:\/\//, ""))
                              : qsTr("%1 at %2 · no models yet").arg(modelData.name).arg(modelData.url.replace(/^https?:\/\//, ""))
                        color: theme.ink2
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    TextButton {
                        size: "compact"
                        text: qsTr("Use")
                        enabled: parent.totalModels > 0
                        onClicked: ai.useLocal(modelData.kind, modelData.url, modelData.textModels[0] || "",
                                               modelData.embeddingModels[0] || "", modelData.speechModels[0] || "")
                    }
                }
            }

            Flow {
                Layout.fillWidth: true
                visible: localSetup.textModel.model !== undefined
                spacing: 8
                TextButton {
                    visible: !localSetup.running
                    size: "compact"
                    primary: true
                    enabled: !localSetup.waiting
                    text: localSetup.installed ? qsTr("Start Ollama") : qsTr("Install Ollama")
                    onClicked: localSetup.install()
                }
                TextButton {
                    visible: localSetup.running
                    size: "compact"
                    primary: true
                    enabled: !localSetup.pulling
                    readonly property bool have: ai.localServers.some(
                        server => server.kind === "ollama" && server.textModels.indexOf(localSetup.textModel.model) >= 0)
                    text: have ? qsTr("Use %1").arg(localSetup.textModel.title || "")
                               : (localSetup.textModel.bytes || 0) > 0
                               ? qsTr("Download %1 (%2)").arg(localSetup.textModel.title || "")
                                                         .arg(Format.size(localSetup.textModel.bytes, Qt.locale()))
                               : qsTr("Download %1").arg(localSetup.textModel.title || "")
                    onClicked: have ? localSetup.useExisting() : localSetup.download()
                }
                TextButton {
                    size: "compact"
                    ghost: true
                    visible: !localSetup.waiting && !localSetup.pulling
                    text: section.otherServerOpen ? qsTr("Hide") : qsTr("Other server…")
                    onClicked: section.otherServerOpen = !section.otherServerOpen
                }
            }

            // Custom server address (llama.cpp, LM Studio, MLX, vLLM).
            ColumnLayout {
                Layout.fillWidth: true
                visible: section.otherServerOpen || localSetup.textModel.model === undefined
                spacing: 6
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Already run llama.cpp, LM Studio, MLX or vLLM? Start it, then Find or enter its address:")
                    color: theme.ink4
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    TextField {
                        id: addressInput
                        Layout.fillWidth: true
                        placeholderText: qsTr("localhost:1234")
                        onTextChanged: section.address = text
                        Keys.onReturnPressed: if (text.trim() !== "" && !ai.findingLocal) ai.findLocal(text.trim())
                        Keys.onEnterPressed: if (text.trim() !== "" && !ai.findingLocal) ai.findLocal(text.trim())
                    }
                    TextButton {
                        size: "compact"
                        text: addressInput.text.trim() !== "" ? qsTr("Connect") : qsTr("Find")
                        enabled: !ai.findingLocal
                        onClicked: ai.findLocal(addressInput.text.trim())
                    }
                }
            }
        }

        // --- 3 compact hardware capability rows (check / amber info SVGs) --
        ColumnLayout {
            Layout.fillWidth: true
            visible: systemCheck.summaryVerdicts.length > 0
            spacing: 8

            Repeater {
                model: systemCheck.summaryVerdicts
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 8
                    Glyph {
                        Layout.preferredWidth: 14
                        Layout.preferredHeight: 14
                        stroke: modelData.level === "good" ? 2.4 : 2.1
                        path: modelData.level === "good" ? Icons.check : Icons.infoCircle
                        color: modelData.level === "good" ? theme.positive
                             : modelData.level === "slow" ? theme.cloud : theme.ink4
                    }
                    Label {
                        Layout.fillWidth: true
                        text: modelData.feature
                        color: theme.ink1
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                    Label {
                        text: modelData.detail
                        color: theme.ink3
                        font.pixelSize: 12
                    }
                }
            }
        }

        // --- Benchmark button + Cancel + optional full report toggle -------
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            TextButton {
                size: "compact"
                text: systemCheck.running ? qsTr("Testing…")
                    : systemCheck.verdicts.length ? qsTr("Test again")
                    : qsTr("Test this computer")
                enabled: !systemCheck.running
                onClicked: systemCheck.run()
            }
            TextButton {
                size: "compact"
                visible: systemCheck.running
                text: qsTr("Cancel")
                onClicked: systemCheck.cancel()
            }
            BusyIndicator {
                visible: systemCheck.running
                running: visible
                implicitWidth: 16
                implicitHeight: 16
            }
            Item { Layout.fillWidth: true }
            TextButton {
                size: "compact"
                ghost: true
                visible: systemCheck.verdicts.length > 0 || systemCheck.machine.memoryGb > 0
                text: section.detailsOpen ? qsTr("Hide details") : qsTr("Details")
                onClicked: section.detailsOpen = !section.detailsOpen
            }
        }

        // Detailed hardware & measured benchmark breakdown.
        ColumnLayout {
            Layout.fillWidth: true
            visible: section.detailsOpen
            spacing: 8

            Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }

            // Hardware specification summary + Refresh button.
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    text: {
                        const m = systemCheck.machine || ({})
                        const lines = []
                        if (m.processor)
                            lines.push(m.processor)
                        if (m.memoryGb > 0)
                            lines.push(qsTr("Memory: %1 GB total (%2 GB free)")
                                       .arg(Number(m.memoryGb).toLocaleString(Qt.locale(), 'f', 1))
                                       .arg(Number(m.availableGb || 0).toLocaleString(Qt.locale(), 'f', 1)))
                        if (m.graphics)
                            lines.push(m.graphicsMemoryGb > 0
                                       ? qsTr("Graphics: %1 (%2 GB)").arg(m.graphics).arg(Math.round(m.graphicsMemoryGb))
                                       : qsTr("Graphics: %1").arg(m.graphics))
                        if (m.features && m.features.length > 0)
                            lines.push(qsTr("Acceleration: %1").arg(m.features.join(", ")))
                        return lines.join("\n")
                    }
                    visible: text !== ""
                    color: theme.ink3
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                    lineHeight: 1.35
                }
                TextButton {
                    Layout.alignment: Qt.AlignTop
                    size: "compact"
                    ghost: true
                    text: qsTr("Refresh")
                    onClicked: systemCheck.refresh(true)
                }
            }

            Repeater {
                model: systemCheck.verdicts
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    spacing: 8
                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 5
                        Layout.preferredWidth: 6
                        Layout.preferredHeight: 6
                        radius: 3
                        color: modelData.level === "good" ? theme.positive
                             : modelData.level === "slow" ? theme.cloud : theme.ink4
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        Label {
                            Layout.fillWidth: true
                            text: modelData.feature + " · " + modelData.verdict
                            color: theme.ink2
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: modelData.detail !== ""
                            text: modelData.detail
                            color: theme.ink4
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                visible: systemCheck.recommendation !== ""
                text: qsTr("Tier: %1. %2").arg(systemCheck.tier).arg(systemCheck.recommendation)
                color: theme.ink3
                font.pixelSize: 12
                wrapMode: Text.Wrap
                lineHeight: 1.35
            }
        }
    }
}
