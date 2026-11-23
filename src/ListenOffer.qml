import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The first Listen in a book with no voice yet: one voice made on this
// computer, chosen for the book's language, offered with its size. It
// downloads in place and the reading starts as soon as it's ready.
Rectangle {
    id: offer

    property var look
    property bool open: false
    readonly property var pack: open ? voicePacks.offer(reader.language) : ({})
    readonly property var voice: pack.voice || null
    property string pendingVoice: ""

    signal moreVoices()

    function show() { open = true }
    function close() {
        if (voicePacks.busy && pendingVoice !== "")
            voicePacks.cancel()
        pendingVoice = ""
        open = false
    }

    width: Math.min(420, parent.width - 48)
    height: column.implicitHeight + 36
    radius: Math.min(14, Math.max(theme.radius, 8))
    color: look.popover
    border.color: look.line
    visible: opacity > 0.01
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 140 } }

    Rectangle {
        z: -1
        anchors.fill: parent
        anchors.topMargin: 6
        anchors.bottomMargin: -6
        radius: parent.radius
        color: "black"
        opacity: offer.look.dark ? 0.35 : 0.12
    }
    MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }

    Connections {
        target: voicePacks
        function onReady(voiceId) {
            if (voiceId !== offer.pendingVoice)
                return
            offer.pendingVoice = ""
            const v = voicePacks.voice(voiceId)
            narration.setVoice(v.id, v.name)
            offer.open = false
            if (!narration.active && narration.hasPendingStart)
                narration.startPending()
        }
    }

    ColumnLayout {
        id: column
        x: 18
        y: 18
        width: parent.width - 36
        spacing: 10

        Label {
            Layout.fillWidth: true
            text: qsTr("Read aloud needs a voice")
            color: offer.look.fg
            font.pixelSize: 15
            font.weight: Font.DemiBold
        }
        Label {
            Layout.fillWidth: true
            visible: offer.voice !== null
            text: offer.voice ? qsTr("%1 reads %2 on this computer: free, private, and offline. It downloads %3 MB once.")
                                    .arg(offer.voice.name).arg(offer.voice.languageName)
                                    .arg(Math.round((offer.pack.bytes || 0) / 1e6).toLocaleString(Qt.locale(), 'f', 0)) : ""
            color: offer.look.muted
            font.pixelSize: 13
            wrapMode: Text.Wrap
            lineHeight: 1.25
        }
        Badge {
            visible: offer.voice !== null
            where: "local"
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: voicePacks.busy && offer.pendingVoice !== ""
            spacing: 6
            ProgressBar {
                Layout.fillWidth: true
                value: voicePacks.progress
                indeterminate: voicePacks.progress >= 1.0
            }
            Label {
                text: voicePacks.progress >= 1.0 ? qsTr("Getting it ready…")
                    : qsTr("Downloading… %1%").arg(Math.round(voicePacks.progress * 100).toLocaleString(Qt.locale(), 'f', 0))
                color: offer.look.muted
                font.pixelSize: 12
            }
        }
        Label {
            Layout.fillWidth: true
            visible: voicePacks.problem !== "" && offer.pendingVoice === ""
            text: voicePacks.problem
            color: theme.danger
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.topMargin: 4
            spacing: 8
            TextButton {
                visible: offer.voice !== null && offer.pendingVoice === ""
                primary: true
                text: qsTr("Download & listen")
                onClicked: {
                    offer.pendingVoice = offer.voice.id
                    voicePacks.installVoice(offer.voice.id)
                }
            }
            TextButton {
                visible: offer.pendingVoice === ""
                text: qsTr("More voices…")
                onClicked: { offer.open = false; offer.moreVoices() }
            }
            Item { Layout.fillWidth: true }
            TextButton {
                text: offer.pendingVoice !== "" ? qsTr("Stop") : qsTr("Not now")
                onClicked: offer.close()
            }
        }
    }
}
