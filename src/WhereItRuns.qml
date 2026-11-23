import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

import "Format.js" as Format
import "Ui.js" as Ui

// Where Dogear's text work happens: Gemini, this computer (a model server
// the reader runs), or both, with what this computer can do, measured.
ColumnLayout {
    id: section

    spacing: 8
    Material.roundedScale: theme.radius

    // The server picked from those found, before it's used.
    property var picked: null
    property string address: ""

    component Heading: Label {
        Layout.topMargin: 16
        color: theme.textTertiary
        font.pixelSize: 12
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.8
    }
    component Note: Label {
        Layout.fillWidth: true
        color: theme.textSecondary
        font.pixelSize: 13
        wrapMode: Text.Wrap
        lineHeight: 1.25
    }
    component Choice: AbstractButton {
        id: choice
        property bool selected: false
        Layout.fillWidth: true
        Layout.preferredWidth: 1
        implicitHeight: 38
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        background: Rectangle {
            radius: Math.min(8, theme.radius)
            color: choice.selected ? Ui.tint(theme.accent, 0.22) : choice.hovered ? Ui.tint(theme.text, 0.1) : Ui.tint(theme.text, 0.05)
            border.width: choice.selected ? 1.5 : 0
            border.color: theme.accent
        }
        contentItem: Label {
            text: choice.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: theme.text
            font.pixelSize: 13
            elide: Text.ElideRight
        }
    }

    Heading { text: qsTr("Where it runs") }
    RowLayout {
        Layout.fillWidth: true
        spacing: 8
        Choice { text: qsTr("Gemini"); selected: ai.preset === "cloud"; onClicked: ai.preset = "cloud" }
        Choice { text: qsTr("Both"); selected: ai.preset === "hybrid"; onClicked: ai.preset = "hybrid" }
        Choice { text: qsTr("This computer"); selected: ai.preset === "local"; onClicked: ai.preset = "local" }
    }
    Note {
        text: ai.preset === "cloud" ? qsTr("Gemini does the thinking: the best answers, with your key. A model on this computer stands in when there's no key.")
            : ai.preset === "hybrid" ? qsTr("Quick work (styles, definitions, translations, who's who) on this computer; careful work (explanations, recaps, Ask) with Gemini.")
            : qsTr("Everything that can run here does, and nothing is sent anywhere. What can't run here says so.")
    }

    // The model server on this computer.
    Rectangle {
        Layout.fillWidth: true
        Layout.topMargin: 4
        radius: Math.min(10, theme.radius)
        color: Ui.tint(theme.text, 0.04)
        border.color: Ui.tint(theme.text, 0.1)
        implicitHeight: local.implicitHeight + 24

        ColumnLayout {
            id: local
            x: 12
            y: 12
            width: parent.width - 24
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Rectangle {
                    width: 10; height: 10; radius: 5
                    color: (ai.localUrl !== "") ? theme.colors.green : theme.textTertiary
                }
                Label {
                    Layout.fillWidth: true
                    text: (ai.localUrl !== "") ? qsTr("%1 at %2").arg(ai.localName).arg(ai.localUrl.replace("http://", ""))
                                      : ai.findingLocal ? qsTr("Looking for model servers on this computer…")
                                      : qsTr("No model on this computer yet")
                    color: theme.text
                    font.pixelSize: 14
                    elide: Text.ElideRight
                }
                BusyIndicator { visible: ai.findingLocal; running: visible; implicitWidth: 20; implicitHeight: 20 }
                TextButton {
                    text: ai.localServers.length || (ai.localUrl !== "") ? qsTr("Find again") : qsTr("Find")
                    enabled: !ai.findingLocal
                    onClicked: ai.findLocal(section.address)
                }
            }
            // The models in use, changeable.
            GridLayout {
                Layout.fillWidth: true
                visible: (ai.localUrl !== "")
                columns: 2
                columnSpacing: 10
                rowSpacing: 4
                readonly property var server: {
                    for (const s of ai.localServers)
                        if (s.url === ai.localUrl)
                            return s
                    return null
                }
                Label { text: qsTr("Text"); color: theme.textSecondary; font.pixelSize: 13 }
                Label {
                    Layout.fillWidth: true
                    visible: !parent.server
                    text: ai.localTextModel
                    color: theme.text
                    font.pixelSize: 13
                }
                ComboBox {
                    Layout.fillWidth: true
                    visible: !!parent.server
                    model: parent.server ? parent.server.textModels : []
                    currentIndex: model.indexOf(ai.localTextModel)
                    onActivated: (index) => ai.useLocal(parent.server.kind, ai.localUrl, model[index], ai.localEmbeddingModel, ai.localSpeechModel)
                }
                Label {
                    visible: !!parent.server && parent.server.speechModels.length > 0
                    text: qsTr("Voices"); color: theme.textSecondary; font.pixelSize: 13
                }
                ComboBox {
                    Layout.fillWidth: true
                    visible: !!parent.server && parent.server.speechModels.length > 0
                    model: parent.server ? [qsTr("None")].concat(parent.server.speechModels) : []
                    currentIndex: Math.max(0, model.indexOf(ai.localSpeechModel))
                    onActivated: (index) => ai.useLocal(parent.server.kind, ai.localUrl, ai.localTextModel,
                                                         ai.localEmbeddingModel, index === 0 ? "" : model[index])
                }
                Label { text: qsTr("Search"); color: theme.textSecondary; font.pixelSize: 13 }
                Label {
                    Layout.fillWidth: true
                    visible: !parent.server
                    text: ai.localEmbeddingModel || qsTr("None")
                    color: ai.localEmbeddingModel ? theme.text : theme.textTertiary
                    font.pixelSize: 13
                }
                ComboBox {
                    Layout.fillWidth: true
                    visible: !!parent.server
                    model: parent.server ? [qsTr("None")].concat(parent.server.embeddingModels) : []
                    currentIndex: Math.max(0, model.indexOf(ai.localEmbeddingModel))
                    onActivated: (index) => ai.useLocal(parent.server.kind, ai.localUrl, ai.localTextModel,
                                                         index === 0 ? "" : model[index], ai.localSpeechModel)
                }
            }
            // Servers found, not yet in use.
            Repeater {
                model: ai.localServers.filter(s => s.url !== ai.localUrl)
                delegate: RowLayout {
                    required property var modelData
                    Layout.fillWidth: true
                    Label {
                        Layout.fillWidth: true
                        text: modelData.textModels.length
                              ? qsTr("%1 at %2 · %n models", "", modelData.textModels.length).arg(modelData.name).arg(modelData.url.replace("http://", ""))
                              : qsTr("%1 at %2 · no models yet").arg(modelData.name).arg(modelData.url.replace("http://", ""))
                        color: theme.textSecondary
                        font.pixelSize: 13
                        elide: Text.ElideRight
                    }
                    TextButton {
                        text: qsTr("Use")
                        enabled: modelData.textModels.length > 0 || modelData.speechModels.length > 0
                        onClicked: ai.useLocal(modelData.kind, modelData.url, modelData.textModels[0] || "",
                                               modelData.embeddingModels[0] || "", modelData.speechModels[0] || "")
                    }
                }
            }
            // Setting one up: Ollama installed, started, and a model that
            // suits this computer downloaded, one step at a time.
            ColumnLayout {
                Layout.fillWidth: true
                visible: ai.localUrl === "" && !ai.findingLocal
                spacing: 8
                Note {
                    text: localSetup.textModel.model === undefined
                        ? qsTr("This computer has too little memory for a model worth having. Gemini does this work instead.")
                        : qsTr("A model on this computer works offline, for free, and keeps your reading private. For this computer: %1 (%2), and %3 (%4) for search by meaning.")
                            .arg(localSetup.textModel.title).arg(Format.size(localSetup.textModel.bytes, Qt.locale()))
                            .arg(localSetup.searchModel.title).arg(Format.size(localSetup.searchModel.bytes, Qt.locale()))
                }
                RowLayout {
                    visible: localSetup.textModel.model !== undefined
                    spacing: 8
                    TextButton {
                        visible: !localSetup.running
                        primary: true
                        enabled: !localSetup.waiting
                        text: localSetup.installed ? qsTr("Start Ollama") : qsTr("Install Ollama")
                        onClicked: localSetup.install()
                    }
                    TextButton {
                        visible: localSetup.running
                        primary: true
                        enabled: !localSetup.pulling
                        // Already there (pulled before): a moment's check, then used.
                        readonly property bool have: ai.localServers.some(server => server.textModels.indexOf(localSetup.textModel.model) >= 0)
                        text: have ? qsTr("Use %1").arg(localSetup.textModel.title) : qsTr("Download %1").arg(localSetup.textModel.title)
                        onClicked: localSetup.download()
                    }
                    TextButton {
                        visible: localSetup.pulling
                        text: qsTr("Stop")
                        onClicked: localSetup.cancel()
                    }
                    BusyIndicator { visible: localSetup.waiting; running: visible; implicitWidth: 22; implicitHeight: 22 }
                }
                Note {
                    visible: localSetup.waiting
                    text: qsTr("Finish in the terminal (it asks for your password), and Dogear carries on by itself.")
                }
                ProgressBar {
                    Layout.fillWidth: true
                    visible: localSetup.pulling
                    value: localSetup.progress
                }
                Note {
                    visible: localSetup.status !== "" || localSetup.problem !== ""
                    text: localSetup.problem !== "" ? localSetup.problem : localSetup.status
                }
                Note {
                    visible: !localSetup.running && !localSetup.waiting
                    color: theme.textTertiary
                    font.pixelSize: 12
                    text: qsTr("Already run llama.cpp, LM Studio, MLX or vLLM? Start it, then Find, or give its address below.")
                }
            }
            RowLayout {
                Layout.fillWidth: true
                visible: !(ai.localUrl !== "")
                spacing: 8
                Rectangle {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36
                    radius: Math.min(8, theme.radius)
                    color: Ui.tint(theme.text, 0.06)
                    border.width: addressInput.activeFocus ? 1.5 : 0
                    border.color: theme.accent
                    TextInput {
                        id: addressInput
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 10
                        verticalAlignment: TextInput.AlignVCenter
                        color: theme.text
                        font.pixelSize: 13
                        selectByMouse: true
                        onTextChanged: section.address = text
                        Keys.onReturnPressed: ai.findLocal(text)
                        Text {
                            anchors.fill: parent
                            verticalAlignment: Text.AlignVCenter
                            visible: addressInput.text === ""
                            text: qsTr("Another address, as localhost:5000")
                            color: theme.textTertiary
                            font: addressInput.font
                        }
                    }
                }
            }
            TextButton {
                visible: (ai.localUrl !== "")
                text: qsTr("Stop using it")
                onClicked: ai.forgetLocal()
            }
        }
    }

    // Test this computer.
    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 6
        TextButton {
            text: systemCheck.running ? qsTr("Testing…") : systemCheck.verdicts.length ? qsTr("Test again") : qsTr("Test this computer")
            enabled: !systemCheck.running
            onClicked: systemCheck.run()
        }
        BusyIndicator { visible: systemCheck.running; running: visible; implicitWidth: 22; implicitHeight: 22 }
        Label {
            Layout.fillWidth: true
            text: {
                const m = systemCheck.machine
                const parts = [qsTr("%1 GB memory").arg(Math.round(m.memoryGb)), qsTr("%n cores", "", m.cores)]
                if (m.graphics)
                    parts.push(m.graphicsMemoryGb > 0 ? qsTr("%1 (%2 GB)").arg(m.graphics).arg(Math.round(m.graphicsMemoryGb)) : m.graphics)
                return parts.join(" · ")
            }
            color: theme.textTertiary
            font.pixelSize: 12
            elide: Text.ElideRight
            horizontalAlignment: Text.AlignRight
        }
    }
    Repeater {
        model: systemCheck.verdicts
        delegate: RowLayout {
            required property var modelData
            Layout.fillWidth: true
            spacing: 10
            Rectangle {
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 5
                width: 8; height: 8; radius: 4
                color: modelData.level === "good" ? theme.colors.green : modelData.level === "slow" ? theme.colors.yellow : theme.textTertiary
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 1
                Label {
                    Layout.fillWidth: true
                    text: modelData.feature + " · " + modelData.verdict
                    color: theme.text
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    visible: modelData.detail !== ""
                    text: modelData.detail
                    color: theme.textTertiary
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                }
            }
        }
    }
    Note {
        visible: systemCheck.recommendation !== ""
        text: qsTr("This computer: %1. %2").arg(systemCheck.tier).arg(systemCheck.recommendation)
        color: theme.text
    }
}
