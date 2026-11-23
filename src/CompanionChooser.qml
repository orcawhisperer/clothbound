import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Format.js" as Format
import "Icons.js" as Icons
import "Ui.js" as Ui

// The companion's first question (J5), when there's no key and no model:
// where should it run? Gemini with the reader's key, this computer, or not now.
// Each choice reveals its next step directly below.
ColumnLayout {
    id: chooser

    // Kept for caller compatibility; inherits app-chrome palette from CompanionPanel.
    property var look
    property bool userPicked: false
    property string pick: "gemini"
    property string keyError: ""
    property bool showKey: false

    readonly property bool localOffered: localSetup.textModel.model !== undefined
    readonly property bool haveModel: localOffered && ai.localServers.some(
        s => s.kind === "ollama" && s.textModels.indexOf(localSetup.textModel.model) >= 0)
    readonly property string downloadSize: {
        const textBytes = Number(localSetup.textModel.bytes) || 0
        const searchBytes = Number(localSetup.searchModel.bytes) || 0
        const total = textBytes + searchBytes
        return total > 0 ? Format.size(total, Qt.locale()) : ""
    }

    signal notNow()

    function selectPick(choice) {
        userPicked = true
        pick = choice
        if (choice === "gemini")
            Qt.callLater(() => { if (chooser.visible && chooser.pick === "gemini") keyField.forceActiveFocus() })
    }

    onLocalOfferedChanged: {
        if (!userPicked)
            pick = localOffered ? "local" : "gemini"
    }

    Connections {
        target: ai
        function onKeyChecked(ok, message) {
            chooser.keyError = ok ? "" : (message || qsTr("Couldn't verify this key."))
            if (ok) {
                keyField.text = ""
                chooser.showKey = false
            }
        }
    }

    spacing: 14

    // Reassure the reader when they invoked Companion with a specific question or selection.
    Rectangle {
        Layout.fillWidth: true
        visible: companion.hasPendingSetup && companion.pendingSetupLabel !== ""
        implicitHeight: pendingRow.implicitHeight + 16
        radius: Ui.radius(9, theme.radius)
        color: theme.accentSoft
        border.width: 1
        border.color: Ui.tint(theme.accent, 0.28)
        RowLayout {
            id: pendingRow
            anchors.fill: parent
            anchors.leftMargin: 10
            anchors.rightMargin: 10
            anchors.topMargin: 8
            anchors.bottomMargin: 8
            spacing: 8
            Glyph {
                Layout.preferredWidth: 13
                Layout.preferredHeight: 13
                stroke: 1.9
                path: Icons.sparkle
                color: theme.accentInk
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Up next once ready: %1").arg(companion.pendingSetupLabel)
                color: theme.accentInk
                font.pixelSize: 12
                font.weight: Font.Medium
                elide: Text.ElideRight
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        spacing: 6
        Label {
            Layout.fillWidth: true
            text: qsTr("Ask about the book as you read")
            color: theme.ink1
            font.pixelSize: 16
            font.weight: Font.DemiBold
            wrapMode: Text.Wrap
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("It never reads past your page. To answer, it needs a model: choose where it runs. You can change this any time.")
            color: theme.ink3
            font.pixelSize: 13
            wrapMode: Text.Wrap
            lineHeight: 1.5
        }
    }

    ButtonGroup { id: picks }
    ColumnLayout {
        Layout.fillWidth: true
        spacing: 8

        OptionCard {
            Layout.fillWidth: true
            ButtonGroup.group: picks
            title: qsTr("Gemini, with your key")
            description: qsTr("The best answers, including the whole book so far. Costs a little; you set the budget.")
            tone: theme.cloud
            checked: chooser.pick === "gemini"
            onClicked: chooser.selectPick("gemini")
        }
        OptionCard {
            Layout.fillWidth: true
            ButtonGroup.group: picks
            visible: chooser.localOffered
            title: qsTr("This computer")
            description: qsTr("Free and private. Short answers are quick here; long ones are better with Gemini.")
            tone: theme.positive
            checked: chooser.pick === "local"
            onClicked: chooser.selectPick("local")
        }
        OptionCard {
            Layout.fillWidth: true
            ButtonGroup.group: picks
            title: qsTr("Not now")
            description: qsTr("Keep reading without it.")
            tone: theme.ink4
            checked: chooser.pick === "later"
            onClicked: chooser.selectPick("later")
        }
    }

    // --- Step 1: Gemini with your key --------------------------------------
    ColumnLayout {
        visible: chooser.pick === "gemini"
        Layout.fillWidth: true
        Layout.topMargin: 4
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            Label {
                Layout.fillWidth: true
                text: qsTr("Your Gemini API key")
                color: theme.ink1
                font.pixelSize: 12
                font.weight: Font.Medium
            }
            AbstractButton {
                visible: keyField.text.length > 0
                implicitHeight: 20
                implicitWidth: revealLabel.implicitWidth + 8
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                onClicked: chooser.showKey = !chooser.showKey
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                background: Rectangle {
                    radius: Ui.radius(4, theme.radius)
                    color: parent.hovered ? theme.fillHover : "transparent"
                }
                contentItem: Label {
                    id: revealLabel
                    text: chooser.showKey ? qsTr("Hide") : qsTr("Show")
                    color: theme.ink3
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
        TextField {
            id: keyField
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            echoMode: chooser.showKey ? TextInput.Normal : TextInput.Password
            placeholderText: qsTr("Paste it here")
            onTextEdited: chooser.keyError = ""
            Keys.onReturnPressed: if (connectBtn.enabled) connectBtn.clicked()
            Keys.onEnterPressed: if (connectBtn.enabled) connectBtn.clicked()
        }
        Label {
            Layout.fillWidth: true
            textFormat: Text.StyledText
            text: {
                const link = "<a href=\"https://aistudio.google.com/apikey\">aistudio.google.com/apikey</a>"
                if (ai.unlimitedBudget)
                    return qsTr("Kept in the system keyring. No monthly cap is set; change it in Settings. Get a key at %1.").arg(link)
                if (ai.budget <= 0)
                    return qsTr("Kept in the system keyring. Cloud spending is paused ($0); change it in Settings. Get a key at %1.").arg(link)
                const cap = Number(ai.budget).toLocaleCurrencyString(Qt.locale("en_US"), "$")
                return qsTr("Kept in the system keyring. Monthly budget: %1, change it in Settings. Get a key at %2.").arg(cap).arg(link)
            }
            color: theme.ink4
            linkColor: theme.accentInk
            font.pixelSize: 11
            lineHeight: 1.4
            wrapMode: Text.Wrap
            onLinkActivated: (link) => Qt.openUrlExternally(link)
            HoverHandler { cursorShape: parent.hoveredLink ? Qt.PointingHandCursor : Qt.ArrowCursor }
        }
        TextButton {
            id: connectBtn
            Layout.fillWidth: true
            primary: true
            size: "large"
            text: ai.checking ? qsTr("Checking…") : qsTr("Connect")
            enabled: keyField.text.trim() !== "" && !ai.checking
            onClicked: {
                chooser.keyError = ""
                ai.setKey(keyField.text.trim())
            }
        }
        RowLayout {
            Layout.fillWidth: true
            visible: ai.checking
            spacing: 8
            BusyIndicator { running: parent.visible; implicitWidth: 16; implicitHeight: 16 }
            Label {
                Layout.fillWidth: true
                text: qsTr("Checking your key with Gemini…")
                color: theme.ink3
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
        }
        Rectangle {
            Layout.fillWidth: true
            visible: !ai.checking && chooser.keyError !== ""
            implicitHeight: keyErrLabel.implicitHeight + 16
            radius: Ui.radius(8, theme.radius)
            color: Ui.tint(theme.danger, 0.08)
            border.width: 1
            border.color: Ui.tint(theme.danger, 0.25)
            Label {
                id: keyErrLabel
                anchors.fill: parent
                anchors.margins: 8
                text: chooser.keyError
                color: theme.danger
                font.pixelSize: 12
                wrapMode: Text.Wrap
                lineHeight: 1.4
            }
        }
    }

    // --- Step 2: This computer (numbered 2-step checklist) -----------------
    ColumnLayout {
        visible: chooser.pick === "local"
        Layout.fillWidth: true
        Layout.topMargin: 4
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Rectangle {
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 1
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                radius: 10
                color: localSetup.running ? theme.positiveSoft : Ui.tint(theme.ink1, 0.10)
                Glyph {
                    visible: localSetup.running
                    anchors.centerIn: parent
                    width: 11
                    height: 11
                    stroke: 2.5
                    path: Icons.check
                    color: theme.positive
                }
                Label {
                    visible: !localSetup.running
                    anchors.centerIn: parent
                    text: "1"
                    color: theme.ink1
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                }
            }
            Label {
                Layout.fillWidth: true
                text: localSetup.running
                      ? qsTr("Ollama is running on this computer.")
                      : localSetup.installed
                      ? qsTr("Start Ollama. A terminal opens and asks for your password; Dogear never sees it.")
                      : qsTr("Install Ollama. A terminal opens and asks for your password; Dogear never sees it.")
                color: localSetup.running ? theme.ink3 : theme.ink2
                font.pixelSize: 13
                wrapMode: Text.Wrap
                lineHeight: 1.45
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 10
            Rectangle {
                Layout.alignment: Qt.AlignTop
                Layout.topMargin: 1
                Layout.preferredWidth: 20
                Layout.preferredHeight: 20
                radius: 10
                color: chooser.haveModel ? theme.positiveSoft : Ui.tint(theme.ink1, 0.10)
                Glyph {
                    visible: chooser.haveModel
                    anchors.centerIn: parent
                    width: 11
                    height: 11
                    stroke: 2.5
                    path: Icons.check
                    color: theme.positive
                }
                Label {
                    visible: !chooser.haveModel
                    anchors.centerIn: parent
                    text: "2"
                    color: theme.ink1
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                }
            }
            Label {
                Layout.fillWidth: true
                text: chooser.haveModel
                      ? qsTr("%1 is already installed on this computer.").arg(localSetup.textModel.title || "")
                      : qsTr("Download %1, chosen for this computer%2")
                            .arg(localSetup.textModel.title || "")
                            .arg(chooser.downloadSize !== "" ? " · " + chooser.downloadSize : "")
                color: theme.ink2
                font.pixelSize: 13
                wrapMode: Text.Wrap
                lineHeight: 1.45
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: localSetup.pulling
            spacing: 4
            ProgressBar {
                Layout.fillWidth: true
                value: localSetup.progress
            }
            Label {
                Layout.alignment: Qt.AlignRight
                text: qsTr("%1%").arg(Math.round(localSetup.progress * 100))
                color: theme.ink4
                font.pixelSize: 11
            }
        }

        Label {
            Layout.fillWidth: true
            visible: localSetup.waiting || localSetup.status !== "" || localSetup.problem !== ""
            text: localSetup.problem !== "" ? localSetup.problem
                : localSetup.status !== "" ? localSetup.status
                : qsTr("Finish in the terminal, and Dogear carries on by itself.")
            color: localSetup.problem !== "" ? theme.danger : theme.ink3
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            TextButton {
                Layout.fillWidth: true
                primary: true
                size: "large"
                enabled: !localSetup.waiting && !localSetup.pulling
                text: localSetup.running
                      ? (chooser.haveModel ? qsTr("Use %1").arg(localSetup.textModel.title || "")
                                           : qsTr("Download %1").arg(localSetup.textModel.title || ""))
                      : localSetup.installed ? qsTr("Start Ollama") : qsTr("Set up this computer")
                onClicked: localSetup.running
                           ? (chooser.haveModel ? localSetup.useExisting() : localSetup.download())
                           : localSetup.install()
            }
            TextButton {
                visible: localSetup.waiting || localSetup.pulling
                size: "large"
                text: localSetup.pulling ? qsTr("Stop") : qsTr("Cancel")
                onClicked: localSetup.cancel()
            }
        }
    }

    // --- Step 3: Not now ---------------------------------------------------
    ColumnLayout {
        visible: chooser.pick === "later"
        Layout.fillWidth: true
        Layout.topMargin: 4
        spacing: 10
        Label {
            Layout.fillWidth: true
            text: qsTr("Define still works without a model, from the dictionary on this computer. Everything else in Dogear is unchanged.")
            color: theme.ink2
            font.pixelSize: 13
            wrapMode: Text.Wrap
            lineHeight: 1.5
        }
        TextButton {
            Layout.fillWidth: true
            size: "large"
            text: qsTr("Back to the book")
            onClicked: chooser.notNow()
        }
    }

    Component.onCompleted: {
        pick = localOffered ? "local" : "gemini"
        localSetup.refresh()
    }
}
