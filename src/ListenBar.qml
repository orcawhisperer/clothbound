import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Icons.js" as Icons
import "Ui.js" as Ui

// The floating listening deck: always-dark warm charcoal so it stays calm and
// legible over Paper, Sepia, Dusk, Night, or Omarchy pages. Row 1 pairs the
// book cover, title, clickable narrator + provenance, centered transport
// controls (with ±15s skip & sentence stepping), speed, sleep timer (with 10s
// fade), background listen, and stop. Row 2 scrubs the chapter's audio clock.
Rectangle {
    id: bar

    property var look
    readonly property bool menuOpen: sleepMenu.opened

    signal voiceRequested()
    signal followRequested()

    // Always-dark warm charcoal palette matching 03_listening.html.
    readonly property color deckBg: Qt.rgba(28 / 255, 27 / 255, 25 / 255, 0.94)
    readonly property color deckFg: "#F2EFE9"
    readonly property color deckSub: "#B9B3A8"
    readonly property color deckSoft: "#CFC9BE"
    readonly property color deckMuted: "#A9A296"
    readonly property color deckRim: (look && look.dark) ? Qt.rgba(1, 1, 1, 0.12) : Qt.rgba(0, 0, 0, 0.25)
    readonly property color deckFill: Qt.rgba(1, 1, 1, 0.08)
    readonly property color deckHover: Qt.rgba(1, 1, 1, 0.14)
    readonly property color deckPressed: Qt.rgba(1, 1, 1, 0.20)
    readonly property color deckMenuBg: "#1F1E1C"

    readonly property bool voiceIsBuiltin: narration.voice.startsWith("builtin:")
    readonly property bool voiceIsStudio: narration.voice.startsWith("studio:")
    readonly property bool voiceIsLocal: voiceIsBuiltin || (voiceIsStudio && ai.localIsLoopback)
    readonly property string voiceAccent: {
        if (narration.voiceAccent !== "")
            return narration.voiceAccent
        if (voices.count >= 0 && narration.voice !== "") {
            const v = voices.voice(narration.voice)
            if (v && v.accent)
                return v.accent
        }
        return ""
    }
    readonly property string provenanceLabel: voiceIsLocal ? qsTr("On this computer")
                                            : voiceIsStudio ? qsTr("Your server")
                                            : qsTr("Sent to Google")

    readonly property real rateSafe: Math.max(0.5, narration.rate)
    readonly property real totalSec: narration.durationUs > 0 ? (narration.durationUs / 1000000.0) / rateSafe : 0
    readonly property real elapsedSec: narration.positionUs > 0 ? (narration.positionUs / 1000000.0) / rateSafe : 0
    readonly property real playbackFrac: narration.durationUs > 0
                                       ? Math.max(0, Math.min(1, narration.positionUs / narration.durationUs)) : 0
    readonly property bool hasStatusBanner: narration.problem !== "" || narration.preparing
                                          || narration.fallbackNotice !== "" || narration.fallbackVoice !== ""
                                          || narration.offerOfflineSetup

    function formatClock(secs) {
        const total = Math.max(0, Math.floor(secs))
        const h = Math.floor(total / 3600)
        const m = Math.floor((total % 3600) / 60)
        const s = total % 60
        const ss = s < 10 ? "0" + s : String(s)
        if (h > 0)
            return h + ":" + (m < 10 ? "0" + m : String(m)) + ":" + ss
        return m + ":" + ss
    }

    // Keep child controls (BusyIndicator, Popup, MenuRow, ToolTip) dark-native.
    palette.window: deckMenuBg
    palette.windowText: deckFg
    palette.base: deckFill
    palette.alternateBase: deckHover
    palette.text: deckFg
    palette.button: deckFill
    palette.buttonText: deckFg
    palette.mid: deckSub
    palette.midlight: Qt.rgba(1, 1, 1, 0.08)
    palette.dark: Qt.rgba(1, 1, 1, 0.14)
    palette.light: deckMenuBg
    palette.highlight: theme.accent
    palette.highlightedText: theme.accentOn
    palette.link: theme.accent
    palette.linkVisited: theme.accentSoft
    palette.toolTipBase: deckMenuBg
    palette.toolTipText: deckFg

    width: Math.max(360, Math.min(720, (parent ? parent.width : 1280) - 48))
    height: deckColumn.implicitHeight + 20
    radius: Ui.radius(16, theme.radius)
    color: deckBg
    border.width: 1
    border.color: deckRim
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Now playing")

    // Soft layered shadow so the dark deck floats calmly above the page.
    Rectangle {
        z: -2
        anchors.fill: parent
        anchors.topMargin: 12
        anchors.bottomMargin: -10
        anchors.leftMargin: -4
        anchors.rightMargin: -4
        radius: bar.radius > 0 ? bar.radius + 4 : 0
        color: Qt.rgba(40 / 255, 30 / 255, 10 / 255, 0.22)
    }
    Rectangle {
        z: -1
        anchors.fill: parent
        anchors.topMargin: 5
        anchors.bottomMargin: -5
        radius: bar.radius
        color: Qt.rgba(0, 0, 0, 0.28)
    }

    // Keep pointer clicks and scroll wheel off the reading surface underneath.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    ColumnLayout {
        id: deckColumn
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: 10
        anchors.leftMargin: 12
        anchors.rightMargin: 10
        spacing: 8

        // --- Row 1: Book/Narrator · Transport · Speed/Sleep/Background/Stop ---
        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            // Left zone: 32×46 mini cover + book title + clickable narrator/provenance.
            RowLayout {
                Layout.preferredWidth: Math.min(254, Math.max(136, Math.round(bar.width * 0.35)))
                Layout.maximumWidth: 264
                spacing: 10

                Item {
                    visible: bar.width >= 470
                    Layout.preferredWidth: 32
                    Layout.preferredHeight: 46
                    Layout.alignment: Qt.AlignVCenter

                    CoverImage {
                        anchors.fill: parent
                        source: reader.bookId > 0 ? library.coverUrl(reader.bookId) : ""
                        title: reader.title
                        author: reader.author
                        radius: Ui.radius(3, theme.radius)
                    }
                    // Subtle spine crease on the left edge.
                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 2
                        color: Qt.rgba(0, 0, 0, 0.30)
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 3

                    Label {
                        Layout.fillWidth: true
                        text: reader.title
                        color: bar.deckFg
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }

                    AbstractButton {
                        id: voiceButton
                        Layout.fillWidth: true
                        implicitHeight: 20
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: narration.voiceName
                                         + (bar.voiceAccent !== "" ? ", " + bar.voiceAccent : "")
                                         + ", " + bar.provenanceLabel
                        onClicked: bar.voiceRequested()

                        background: Rectangle {
                            x: -4
                            y: -2
                            width: Math.min(voiceButton.width + 8, voiceMetaRow.implicitWidth + 8)
                            height: voiceButton.height + 4
                            radius: Ui.radius(5, theme.radius)
                            color: voiceButton.pressed ? bar.deckHover
                                 : voiceButton.hovered ? bar.deckFill : "transparent"
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -2
                                visible: voiceButton.visualFocus
                                radius: Ui.radius(7, theme.radius)
                                color: "transparent"
                                border.width: 2
                                border.color: theme.accent
                            }
                        }

                        contentItem: RowLayout {
                            id: voiceMetaRow
                            spacing: 6

                            Label {
                                Layout.fillWidth: true
                                Layout.maximumWidth: implicitWidth
                                text: narration.voiceName
                                      + (bar.voiceAccent !== "" ? " · " + bar.voiceAccent : "")
                                color: voiceButton.hovered ? bar.deckFg : bar.deckSub
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }

                            Rectangle {
                                visible: narration.voiceName !== "" && bar.width >= 540
                                Layout.alignment: Qt.AlignVCenter
                                implicitWidth: provText.implicitWidth + 12
                                implicitHeight: 17
                                radius: 9
                                color: bar.voiceIsLocal || bar.voiceIsStudio ? bar.deckFill : theme.cloudSoft
                                Label {
                                    id: provText
                                    anchors.centerIn: parent
                                    text: bar.provenanceLabel
                                    color: bar.voiceIsLocal || bar.voiceIsStudio ? bar.deckSoft : theme.cloudInk
                                    font.pixelSize: 10
                                    font.weight: Font.DemiBold
                                }
                            }
                        }

                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Choose a narrator (V)")
                        ToolTip.delay: 600
                    }
                }
            }

            // Center zone: −15s · Prev sentence · 44×44 Play/Pause · Next sentence · +15s.
            Item {
                Layout.fillWidth: true
                implicitHeight: 44
                implicitWidth: transportRow.implicitWidth

                Row {
                    id: transportRow
                    anchors.centerIn: parent
                    spacing: 4

                    AbstractButton {
                        id: back15Btn
                        visible: bar.width >= 560
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: back15Label.implicitWidth + 10
                        implicitHeight: 30
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: qsTr("Back 15 seconds")
                        onClicked: narration.skipSeconds(-15)
                        background: Rectangle {
                            radius: Ui.radius(7, theme.radius)
                            color: back15Btn.pressed ? bar.deckPressed
                                 : back15Btn.hovered ? bar.deckFill : "transparent"
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -2
                                visible: back15Btn.visualFocus
                                radius: Ui.radius(9, theme.radius)
                                color: "transparent"
                                border.width: 2
                                border.color: theme.accent
                            }
                        }
                        contentItem: Label {
                            id: back15Label
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            text: "−15"
                            color: back15Btn.hovered ? bar.deckFg : bar.deckMuted
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.features: ({ "tnum": 1 })
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Back 15 seconds (⇧←)")
                        ToolTip.delay: 600
                    }

                    AbstractButton {
                        id: prevBtn
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: 36
                        implicitHeight: 36
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: qsTr("Previous sentence")
                        onClicked: narration.prevSentence()
                        background: Rectangle {
                            radius: 18
                            color: prevBtn.pressed ? bar.deckPressed
                                 : prevBtn.hovered ? bar.deckFill : "transparent"
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -2
                                visible: prevBtn.visualFocus
                                radius: 20
                                color: "transparent"
                                border.width: 2
                                border.color: theme.accent
                            }
                        }
                        contentItem: Item {
                            Glyph {
                                anchors.centerIn: parent
                                width: 18
                                height: 18
                                path: Icons.skipBackSolid
                                filled: true
                                color: bar.deckFg
                            }
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Previous sentence")
                        ToolTip.delay: 600
                    }

                    AbstractButton {
                        id: playBtn
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: 44
                        implicitHeight: 44
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: narration.playing ? qsTr("Pause") : qsTr("Play")
                        onClicked: narration.toggle()
                        background: Rectangle {
                            radius: 22
                            color: playBtn.pressed ? Qt.darker(theme.accent, 1.1)
                                 : playBtn.hovered ? Qt.lighter(theme.accent, 1.07) : theme.accent
                            Behavior on color { ColorAnimation { duration: 100 } }
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -3
                                visible: playBtn.visualFocus
                                radius: 25
                                color: "transparent"
                                border.width: 2
                                border.color: bar.deckFg
                            }
                        }
                        contentItem: Item {
                            Glyph {
                                anchors.centerIn: parent
                                anchors.horizontalCenterOffset: narration.playing ? 0 : 1
                                width: 18
                                height: 18
                                visible: !(narration.playing && (narration.buffering || narration.preparing))
                                path: narration.playing ? Icons.pauseSolid : Icons.playSolid
                                filled: true
                                color: theme.accentOn
                            }
                            BusyIndicator {
                                anchors.centerIn: parent
                                visible: (narration.buffering || narration.preparing) && narration.playing
                                running: visible
                                implicitWidth: 26
                                implicitHeight: 26
                            }
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: narration.playing ? qsTr("Pause (P)") : qsTr("Play (P)")
                        ToolTip.delay: 600
                    }

                    AbstractButton {
                        id: nextBtn
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: 36
                        implicitHeight: 36
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: qsTr("Next sentence")
                        onClicked: narration.nextSentence()
                        background: Rectangle {
                            radius: 18
                            color: nextBtn.pressed ? bar.deckPressed
                                 : nextBtn.hovered ? bar.deckFill : "transparent"
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -2
                                visible: nextBtn.visualFocus
                                radius: 20
                                color: "transparent"
                                border.width: 2
                                border.color: theme.accent
                            }
                        }
                        contentItem: Item {
                            Glyph {
                                anchors.centerIn: parent
                                width: 18
                                height: 18
                                path: Icons.skipForwardSolid
                                filled: true
                                color: bar.deckFg
                            }
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Next sentence")
                        ToolTip.delay: 600
                    }

                    AbstractButton {
                        id: fwd15Btn
                        visible: bar.width >= 560
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: fwd15Label.implicitWidth + 10
                        implicitHeight: 30
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: qsTr("Forward 15 seconds")
                        onClicked: narration.skipSeconds(15)
                        background: Rectangle {
                            radius: Ui.radius(7, theme.radius)
                            color: fwd15Btn.pressed ? bar.deckPressed
                                 : fwd15Btn.hovered ? bar.deckFill : "transparent"
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -2
                                visible: fwd15Btn.visualFocus
                                radius: Ui.radius(9, theme.radius)
                                color: "transparent"
                                border.width: 2
                                border.color: theme.accent
                            }
                        }
                        contentItem: Label {
                            id: fwd15Label
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            text: "+15"
                            color: fwd15Btn.hovered ? bar.deckFg : bar.deckMuted
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.features: ({ "tnum": 1 })
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Forward 15 seconds (⇧→)")
                        ToolTip.delay: 600
                    }
                }
            }

            // Right zone: Speed pill · Sleep timer · Background tray · Stop ×.
            Item {
                Layout.preferredWidth: Math.min(216, Math.max(rightRow.implicitWidth, Math.round(bar.width * 0.29)))
                implicitHeight: 30
                implicitWidth: rightRow.implicitWidth

                Row {
                    id: rightRow
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    AbstractButton {
                        id: rateButton
                        anchors.verticalCenter: parent.verticalCenter
                        implicitHeight: 30
                        implicitWidth: rateLabel.implicitWidth + 18
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: qsTr("Speed %1").arg(rateLabel.text)
                        background: Rectangle {
                            radius: Ui.radius(7, theme.radius)
                            color: rateButton.pressed ? bar.deckPressed
                                 : rateButton.hovered ? bar.deckHover : Qt.rgba(1, 1, 1, 0.07)
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -2
                                visible: rateButton.visualFocus
                                radius: Ui.radius(9, theme.radius)
                                color: "transparent"
                                border.width: 2
                                border.color: theme.accent
                            }
                        }
                        contentItem: Label {
                            id: rateLabel
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            text: Number(narration.rate).toLocaleString(Qt.locale(), 'f', narration.rate * 20 % 2 ? 2 : 1) + "×"
                            color: bar.deckFg
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            font.features: ({ "tnum": 1 })
                        }
                        onClicked: {
                            const steps = [1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 0.75, 0.9]
                            const at = steps.findIndex(step => Math.abs(step - narration.rate) < 0.01)
                            narration.rate = steps[(at + 1) % steps.length]
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Speed (< and >)")
                        ToolTip.delay: 600
                    }

                    AbstractButton {
                        id: sleepButton
                        anchors.verticalCenter: parent.verticalCenter
                        readonly property bool timerActive: narration.sleepSecondsLeft > 0 || narration.sleepAtChapterEnd
                        implicitHeight: 30
                        implicitWidth: timerActive ? sleepRow.implicitWidth + 16 : 30
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: sleepButton.ToolTip.text
                        background: Rectangle {
                            radius: Ui.radius(7, theme.radius)
                            color: narration.sleepFading ? theme.cloudSoft
                                 : sleepButton.pressed ? bar.deckPressed
                                 : sleepButton.hovered ? bar.deckFill : "transparent"
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -2
                                visible: sleepButton.visualFocus
                                radius: Ui.radius(9, theme.radius)
                                color: "transparent"
                                border.width: 2
                                border.color: theme.accent
                            }
                        }
                        contentItem: Row {
                            id: sleepRow
                            anchors.centerIn: parent
                            spacing: 5
                            Glyph {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 15
                                height: 15
                                path: Icons.moon
                                color: narration.sleepFading ? theme.cloudInk
                                     : sleepButton.timerActive ? theme.accent : bar.deckSoft
                            }
                            Label {
                                anchors.verticalCenter: parent.verticalCenter
                                visible: sleepButton.timerActive
                                text: narration.sleepSecondsLeft > 0
                                      ? bar.formatClock(narration.sleepSecondsLeft)
                                      : qsTr("Chapter")
                                color: narration.sleepFading ? theme.cloudInk
                                     : bar.deckSoft
                                font.pixelSize: 12
                                font.weight: narration.sleepSecondsLeft > 0 ? Font.DemiBold : Font.Normal
                                font.features: ({ "tnum": 1 })
                            }
                        }
                        onClicked: sleepMenu.opened ? sleepMenu.close() : sleepMenu.open()
                        ToolTip.visible: hovered
                        ToolTip.text: narration.sleepFading ? qsTr("Fading out… %1").arg(bar.formatClock(narration.sleepSecondsLeft))
                                    : narration.sleepSecondsLeft > 0 ? qsTr("Stopping in %1").arg(bar.formatClock(narration.sleepSecondsLeft))
                                    : narration.sleepAtChapterEnd ? qsTr("Sleep timer: end of chapter") : qsTr("Sleep timer")
                        ToolTip.delay: 600

                        Popup {
                            id: sleepMenu
                            y: -height - 12
                            x: Math.max(-sleepButton.x, Math.min(sleepButton.width - width, sleepButton.width / 2 - width / 2))
                            width: 224
                            padding: 6
                            background: Rectangle {
                                radius: Ui.radius(10, theme.radius)
                                color: bar.deckMenuBg
                                border.width: 1
                                border.color: Qt.rgba(1, 1, 1, 0.10)
                            }
                            contentItem: Column {
                                spacing: 2
                                Repeater {
                                    model: [5, 15, 30, 60]
                                    delegate: MenuRow {
                                        required property int modelData
                                        text: qsTr("In %n minutes", "", modelData)
                                        onClicked: { narration.sleepIn(modelData); sleepMenu.close() }
                                    }
                                }
                                MenuRow {
                                    text: qsTr("At the end of the chapter")
                                    checked: narration.sleepAtChapterEnd
                                    onClicked: { narration.sleepAtEndOfChapter(); sleepMenu.close() }
                                }
                                MenuRow {
                                    text: qsTr("Off")
                                    checked: narration.sleepSecondsLeft === 0 && !narration.sleepAtChapterEnd
                                    onClicked: { narration.sleepIn(0); sleepMenu.close() }
                                }
                            }
                        }
                    }

                    AbstractButton {
                        id: trayBtn
                        visible: bar.width >= 430
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: 30
                        implicitHeight: 30
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: qsTr("Keep listening in the background")
                        onClicked: win.listenInBackground()
                        background: Rectangle {
                            radius: Ui.radius(7, theme.radius)
                            color: trayBtn.pressed ? bar.deckPressed
                                 : trayBtn.hovered ? bar.deckFill : "transparent"
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -2
                                visible: trayBtn.visualFocus
                                radius: Ui.radius(9, theme.radius)
                                color: "transparent"
                                border.width: 2
                                border.color: theme.accent
                            }
                        }
                        contentItem: Item {
                            Glyph {
                                anchors.centerIn: parent
                                width: 15
                                height: 15
                                path: Icons.tray
                                color: trayBtn.hovered ? bar.deckFg : bar.deckSoft
                            }
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Keep listening in the background")
                        ToolTip.delay: 600
                    }

                    AbstractButton {
                        id: stopBtn
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: 30
                        implicitHeight: 30
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: qsTr("Stop listening")
                        onClicked: narration.stop()
                        background: Rectangle {
                            radius: Ui.radius(7, theme.radius)
                            color: stopBtn.pressed ? bar.deckPressed
                                 : stopBtn.hovered ? bar.deckFill : "transparent"
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -2
                                visible: stopBtn.visualFocus
                                radius: Ui.radius(9, theme.radius)
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
                                stroke: 2.0
                                path: Icons.close
                                color: stopBtn.hovered ? bar.deckFg : bar.deckSoft
                            }
                        }
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Stop listening (⇧P)")
                        ToolTip.delay: 600
                    }
                }
            }
        }

        // --- Row 2: Interactive Chapter Audio Timeline & Scrubber ---
        RowLayout {
            id: timelineRow
            Layout.fillWidth: true
            Layout.leftMargin: 2
            Layout.rightMargin: 2
            spacing: 10

            readonly property real displayFrac: scrubber.dragging ? scrubber.dragFrac : bar.playbackFrac
            readonly property real shownElapsed: scrubber.dragging ? scrubber.dragFrac * bar.totalSec : bar.elapsedSec
            readonly property real shownRemaining: Math.max(0, bar.totalSec - shownElapsed)
            readonly property string chapterName: reader.titleOfChapter(narration.chapter)

            Label {
                text: bar.formatClock(timelineRow.shownElapsed)
                color: bar.deckMuted
                font.pixelSize: 11
                font.features: ({ "tnum": 1 })
            }

            Item {
                id: scrubber
                Layout.fillWidth: true
                implicitHeight: 16
                property bool dragging: false
                property real dragFrac: 0

                function fracAt(px) {
                    return width > 0 ? Math.max(0, Math.min(1, px / width)) : 0
                }

                Rectangle {
                    id: track
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    height: scrubMouse.containsMouse || scrubber.dragging ? 4 : 3
                    radius: height / 2
                    color: Qt.rgba(1, 1, 1, 0.12)

                    Rectangle {
                        width: parent.width * timelineRow.displayFrac
                        height: parent.height
                        radius: parent.radius
                        color: theme.accent
                    }
                }

                Rectangle {
                    visible: narration.durationUs > 0 && (scrubMouse.containsMouse || scrubber.dragging)
                    x: Math.max(0, Math.min(parent.width - width, parent.width * timelineRow.displayFrac - width / 2))
                    anchors.verticalCenter: parent.verticalCenter
                    width: 10
                    height: 10
                    radius: 5
                    color: bar.deckFg
                    border.width: 2
                    border.color: theme.accent
                }

                MouseArea {
                    id: scrubMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    enabled: narration.durationUs > 0
                    cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onPressed: (mouse) => {
                        scrubber.dragging = true
                        scrubber.dragFrac = scrubber.fracAt(mouse.x)
                    }
                    onPositionChanged: (mouse) => {
                        if (scrubber.dragging)
                            scrubber.dragFrac = scrubber.fracAt(mouse.x)
                    }
                    onReleased: (mouse) => {
                        if (!scrubber.dragging)
                            return
                        const frac = scrubber.fracAt(mouse.x)
                        scrubber.dragging = false
                        if (narration.durationUs > 0)
                            narration.seekToUs(Math.round(frac * narration.durationUs))
                    }
                    onCanceled: scrubber.dragging = false
                }
            }

            Label {
                Layout.maximumWidth: Math.max(90, bar.width * 0.38)
                text: (bar.totalSec > 0 ? "−" + bar.formatClock(timelineRow.shownRemaining) : "−0:00")
                      + (timelineRow.chapterName !== "" ? " · " + timelineRow.chapterName : "")
                color: bar.deckMuted
                font.pixelSize: 11
                font.features: ({ "tnum": 1 })
                elide: Text.ElideRight
            }
        }

        // --- Conditional Row 3: Honest Status / Preparing / Offline Fallback ---
        RowLayout {
            visible: bar.hasStatusBanner
            Layout.fillWidth: true
            Layout.leftMargin: 2
            Layout.rightMargin: 2
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: narration.problem !== "" ? narration.problem
                    : narration.preparing ? qsTr("Preparing ahead · %1%").arg(narration.preparedPercent)
                    : narration.fallbackNotice
                color: narration.problem !== "" ? theme.cloudInk : bar.deckSub
                font.pixelSize: 12
                elide: Text.ElideRight
            }
            TextButton {
                visible: narration.fallbackVoice !== ""
                size: "compact"
                text: qsTr("Use %1 offline").arg(narration.fallbackVoiceName)
                onClicked: narration.useFallbackVoice()
            }
            TextButton {
                visible: narration.fallbackVoice === "" && narration.offerOfflineSetup
                size: "compact"
                text: qsTr("Offline voices")
                onClicked: bar.voiceRequested()
            }
        }
    }
}
