import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Icons.js" as Icons
import "Ui.js" as Ui

// The floating listening deck: always-dark warm charcoal so it stays calm and
// legible over Paper, Sepia, Dusk, Night, or Omarchy pages. Row 1 pairs the
// book cover, title, clickable narrator + provenance, centered transport
// controls (with ±15s skip & sentence stepping), speed popover, sleep timer
// (with 10s fade indicator), background listen, and stop. Row 2 scrubs the
// chapter's audio clock with zero layout jitter.
Rectangle {
    id: bar

    property var look
    readonly property bool menuOpen: sleepMenu.opened || speedMenu.opened

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
    function formatRate(rate) {
        const rounded = Math.round(rate * 100) / 100
        const decimals = Math.round(rounded * 100) % 10 !== 0 ? 2 : 1
        return Number(rounded).toLocaleString(Qt.locale(), 'f', decimals) + "×"
    }

    onVisibleChanged: {
        if (!visible) {
            speedMenu.close()
            sleepMenu.close()
            scrubber.dragging = false
        }
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

        // --- Row 1: Book/Narrator · Centered Transport · Speed/Sleep/Background/Stop ---
        RowLayout {
            Layout.fillWidth: true
            spacing: 10

            // Left zone: 32×46 mini cover + book title + clickable narrator switcher.
            // Equal weight with Right zone keeps Play/Pause dead-center with zero jitter.
            RowLayout {
                Layout.fillWidth: true
                Layout.minimumWidth: 108
                Layout.preferredWidth: 1
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
                    Layout.minimumWidth: 0
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
                        onClicked: {
                            speedMenu.close()
                            sleepMenu.close()
                            bar.voiceRequested()
                        }

                        readonly property real contentNaturalW:
                            voiceNameLabel.implicitWidth
                            + 4 + voiceChevron.width
                            + (provPill.visible ? 6 + provPill.implicitWidth : 0)

                        background: Rectangle {
                            x: -4
                            y: -2
                            width: Math.min(voiceButton.width + 8, voiceButton.contentNaturalW + 8)
                            height: voiceButton.height + 4
                            radius: Ui.radius(5, theme.radius)
                            color: voiceButton.pressed ? bar.deckHover
                                 : voiceButton.hovered ? bar.deckFill : "transparent"
                            Behavior on color { ColorAnimation { duration: 100 } }
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

                        contentItem: Row {
                            id: voiceMetaRow
                            spacing: 5

                            Label {
                                id: voiceNameLabel
                                anchors.verticalCenter: parent.verticalCenter
                                readonly property real maxTextW: Math.max(24, voiceButton.width
                                    - voiceChevron.width - 5
                                    - (provPill.visible ? provPill.implicitWidth + 5 : 0))
                                width: Math.min(implicitWidth, maxTextW)
                                text: narration.voiceName
                                      + (bar.voiceAccent !== "" ? " · " + bar.voiceAccent : "")
                                color: voiceButton.hovered ? bar.deckFg : bar.deckSub
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }

                            Glyph {
                                id: voiceChevron
                                anchors.verticalCenter: parent.verticalCenter
                                width: 11
                                height: 11
                                stroke: 2.0
                                path: Icons.chevronsUpDown
                                color: voiceButton.hovered ? bar.deckSoft : bar.deckMuted
                            }

                            Rectangle {
                                id: provPill
                                visible: narration.voiceName !== "" && bar.width >= 570
                                anchors.verticalCenter: parent.verticalCenter
                                implicitWidth: provText.implicitWidth + 10
                                width: implicitWidth
                                height: 17
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
                Layout.fillWidth: false
                Layout.preferredWidth: transportRow.implicitWidth
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
                        implicitWidth: 32
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
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            text: "−15"
                            color: back15Btn.hovered ? bar.deckFg : bar.deckMuted
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.features: ({ "tnum": 1 })
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
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
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
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
                        scale: playBtn.pressed && !theme.reducedMotion ? 0.94 : 1.0
                        Behavior on scale {
                            enabled: !theme.reducedMotion
                            NumberAnimation { duration: 90; easing.type: Easing.OutCubic }
                        }
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
                                implicitWidth: 24
                                implicitHeight: 24
                                // Contrast cleanly on top of the solid theme.accent circle.
                                palette.highlight: theme.accentOn
                            }
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
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
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Next sentence")
                        ToolTip.delay: 600
                    }

                    AbstractButton {
                        id: fwd15Btn
                        visible: bar.width >= 560
                        anchors.verticalCenter: parent.verticalCenter
                        implicitWidth: 32
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
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            text: "+15"
                            color: fwd15Btn.hovered ? bar.deckFg : bar.deckMuted
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                            font.features: ({ "tnum": 1 })
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Forward 15 seconds (⇧→)")
                        ToolTip.delay: 600
                    }
                }
            }

            // Right zone: Speed popover pill · Sleep timer · Background tray · Stop ×.
            Item {
                Layout.fillWidth: true
                Layout.minimumWidth: rightRow.implicitWidth
                Layout.preferredWidth: 1
                implicitHeight: 30
                implicitWidth: rightRow.implicitWidth

                Row {
                    id: rightRow
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 3

                    AbstractButton {
                        id: rateButton
                        anchors.verticalCenter: parent.verticalCenter
                        implicitHeight: 30
                        // Pinned width so 1.0× ↔ 1.25× never jitters the row.
                        implicitWidth: 48
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: qsTr("Speed %1").arg(rateLabel.text)
                        background: Rectangle {
                            radius: Ui.radius(7, theme.radius)
                            color: speedMenu.opened ? bar.deckHover
                                 : rateButton.pressed ? bar.deckPressed
                                 : rateButton.hovered ? bar.deckHover : Qt.rgba(1, 1, 1, 0.07)
                            border.width: speedMenu.opened ? 1 : 0
                            border.color: Qt.rgba(1, 1, 1, 0.14)
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
                            text: bar.formatRate(narration.rate)
                            color: bar.deckFg
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            font.features: ({ "tnum": 1 })
                        }
                        onClicked: {
                            sleepMenu.close()
                            speedMenu.opened ? speedMenu.close() : speedMenu.open()
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered && !speedMenu.opened
                        ToolTip.text: qsTr("Playback speed (< and >)")
                        ToolTip.delay: 600

                        Popup {
                            id: speedMenu
                            y: -height - 12
                            x: Math.max(-rateButton.x, Math.min(rateButton.width - width, rateButton.width / 2 - width / 2))
                            width: 196
                            padding: 6
                            background: Rectangle {
                                radius: Ui.radius(10, theme.radius)
                                color: bar.deckMenuBg
                                border.width: 1
                                border.color: Qt.rgba(1, 1, 1, 0.12)
                                Rectangle {
                                    z: -1
                                    anchors.fill: parent
                                    anchors.topMargin: 6
                                    anchors.bottomMargin: -6
                                    anchors.leftMargin: -3
                                    anchors.rightMargin: -3
                                    radius: parent.radius > 0 ? parent.radius + 2 : 0
                                    color: Qt.rgba(0, 0, 0, 0.32)
                                }
                            }
                            contentItem: Column {
                                spacing: 2

                                // Fine ±0.05× stepper row for exact pacing.
                                RowLayout {
                                    width: parent.width
                                    spacing: 4

                                    AbstractButton {
                                        id: slowerBtn
                                        Layout.preferredWidth: 32
                                        Layout.preferredHeight: 28
                                        enabled: narration.rate > 0.755
                                        hoverEnabled: true
                                        focusPolicy: Qt.TabFocus
                                        opacity: enabled ? 1 : 0.35
                                        Accessible.name: qsTr("Slower")
                                        onClicked: narration.rate = narration.rate - 0.05
                                        background: Rectangle {
                                            radius: Ui.radius(6, theme.radius)
                                            color: slowerBtn.pressed ? bar.deckPressed
                                                 : slowerBtn.hovered ? bar.deckHover : bar.deckFill
                                        }
                                        contentItem: Label {
                                            text: "−"
                                            color: bar.deckFg
                                            font.pixelSize: 15
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        horizontalAlignment: Text.AlignHCenter
                                        text: bar.formatRate(narration.rate)
                                        color: bar.deckFg
                                        font.pixelSize: 13
                                        font.weight: Font.DemiBold
                                        font.features: ({ "tnum": 1 })
                                    }
                                    AbstractButton {
                                        id: fasterBtn
                                        Layout.preferredWidth: 32
                                        Layout.preferredHeight: 28
                                        enabled: narration.rate < 1.995
                                        hoverEnabled: true
                                        focusPolicy: Qt.TabFocus
                                        opacity: enabled ? 1 : 0.35
                                        Accessible.name: qsTr("Faster")
                                        onClicked: narration.rate = narration.rate + 0.05
                                        background: Rectangle {
                                            radius: Ui.radius(6, theme.radius)
                                            color: fasterBtn.pressed ? bar.deckPressed
                                                 : fasterBtn.hovered ? bar.deckHover : bar.deckFill
                                        }
                                        contentItem: Label {
                                            text: "+"
                                            color: bar.deckFg
                                            font.pixelSize: 15
                                            horizontalAlignment: Text.AlignHCenter
                                            verticalAlignment: Text.AlignVCenter
                                        }
                                    }
                                }

                                Rectangle {
                                    width: parent.width
                                    height: 1
                                    color: Qt.rgba(1, 1, 1, 0.08)
                                }

                                Repeater {
                                    model: [0.75, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0]
                                    delegate: MenuRow {
                                        required property real modelData
                                        text: bar.formatRate(modelData)
                                        hint: Math.abs(modelData - 1.0) < 0.01 ? qsTr("Normal") : ""
                                        checked: Math.abs(narration.rate - modelData) < 0.015
                                        onClicked: {
                                            narration.rate = modelData
                                            speedMenu.close()
                                        }
                                    }
                                }
                            }
                        }
                    }

                    AbstractButton {
                        id: sleepButton
                        anchors.verticalCenter: parent.verticalCenter
                        readonly property bool timerActive: narration.sleepSecondsLeft > 0 || narration.sleepAtChapterEnd
                        implicitHeight: 30
                        // Stable active width prevents jitter as countdown ticks 10:00 → 9:59 → 0:09.
                        implicitWidth: timerActive ? Math.max(66, sleepRow.implicitWidth + 16) : 30
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: sleepButton.ToolTip.text
                        background: Rectangle {
                            radius: Ui.radius(7, theme.radius)
                            clip: true
                            color: narration.sleepFading ? theme.cloudSoft
                                 : sleepMenu.opened ? bar.deckHover
                                 : sleepButton.pressed ? bar.deckPressed
                                 : sleepButton.hovered ? bar.deckFill : "transparent"
                            border.width: narration.sleepFading ? 1 : 0
                            border.color: Ui.tint(theme.cloud, 0.40)

                            // 10-second fade-out progress bar draining along the bottom edge.
                            Rectangle {
                                visible: narration.sleepFading
                                anchors.left: parent.left
                                anchors.bottom: parent.bottom
                                height: 2
                                width: parent.width * Math.max(0, Math.min(1, narration.sleepSecondsLeft / 10.0))
                                color: theme.cloud
                                Behavior on width {
                                    enabled: !theme.reducedMotion
                                    NumberAnimation { duration: 150 }
                                }
                            }

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
                                id: sleepMoon
                                anchors.verticalCenter: parent.verticalCenter
                                width: 15
                                height: 15
                                path: Icons.moon
                                color: narration.sleepFading ? theme.cloudInk
                                     : sleepButton.timerActive ? theme.accent : bar.deckSoft
                                SequentialAnimation on opacity {
                                    running: narration.sleepFading && bar.visible && !theme.reducedMotion
                                    loops: Animation.Infinite
                                    NumberAnimation { to: 0.45; duration: 600; easing.type: Easing.InOutSine }
                                    NumberAnimation { to: 1.0; duration: 600; easing.type: Easing.InOutSine }
                                    onRunningChanged: if (!running) sleepMoon.opacity = 1.0
                                }
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
                        onClicked: {
                            speedMenu.close()
                            sleepMenu.opened ? sleepMenu.close() : sleepMenu.open()
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered && !sleepMenu.opened
                        ToolTip.text: narration.sleepFading ? qsTr("Fading out… %1").arg(bar.formatClock(narration.sleepSecondsLeft))
                                    : narration.sleepSecondsLeft > 0 ? qsTr("Stopping in %1").arg(bar.formatClock(narration.sleepSecondsLeft))
                                    : narration.sleepAtChapterEnd ? qsTr("Sleep timer: end of chapter") : qsTr("Sleep timer")
                        ToolTip.delay: 600

                        Popup {
                            id: sleepMenu
                            y: -height - 12
                            x: Math.max(-sleepButton.x, Math.min(sleepButton.width - width, sleepButton.width / 2 - width / 2))
                            width: 228
                            padding: 6
                            background: Rectangle {
                                radius: Ui.radius(10, theme.radius)
                                color: bar.deckMenuBg
                                border.width: 1
                                border.color: Qt.rgba(1, 1, 1, 0.12)
                                Rectangle {
                                    z: -1
                                    anchors.fill: parent
                                    anchors.topMargin: 6
                                    anchors.bottomMargin: -6
                                    anchors.leftMargin: -3
                                    anchors.rightMargin: -3
                                    radius: parent.radius > 0 ? parent.radius + 2 : 0
                                    color: Qt.rgba(0, 0, 0, 0.32)
                                }
                            }
                            contentItem: Column {
                                spacing: 2
                                // Live active countdown / fade banner inside the sleep menu.
                                Label {
                                    visible: sleepButton.timerActive
                                    width: parent.width
                                    leftPadding: 10
                                    rightPadding: 10
                                    topPadding: 4
                                    bottomPadding: 4
                                    text: narration.sleepFading
                                          ? qsTr("Fading out · %1").arg(bar.formatClock(narration.sleepSecondsLeft))
                                          : narration.sleepSecondsLeft > 0
                                            ? qsTr("Stopping in %1").arg(bar.formatClock(narration.sleepSecondsLeft))
                                            : qsTr("Stopping at chapter end")
                                    color: narration.sleepFading ? theme.cloudInk : bar.deckSub
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                    font.features: ({ "tnum": 1 })
                                }
                                Rectangle {
                                    visible: sleepButton.timerActive
                                    width: parent.width
                                    height: 1
                                    color: Qt.rgba(1, 1, 1, 0.08)
                                }
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
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
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
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
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
                Layout.minimumWidth: bar.totalSec >= 3600 ? 46 : 34
                horizontalAlignment: Text.AlignRight
                text: bar.formatClock(timelineRow.shownElapsed)
                color: scrubber.dragging ? bar.deckFg : bar.deckMuted
                font.pixelSize: 11
                font.features: ({ "tnum": 1 })
            }

            Item {
                id: scrubber
                Layout.fillWidth: true
                implicitHeight: 16
                activeFocusOnTab: narration.durationUs > 0
                Accessible.role: Accessible.Slider
                Accessible.name: qsTr("Chapter timeline")
                property bool dragging: false
                property real dragFrac: 0

                function fracAt(px) {
                    return width > 0 ? Math.max(0, Math.min(1, px / width)) : 0
                }

                Keys.onLeftPressed: (event) => {
                    narration.skipSeconds(-15)
                    event.accepted = true
                }
                Keys.onRightPressed: (event) => {
                    narration.skipSeconds(15)
                    event.accepted = true
                }

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -2
                    visible: scrubber.activeFocus
                    radius: Ui.radius(6, theme.radius)
                    color: "transparent"
                    border.width: 1.5
                    border.color: theme.accent
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
                    id: scrubKnob
                    visible: narration.durationUs > 0 && (scrubMouse.containsMouse || scrubber.dragging || scrubber.activeFocus)
                    x: Math.max(0, Math.min(parent.width - width, parent.width * timelineRow.displayFrac - width / 2))
                    anchors.verticalCenter: parent.verticalCenter
                    width: scrubber.dragging ? 12 : 10
                    height: width
                    radius: width / 2
                    color: bar.deckFg
                    border.width: 2
                    border.color: theme.accent
                }

                // Live scrub timestamp pill while dragging so you can seek precisely.
                Rectangle {
                    visible: scrubber.dragging && bar.totalSec > 0
                    z: 20
                    x: Math.max(0, Math.min(parent.width - width, parent.width * timelineRow.displayFrac - width / 2))
                    y: -height - 6
                    width: scrubTipLabel.implicitWidth + 12
                    height: 22
                    radius: Ui.radius(6, theme.radius)
                    color: bar.deckMenuBg
                    border.width: 1
                    border.color: Qt.rgba(1, 1, 1, 0.14)
                    Label {
                        id: scrubTipLabel
                        anchors.centerIn: parent
                        text: bar.formatClock(timelineRow.shownElapsed)
                        color: bar.deckFg
                        font.pixelSize: 11
                        font.weight: Font.DemiBold
                        font.features: ({ "tnum": 1 })
                    }
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
                        if (narration.durationUs > 0)
                            narration.seekToUs(Math.round(frac * narration.durationUs))
                        scrubber.dragging = false
                    }
                    onCanceled: scrubber.dragging = false
                }
            }

            // Remaining clock (fixed minimum width) + optional elided chapter title.
            RowLayout {
                spacing: 4
                Layout.maximumWidth: Math.max(96, bar.width * 0.38)

                Label {
                    Layout.minimumWidth: bar.totalSec >= 3600 ? 52 : 40
                    horizontalAlignment: Text.AlignRight
                    text: bar.totalSec > 0 ? "−" + bar.formatClock(timelineRow.shownRemaining) : "−0:00"
                    color: bar.deckMuted
                    font.pixelSize: 11
                    font.features: ({ "tnum": 1 })
                }
                Label {
                    visible: timelineRow.chapterName !== "" && bar.width >= 460
                    Layout.fillWidth: true
                    Layout.maximumWidth: Math.max(64, bar.width * 0.26)
                    text: "· " + timelineRow.chapterName
                    color: bar.deckMuted
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
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
