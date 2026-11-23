import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Format.js" as Format

// The companion's first question, when there's no key and no model: where
// should it run? Gemini with the reader's key, this computer, or not now.
// Each choice shows its next step right there.
ColumnLayout {
    id: chooser

    property var look
    property string pick: localSetup.textModel.model !== undefined ? "local" : "gemini"

    signal notNow()

    spacing: 14

    Item { Layout.preferredHeight: 4 }
    Label {
        Layout.fillWidth: true
        text: qsTr("Ask about the book as you read")
        color: chooser.look.fg
        font.pixelSize: 16
        font.weight: Font.DemiBold
        wrapMode: Text.Wrap
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("It never reads past your page. To answer, it needs a model: choose where it runs. You can change this any time in Settings.")
        color: chooser.look.muted
        font.pixelSize: 13
        wrapMode: Text.Wrap
        lineHeight: 1.3
    }

    ButtonGroup { id: picks }
    OptionCard {
        Layout.fillWidth: true
        ButtonGroup.group: picks
        title: qsTr("Gemini, with your key")
        description: qsTr("The best answers, including the whole book so far. Costs a little; you set the budget.")
        tone: theme.cloud
        checked: chooser.pick === "gemini"
        onClicked: chooser.pick = "gemini"
    }
    OptionCard {
        Layout.fillWidth: true
        ButtonGroup.group: picks
        visible: localSetup.textModel.model !== undefined
        title: qsTr("This computer")
        description: qsTr("Free and private. Short answers are quick here; long ones are better with Gemini.")
        tone: theme.positive
        checked: chooser.pick === "local"
        onClicked: chooser.pick = "local"
    }
    OptionCard {
        Layout.fillWidth: true
        ButtonGroup.group: picks
        title: qsTr("Not now")
        description: qsTr("Keep reading without it.")
        tone: chooser.look.muted
        checked: chooser.pick === "later"
        onClicked: chooser.pick = "later"
    }

    // --- The next step for the choice ------------------------------------
    ColumnLayout {
        visible: chooser.pick === "gemini"
        Layout.fillWidth: true
        spacing: 8
        TextField {
            id: keyField
            Layout.fillWidth: true
            echoMode: TextInput.Password
            placeholderText: qsTr("Paste your Gemini API key")
            Keys.onReturnPressed: connect.clicked()
        }
        Label {
            Layout.fillWidth: true
            textFormat: Text.StyledText
            text: qsTr("Get one at <a href=\"https://aistudio.google.com/apikey\">aistudio.google.com/apikey</a>. It's kept in the system keyring.")
            color: chooser.look.muted
            linkColor: chooser.look.link
            font.pixelSize: 11
            wrapMode: Text.Wrap
            onLinkActivated: (link) => Qt.openUrlExternally(link)
        }
        TextButton {
            id: connect
            Layout.fillWidth: true
            primary: true
            text: ai.checking ? qsTr("Checking…") : qsTr("Connect")
            enabled: keyField.text.trim() !== "" && !ai.checking
            onClicked: ai.setKey(keyField.text.trim())
        }
        Label {
            Layout.fillWidth: true
            visible: ai.hasKey === false && ai.status !== "" && keyField.text !== ""
            text: ai.status
            color: chooser.look.muted
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
    }
    ColumnLayout {
        visible: chooser.pick === "local"
        Layout.fillWidth: true
        spacing: 8
        Label {
            Layout.fillWidth: true
            text: {
                const size = localSetup.textModel.bytes > 0 ? Format.size(localSetup.textModel.bytes, Qt.locale()) : ""
                return localSetup.running
                    ? qsTr("Download %1, chosen for this computer%2.").arg(localSetup.textModel.title).arg(size ? " · " + size : "")
                    : qsTr("Dogear installs Ollama (a terminal asks for your password; Dogear never sees it), then downloads %1, chosen for this computer%2.")
                          .arg(localSetup.textModel.title).arg(size ? " · " + size : "")
            }
            color: chooser.look.fg
            font.pixelSize: 13
            wrapMode: Text.Wrap
            lineHeight: 1.25
        }
        ProgressBar {
            Layout.fillWidth: true
            visible: localSetup.pulling
            value: localSetup.progress
        }
        Label {
            Layout.fillWidth: true
            visible: localSetup.status !== "" || localSetup.problem !== ""
            text: localSetup.problem !== "" ? localSetup.problem : localSetup.status
            color: chooser.look.muted
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        TextButton {
            Layout.fillWidth: true
            primary: true
            enabled: !localSetup.waiting && !localSetup.pulling
            text: localSetup.running ? qsTr("Download %1").arg(localSetup.textModel.title)
                : localSetup.installed ? qsTr("Start Ollama") : qsTr("Set up this computer")
            onClicked: localSetup.running ? localSetup.download() : localSetup.install()
        }
    }
    ColumnLayout {
        visible: chooser.pick === "later"
        Layout.fillWidth: true
        spacing: 8
        Label {
            Layout.fillWidth: true
            text: qsTr("Everything else in Dogear works as before. The companion is a click away (C) when you want it.")
            color: chooser.look.fg
            font.pixelSize: 13
            wrapMode: Text.Wrap
            lineHeight: 1.25
        }
        TextButton {
            Layout.fillWidth: true
            text: qsTr("Back to the book")
            onClicked: chooser.notNow()
        }
    }
    Item { Layout.fillHeight: true }

    Component.onCompleted: localSetup.refresh()
}
