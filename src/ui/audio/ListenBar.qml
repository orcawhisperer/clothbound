import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"
import "../library"

import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui

// Adaptive slim floating narration pill: always-dark warm translucent charcoal
// so it stays calm and legible over Paper, Sepia, Dusk, Night, or Omarchy
// pages. Rests as a compact 44 px single-row capsule with a 2 px hairline
// progress bar hugging its bottom inner edge, and expands smoothly on hover,
// open popover, scrubber drag, keyboard focus, or status banner to reveal full
// precision transport, sleep timer, background listen, and chapter scrubber.
Rectangle {
    id: bar

    property var look
    // Omitted inside the open reader to avoid repeating the cover and title;
    // set true when rendered outside an open book (e.g. Library background listen).
    property bool showBookInfo: false
    // Available horizontal width between open side panels (bound by EpubReader).
    property real availableWidth: parent ? parent.width : 1280
    // Pause width animation while a docked side panel is actively being resized.
    property bool animateSize: true

    readonly property bool menuOpen: sleepMenu.opened || speedMenu.opened
    readonly property bool sleepTimerActive: narration.sleepSecondsLeft > 0 || narration.sleepAtChapterEnd
    readonly property bool hasStatusBanner: narration.problem !== "" || narration.preparing
                                          || narration.fallbackNotice !== "" || narration.fallbackVoice !== ""
                                          || narration.offerOfflineSetup

    // Hover-exit debounce state so moving the pointer briefly never jitter-collapses.
    property bool hoverExpanded: false
    readonly property bool activeInteraction: pillHover.hovered || menuOpen || scrubber.dragging
                                            || pillFocusScope.activeFocus || hasStatusBanner
    readonly property bool expanded: activeInteraction || hoverExpanded

    signal voiceRequested()
    signal followRequested()
    signal openBookRequested()

    // Always-dark warm charcoal palette.
    readonly property color deckBg: Qt.rgba(28 / 255, 27 / 255, 25 / 255, 0.94)
    readonly property color deckFg: "#F2EFE9"
    readonly property color deckSub: "#B9B3A8"
    readonly property color deckSoft: "#CFC9BE"
    readonly property color deckMuted: "#A9A296"
    readonly property color deckRim: (look && look.dark) ? Qt.rgba(1, 1, 1, 0.14) : Qt.rgba(0, 0, 0, 0.25)
    readonly property color deckFill: Qt.rgba(1, 1, 1, 0.08)
    readonly property color deckHover: Qt.rgba(1, 1, 1, 0.14)
    readonly property color deckPressed: Qt.rgba(1, 1, 1, 0.20)
    readonly property color deckMenuBg: "#1F1E1C"

    readonly property bool voiceIsBuiltin: narration.voice.startsWith("builtin:")
    readonly property bool voiceIsStudio: narration.voice.startsWith("studio:")
    readonly property bool voiceIsLocal: voiceIsBuiltin || (voiceIsStudio && ai.localIsLoopback)
    readonly property color provenanceDotColor: voiceIsLocal ? theme.positive
                                              : voiceIsStudio ? deckSoft
                                              : theme.cloud
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

    // Curved ±15s skip glyphs on a 24 × 24 grid (matching Icons.js line style).
    readonly property string skipBack15Path: "M11.2 3.2 L8.6 5.5 L11.2 7.8 M9.2 5.5 H12 A7.5 7.5 0 1 1 5.5 9.25"
    readonly property string skipFwd15Path: "M12.8 3.2 L15.4 5.5 L12.8 7.8 M14.8 5.5 H12 A7.5 7.5 0 1 0 18.5 9.25"

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

    onActiveInteractionChanged: {
        if (activeInteraction) {
            collapseTimer.stop()
            hoverExpanded = true
        } else if (hoverExpanded) {
            collapseTimer.restart()
        }
    }

    // Preserve keyboard focus smoothly when Tabbing into the resting pill expands it.
    onExpandedChanged: {
        if (expanded) {
            if (restPlayBtn.activeFocus)
                playBtn.forceActiveFocus()
            else if (restVoiceBtn.activeFocus)
                voiceButton.forceActiveFocus()
            else if (restFollowBtn.activeFocus && expFollowBtn.visible)
                expFollowBtn.forceActiveFocus()
            else if (restSleepBtn.activeFocus)
                sleepButton.forceActiveFocus()
            else if (restRateBtn.activeFocus)
                rateButton.forceActiveFocus()
            else if (restStopBtn.activeFocus)
                stopBtn.forceActiveFocus()
        }
    }

    onVisibleChanged: {
        if (!visible) {
            collapseTimer.stop()
            hoverExpanded = false
            speedMenu.close()
            sleepMenu.close()
            scrubber.dragging = false
        }
    }

    Timer {
        id: collapseTimer
        interval: 450
        onTriggered: {
            if (!bar.activeInteraction)
                bar.hoverExpanded = false
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

    readonly property real maxBarWidth: Math.max(300, bar.availableWidth - 24)
    readonly property real restingTargetWidth: {
        let w = 368
        if (!narration.following)
            w += 44
        if (sleepTimerActive)
            w += 46
        if (showBookInfo)
            w += 40
        return Math.min(420, w)
    }
    readonly property real expandedTargetWidth: (sleepTimerActive || showBookInfo) ? 568
                                              : !narration.following ? 556 : 544
    readonly property real expandedTargetHeight: hasStatusBanner ? (86 + 6 + Math.max(24, statusBannerRow.implicitHeight)) : 86

    width: Math.min(maxBarWidth, expanded ? expandedTargetWidth : restingTargetWidth)
    height: expanded ? expandedTargetHeight : 44
    radius: Ui.radius(expanded ? 16 : 22, theme.radius)

    Behavior on width {
        enabled: !theme.reducedMotion && bar.animateSize
        NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
    }
    Behavior on height {
        enabled: !theme.reducedMotion
        NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
    }
    Behavior on radius {
        enabled: !theme.reducedMotion
        NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
    }

    color: deckBg
    border.width: 1
    border.color: deckRim
    Accessible.role: Accessible.Pane
    Accessible.name: qsTr("Now playing")

    // Soft layered shadow so the dark pill floats calmly above the page.
    Rectangle {
        z: -2
        anchors.fill: parent
        anchors.topMargin: 10
        anchors.bottomMargin: -8
        anchors.leftMargin: -3
        anchors.rightMargin: -3
        radius: bar.radius > 0 ? bar.radius + 3 : 0
        color: Qt.rgba(40 / 255, 30 / 255, 10 / 255, 0.22)
    }
    Rectangle {
        z: -1
        anchors.fill: parent
        anchors.topMargin: 4
        anchors.bottomMargin: -4
        radius: bar.radius
        color: Qt.rgba(0, 0, 0, 0.28)
    }

    // Subtle inner rim highlight for glass-pill depth.
    Rectangle {
        z: 0
        anchors.fill: parent
        anchors.margins: 1
        radius: bar.radius > 0 ? Math.max(0, bar.radius - 1) : 0
        color: "transparent"
        border.width: 1
        border.color: Qt.rgba(1, 1, 1, 0.05)
    }

    // Passive hover tracking over the whole pill (stays active over child controls).
    HoverHandler {
        id: pillHover
    }

    // Keep pointer clicks and scroll wheel off the reading surface underneath.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    FocusScope {
        id: pillFocusScope
        anchors.fill: parent

        Item {
            id: contentClip
            anchors.fill: parent
            clip: true

            // =========================================================================
            // RESTING STATE: 44 px single-row calm capsule + 2 px bottom hairline
            // =========================================================================
            Item {
                id: restingLayer
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 44
                opacity: bar.expanded ? 0 : 1
                Behavior on opacity {
                    enabled: !theme.reducedMotion
                    NumberAnimation { duration: bar.expanded ? 110 : 170; easing.type: Easing.OutCubic }
                }
                visible: opacity > 0.01
                enabled: !bar.expanded

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 6
                    anchors.rightMargin: 7
                    spacing: 6

                    // Optional compact cover when shown outside the reader (clicks back into the book).
                    AbstractButton {
                        id: restCoverBtn
                        visible: bar.showBookInfo
                        Layout.preferredWidth: 22
                        Layout.preferredHeight: 30
                        Layout.leftMargin: 2
                        Layout.alignment: Qt.AlignVCenter
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: qsTr("Back to the book")
                        onClicked: bar.openBookRequested()
                        background: Rectangle {
                            radius: Ui.radius(4, theme.radius)
                            color: "transparent"
                            border.width: restCoverBtn.visualFocus ? 2 : 0
                            border.color: theme.accent
                        }
                        contentItem: Item {
                            CoverImage {
                                anchors.fill: parent
                                anchors.margins: 1
                                source: reader.bookId > 0 ? library.coverUrl(reader.bookId) : ""
                                title: reader.title
                                author: reader.author
                                radius: Ui.radius(3, theme.radius)
                            }
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered && !bar.expanded
                        ToolTip.text: qsTr("Back to the book")
                        ToolTip.delay: 500
                    }

                    // 32 px concentric accent Play/Pause circle.
                    AbstractButton {
                        id: restPlayBtn
                        Layout.preferredWidth: 32
                        Layout.preferredHeight: 32
                        Layout.alignment: Qt.AlignVCenter
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: narration.playing ? qsTr("Pause") : qsTr("Play")
                        scale: restPlayBtn.pressed && !theme.reducedMotion ? 0.94 : 1.0
                        Behavior on scale {
                            enabled: !theme.reducedMotion
                            NumberAnimation { duration: 90; easing.type: Easing.OutCubic }
                        }
                        onClicked: narration.toggle()
                        background: Rectangle {
                            radius: 16
                            color: restPlayBtn.pressed ? Qt.darker(theme.accent, 1.1)
                                 : restPlayBtn.hovered ? Qt.lighter(theme.accent, 1.07) : theme.accent
                            Behavior on color { enabled: !theme.reducedMotion; ColorAnimation { duration: 100 } }
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -2
                                visible: restPlayBtn.visualFocus
                                radius: 18
                                color: "transparent"
                                border.width: 2
                                border.color: bar.deckFg
                            }
                        }
                        contentItem: Item {
                            Glyph {
                                anchors.centerIn: parent
                                anchors.horizontalCenterOffset: narration.playing ? 0 : 1
                                width: 14
                                height: 14
                                visible: !(narration.playing && (narration.buffering || narration.preparing))
                                path: narration.playing ? Icons.pauseSolid : Icons.playSolid
                                filled: true
                                color: theme.accentOn
                            }
                            BusyIndicator {
                                anchors.centerIn: parent
                                visible: (narration.buffering || narration.preparing) && narration.playing
                                running: visible
                                implicitWidth: 18
                                implicitHeight: 18
                                palette.highlight: theme.accentOn
                            }
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered && !bar.expanded
                        ToolTip.text: narration.playing ? qsTr("Pause (P)") : qsTr("Play (P)")
                        ToolTip.delay: 600
                    }

                    // Voice switcher: subtle local/cloud dot + voice name + chevron.
                    AbstractButton {
                        id: restVoiceBtn
                        Layout.fillWidth: true
                        Layout.minimumWidth: 64
                        Layout.preferredHeight: 28
                        Layout.alignment: Qt.AlignVCenter
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
                        readonly property real naturalW: 6 + 6 + restVoiceLabel.implicitWidth + 4 + restVoiceChev.width + 12
                        background: Rectangle {
                            width: Math.min(restVoiceBtn.width, restVoiceBtn.naturalW)
                            height: restVoiceBtn.height
                            radius: Ui.radius(14, theme.radius)
                            color: restVoiceBtn.pressed ? bar.deckHover
                                 : restVoiceBtn.hovered ? bar.deckFill : "transparent"
                            Behavior on color { enabled: !theme.reducedMotion; ColorAnimation { duration: 100 } }
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -2
                                visible: restVoiceBtn.visualFocus
                                radius: Ui.radius(16, theme.radius)
                                color: "transparent"
                                border.width: 2
                                border.color: theme.accent
                            }
                        }
                        contentItem: Item {
                            Row {
                                x: 6
                                anchors.verticalCenter: parent.verticalCenter
                                spacing: 6
                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 6
                                    height: 6
                                    radius: 3
                                    color: bar.provenanceDotColor
                                }
                                Label {
                                    id: restVoiceLabel
                                    anchors.verticalCenter: parent.verticalCenter
                                    readonly property real maxTextW: Math.max(24, restVoiceBtn.width - 6 - 6 - 6 - restVoiceChev.width - 10)
                                    width: Math.min(implicitWidth, maxTextW)
                                    text: bar.showBookInfo && reader.title !== ""
                                          ? (reader.title + (narration.voiceName !== "" ? " · " + narration.voiceName : ""))
                                          : narration.voiceName
                                    color: restVoiceBtn.hovered ? bar.deckFg : bar.deckSoft
                                    font.pixelSize: 12
                                    font.weight: Font.Medium
                                    elide: Text.ElideRight
                                }
                                Glyph {
                                    id: restVoiceChev
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 10
                                    height: 10
                                    stroke: 2.0
                                    path: Icons.chevronsUpDown
                                    color: restVoiceBtn.hovered ? bar.deckSoft : bar.deckMuted
                                }
                            }
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered && !bar.expanded
                        ToolTip.text: qsTr("Choose a narrator (V)") + " · " + bar.provenanceLabel
                        ToolTip.delay: 600
                    }

                    // Compact Follow-page pill when user scrolled away from the voice.
                    AbstractButton {
                        id: restFollowBtn
                        visible: !narration.following
                        Layout.preferredHeight: 26
                        Layout.preferredWidth: restFollowRow.implicitWidth + 14
                        Layout.alignment: Qt.AlignVCenter
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: qsTr("Back to the voice")
                        onClicked: bar.followRequested()
                        background: Rectangle {
                            radius: Ui.radius(13, theme.radius)
                            color: restFollowBtn.pressed ? Ui.tint(theme.accent, 0.32)
                                 : restFollowBtn.hovered ? Ui.tint(theme.accent, 0.24) : Ui.tint(theme.accent, 0.16)
                            border.width: 1
                            border.color: Ui.tint(theme.accent, 0.36)
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -2
                                visible: restFollowBtn.visualFocus
                                radius: Ui.radius(15, theme.radius)
                                color: "transparent"
                                border.width: 2
                                border.color: theme.accent
                            }
                        }
                        contentItem: Item {
                            Row {
                                id: restFollowRow
                                anchors.centerIn: parent
                                spacing: 4
                                Glyph {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 11
                                    height: 11
                                    stroke: 2.1
                                    path: Icons.waveform
                                    color: bar.deckFg
                                }
                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: qsTr("Follow")
                                    color: bar.deckFg
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                }
                            }
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered && !bar.expanded
                        ToolTip.text: qsTr("Back to the voice")
                        ToolTip.delay: 500
                    }

                    // Subtle sleep countdown indicator in resting mode only when a timer is active.
                    AbstractButton {
                        id: restSleepBtn
                        visible: bar.sleepTimerActive
                        Layout.preferredHeight: 26
                        Layout.preferredWidth: Math.max(54, restSleepRow.implicitWidth + 12)
                        Layout.alignment: Qt.AlignVCenter
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: sleepButton.ToolTip.text
                        onClicked: {
                            speedMenu.close()
                            sleepMenu.opened ? sleepMenu.close() : sleepMenu.open()
                        }
                        background: Rectangle {
                            radius: Ui.radius(13, theme.radius)
                            color: narration.sleepFading ? theme.cloudSoft
                                 : restSleepBtn.hovered ? bar.deckHover : bar.deckFill
                            border.width: narration.sleepFading ? 1 : 0
                            border.color: Ui.tint(theme.cloud, 0.40)
                        }
                        contentItem: Item {
                            Row {
                                id: restSleepRow
                                anchors.centerIn: parent
                                spacing: 4
                                Glyph {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 12
                                    height: 12
                                    path: Icons.moon
                                    color: narration.sleepFading ? theme.cloudInk : theme.accent
                                }
                                Label {
                                    anchors.verticalCenter: parent.verticalCenter
                                    text: narration.sleepSecondsLeft > 0
                                          ? bar.formatClock(narration.sleepSecondsLeft)
                                          : qsTr("Chapter")
                                    color: narration.sleepFading ? theme.cloudInk : bar.deckSoft
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                    font.features: ({ "tnum": 1 })
                                }
                            }
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                    }

                    // Rate pill (1.0×): pinned width so 1.0× ↔ 1.25× never jitters.
                    AbstractButton {
                        id: restRateBtn
                        Layout.preferredWidth: 44
                        Layout.preferredHeight: 26
                        Layout.alignment: Qt.AlignVCenter
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: qsTr("Speed %1").arg(restRateLabel.text)
                        onClicked: {
                            sleepMenu.close()
                            speedMenu.opened ? speedMenu.close() : speedMenu.open()
                        }
                        background: Rectangle {
                            radius: Ui.radius(13, theme.radius)
                            color: speedMenu.opened ? bar.deckHover
                                 : restRateBtn.pressed ? bar.deckPressed
                                 : restRateBtn.hovered ? bar.deckHover : Qt.rgba(1, 1, 1, 0.08)
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -2
                                visible: restRateBtn.visualFocus
                                radius: Ui.radius(15, theme.radius)
                                color: "transparent"
                                border.width: 2
                                border.color: theme.accent
                            }
                        }
                        contentItem: Label {
                            id: restRateLabel
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            text: bar.formatRate(narration.rate)
                            color: bar.deckFg
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            font.features: ({ "tnum": 1 })
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered && !speedMenu.opened && !bar.expanded
                        ToolTip.text: qsTr("Playback speed (< and >)")
                        ToolTip.delay: 600
                    }

                    // Stop button (✕).
                    AbstractButton {
                        id: restStopBtn
                        Layout.preferredWidth: 28
                        Layout.preferredHeight: 28
                        Layout.alignment: Qt.AlignVCenter
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: qsTr("Stop listening")
                        onClicked: narration.stop()
                        background: Rectangle {
                            radius: 14
                            color: restStopBtn.pressed ? bar.deckPressed
                                 : restStopBtn.hovered ? bar.deckFill : "transparent"
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -2
                                visible: restStopBtn.visualFocus
                                radius: 16
                                color: "transparent"
                                border.width: 2
                                border.color: theme.accent
                            }
                        }
                        contentItem: Item {
                            Glyph {
                                anchors.centerIn: parent
                                width: 13
                                height: 13
                                stroke: 2.0
                                path: Icons.close
                                color: restStopBtn.hovered ? bar.deckFg : bar.deckSub
                            }
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered && !bar.expanded
                        ToolTip.text: qsTr("Stop listening (⇧P)")
                        ToolTip.delay: 600
                    }
                }

                // Ultra-slim 2 px hairline chapter progress bar hugging the bottom inner edge
                // (shifts cleanly to a 2 px amber drain during the sleep timer's final 10 s).
                Rectangle {
                    id: restingHairline
                    anchors.bottom: parent.bottom
                    anchors.bottomMargin: 1
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.leftMargin: bar.radius > 0 ? Math.max(14, bar.radius - 2) : 1
                    anchors.rightMargin: bar.radius > 0 ? Math.max(14, bar.radius - 2) : 1
                    height: 2
                    radius: bar.radius > 0 ? 1 : 0
                    color: Qt.rgba(1, 1, 1, 0.10)

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        readonly property real frac: narration.sleepFading
                            ? Math.max(0, Math.min(1, narration.sleepSecondsLeft / 10.0))
                            : bar.playbackFrac
                        width: parent.width * frac
                        radius: parent.radius
                        color: narration.sleepFading ? theme.cloud : theme.accent
                        Behavior on width {
                            enabled: !theme.reducedMotion
                            NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
                        }
                        Behavior on color {
                            enabled: !theme.reducedMotion
                            ColorAnimation { duration: 180 }
                        }
                    }
                }
            }

            // =========================================================================
            // EXPANDED STATE: Precision transport + secondary controls + scrubber
            // =========================================================================
            ColumnLayout {
                id: expandedCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 10
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 8
                opacity: bar.expanded ? 1 : 0
                Behavior on opacity {
                    enabled: !theme.reducedMotion
                    NumberAnimation { duration: bar.expanded ? 180 : 110; easing.type: Easing.OutCubic }
                }
                visible: opacity > 0.01
                enabled: bar.expanded

                // --- Row 1: Voice/Follow · Dead-Center Transport · Rate/Sleep/Background/Stop ---
                Item {
                    id: expandedRow1
                    Layout.fillWidth: true
                    Layout.preferredHeight: 36

                    // Left zone: strictly bounded to the left of the centered transport cluster.
                    RowLayout {
                        id: expandedLeftZone
                        anchors.left: parent.left
                        anchors.right: transportRow.left
                        anchors.rightMargin: 8
                        anchors.verticalCenter: parent.verticalCenter
                        height: parent.height
                        spacing: 6
                        clip: true

                        AbstractButton {
                            id: expCoverBtn
                            visible: bar.showBookInfo
                            Layout.preferredWidth: 24
                            Layout.preferredHeight: 34
                            Layout.alignment: Qt.AlignVCenter
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            Accessible.name: qsTr("Back to the book")
                            onClicked: bar.openBookRequested()
                            background: Rectangle {
                                radius: Ui.radius(4, theme.radius)
                                color: "transparent"
                                border.width: expCoverBtn.visualFocus ? 2 : 0
                                border.color: theme.accent
                            }
                            contentItem: Item {
                                CoverImage {
                                    anchors.fill: parent
                                    source: reader.bookId > 0 ? library.coverUrl(reader.bookId) : ""
                                    title: reader.title
                                    author: reader.author
                                    radius: Ui.radius(3, theme.radius)
                                }
                                Rectangle {
                                    anchors.left: parent.left
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    width: 2
                                    color: Qt.rgba(0, 0, 0, 0.30)
                                }
                            }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Back to the book")
                            ToolTip.delay: 500
                        }

                        ColumnLayout {
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            Layout.alignment: Qt.AlignVCenter
                            spacing: 1

                            AbstractButton {
                                id: bookTitleBtn
                                visible: bar.showBookInfo && reader.title !== ""
                                Layout.fillWidth: true
                                Layout.minimumWidth: 40
                                implicitHeight: 16
                                hoverEnabled: true
                                focusPolicy: Qt.TabFocus
                                Accessible.name: qsTr("Back to the book")
                                onClicked: bar.openBookRequested()
                                background: Rectangle {
                                    radius: Ui.radius(4, theme.radius)
                                    color: bookTitleBtn.pressed ? bar.deckHover
                                         : bookTitleBtn.hovered ? bar.deckFill : "transparent"
                                }
                                contentItem: Label {
                                    leftPadding: 4
                                    rightPadding: 4
                                    verticalAlignment: Text.AlignVCenter
                                    text: reader.title
                                    color: bookTitleBtn.hovered ? theme.accent : bar.deckFg
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                    elide: Text.ElideRight
                                }
                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Back to the book")
                                ToolTip.delay: 500
                            }

                            AbstractButton {
                                id: voiceButton
                                Layout.fillWidth: true
                                Layout.minimumWidth: 44
                                implicitHeight: bar.showBookInfo ? 18 : 28
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
                                    6 + 6 + 5 + voiceNameLabel.implicitWidth + 5 + voiceChevron.width + 8

                                background: Rectangle {
                                    x: 0
                                    width: Math.min(voiceButton.width, voiceButton.contentNaturalW)
                                    height: voiceButton.height
                                    radius: Ui.radius(8, theme.radius)
                                    color: voiceButton.pressed ? bar.deckHover
                                         : voiceButton.hovered ? bar.deckFill : "transparent"
                                    Behavior on color { enabled: !theme.reducedMotion; ColorAnimation { duration: 100 } }
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: -2
                                        visible: voiceButton.visualFocus
                                        radius: Ui.radius(10, theme.radius)
                                        color: "transparent"
                                        border.width: 2
                                        border.color: theme.accent
                                    }
                                }

                                contentItem: Item {
                                    clip: true
                                    Row {
                                        x: 6
                                        anchors.verticalCenter: parent.verticalCenter
                                        spacing: 5

                                        Rectangle {
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: 6
                                            height: 6
                                            radius: 3
                                            color: bar.provenanceDotColor
                                        }

                                        Label {
                                            id: voiceNameLabel
                                            anchors.verticalCenter: parent.verticalCenter
                                            readonly property real maxTextW: Math.max(20, voiceButton.width - 6 - 6 - 5 - 5 - voiceChevron.width - 8)
                                            width: Math.min(implicitWidth, maxTextW)
                                            text: narration.voiceName
                                            color: voiceButton.hovered ? bar.deckFg : bar.deckSoft
                                            font.pixelSize: bar.showBookInfo ? 11 : 12
                                            font.weight: Font.Medium
                                            elide: Text.ElideRight
                                        }

                                        Glyph {
                                            id: voiceChevron
                                            anchors.verticalCenter: parent.verticalCenter
                                            width: 10
                                            height: 10
                                            stroke: 2.0
                                            path: Icons.chevronsUpDown
                                            color: voiceButton.hovered ? bar.deckSoft : bar.deckMuted
                                        }
                                    }
                                }

                                HoverHandler { cursorShape: Qt.PointingHandCursor }
                                ToolTip.visible: hovered
                                ToolTip.text: qsTr("Choose a narrator (V)") + " · " + bar.provenanceLabel
                                ToolTip.delay: 600
                            }
                        }

                        // Compact Follow-page pill when scrolled away.
                        AbstractButton {
                            id: expFollowBtn
                            visible: !narration.following
                            Layout.preferredHeight: 24
                            Layout.preferredWidth: expFollowRow.implicitWidth + 12
                            Layout.alignment: Qt.AlignVCenter
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            Accessible.name: qsTr("Back to the voice")
                            onClicked: bar.followRequested()
                            background: Rectangle {
                                radius: Ui.radius(12, theme.radius)
                                color: expFollowBtn.pressed ? Ui.tint(theme.accent, 0.32)
                                     : expFollowBtn.hovered ? Ui.tint(theme.accent, 0.24) : Ui.tint(theme.accent, 0.16)
                                border.width: 1
                                border.color: Ui.tint(theme.accent, 0.36)
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -2
                                    visible: expFollowBtn.visualFocus
                                    radius: Ui.radius(14, theme.radius)
                                    color: "transparent"
                                    border.width: 2
                                    border.color: theme.accent
                                }
                            }
                            contentItem: Item {
                                Row {
                                    id: expFollowRow
                                    anchors.centerIn: parent
                                    spacing: 4
                                    Glyph {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 10
                                        height: 10
                                        stroke: 2.1
                                        path: Icons.waveform
                                        color: bar.deckFg
                                    }
                                    Label {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: qsTr("Follow")
                                        color: bar.deckFg
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                    }
                                }
                            }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Back to the voice")
                            ToolTip.delay: 500
                        }
                    }

                    // Center zone: Prev sentence · ↺15s · ▶/⏸ · ↻15s · Next sentence — dead-center.
                    Row {
                        id: transportRow
                        anchors.centerIn: parent
                        spacing: 3

                        AbstractButton {
                            id: prevBtn
                            anchors.verticalCenter: parent.verticalCenter
                            implicitWidth: 28
                            implicitHeight: 28
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            Accessible.name: qsTr("Previous sentence")
                            onClicked: narration.prevSentence()
                            background: Rectangle {
                                radius: 14
                                color: prevBtn.pressed ? bar.deckPressed
                                     : prevBtn.hovered ? bar.deckFill : "transparent"
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -2
                                    visible: prevBtn.visualFocus
                                    radius: 16
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
                                    path: Icons.skipBackSolid
                                    filled: true
                                    color: prevBtn.hovered ? bar.deckFg : bar.deckSoft
                                }
                            }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Previous sentence")
                            ToolTip.delay: 600
                        }

                        AbstractButton {
                            id: back15Btn
                            visible: bar.width >= 470
                            anchors.verticalCenter: parent.verticalCenter
                            implicitWidth: 36
                            implicitHeight: 28
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
                            contentItem: Item {
                                Row {
                                    anchors.centerIn: parent
                                    spacing: 2
                                    Glyph {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.verticalCenterOffset: -0.5
                                        width: 12
                                        height: 12
                                        stroke: 2.0
                                        path: bar.skipBack15Path
                                        color: back15Btn.hovered ? bar.deckFg : bar.deckSub
                                    }
                                    Label {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: "15"
                                        color: back15Btn.hovered ? bar.deckFg : bar.deckSub
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                        font.features: ({ "tnum": 1 })
                                    }
                                }
                            }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Back 15 seconds (⇧←)")
                            ToolTip.delay: 600
                        }

                        AbstractButton {
                            id: playBtn
                            anchors.verticalCenter: parent.verticalCenter
                            implicitWidth: 36
                            implicitHeight: 36
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
                                radius: 18
                                color: playBtn.pressed ? Qt.darker(theme.accent, 1.1)
                                     : playBtn.hovered ? Qt.lighter(theme.accent, 1.07) : theme.accent
                                Behavior on color { enabled: !theme.reducedMotion; ColorAnimation { duration: 100 } }
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -3
                                    visible: playBtn.visualFocus
                                    radius: 21
                                    color: "transparent"
                                    border.width: 2
                                    border.color: bar.deckFg
                                }
                            }
                            contentItem: Item {
                                Glyph {
                                    anchors.centerIn: parent
                                    anchors.horizontalCenterOffset: narration.playing ? 0 : 1
                                    width: 15
                                    height: 15
                                    visible: !(narration.playing && (narration.buffering || narration.preparing))
                                    path: narration.playing ? Icons.pauseSolid : Icons.playSolid
                                    filled: true
                                    color: theme.accentOn
                                }
                                BusyIndicator {
                                    anchors.centerIn: parent
                                    visible: (narration.buffering || narration.preparing) && narration.playing
                                    running: visible
                                    implicitWidth: 20
                                    implicitHeight: 20
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
                            id: fwd15Btn
                            visible: bar.width >= 470
                            anchors.verticalCenter: parent.verticalCenter
                            implicitWidth: 36
                            implicitHeight: 28
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
                            contentItem: Item {
                                Row {
                                    anchors.centerIn: parent
                                    spacing: 2
                                    Glyph {
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.verticalCenterOffset: -0.5
                                        width: 12
                                        height: 12
                                        stroke: 2.0
                                        path: bar.skipFwd15Path
                                        color: fwd15Btn.hovered ? bar.deckFg : bar.deckSub
                                    }
                                    Label {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: "15"
                                        color: fwd15Btn.hovered ? bar.deckFg : bar.deckSub
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                        font.features: ({ "tnum": 1 })
                                    }
                                }
                            }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Forward 15 seconds (⇧→)")
                            ToolTip.delay: 600
                        }

                        AbstractButton {
                            id: nextBtn
                            anchors.verticalCenter: parent.verticalCenter
                            implicitWidth: 28
                            implicitHeight: 28
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            Accessible.name: qsTr("Next sentence")
                            onClicked: narration.nextSentence()
                            background: Rectangle {
                                radius: 14
                                color: nextBtn.pressed ? bar.deckPressed
                                     : nextBtn.hovered ? bar.deckFill : "transparent"
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -2
                                    visible: nextBtn.visualFocus
                                    radius: 16
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
                                    path: Icons.skipForwardSolid
                                    filled: true
                                    color: nextBtn.hovered ? bar.deckFg : bar.deckSoft
                                }
                            }
                            HoverHandler { cursorShape: Qt.PointingHandCursor }
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("Next sentence")
                            ToolTip.delay: 600
                        }
                    }

                    // Right zone: Speed popover pill · Sleep timer · Background tray · Stop ×.
                    Row {
                        id: rightRow
                        anchors.right: parent.right
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 3

                        AbstractButton {
                            id: rateButton
                            anchors.verticalCenter: parent.verticalCenter
                            implicitHeight: 28
                            // Pinned width so 1.0× ↔ 1.25× never jitters the row.
                            implicitWidth: 44
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
                        }

                        AbstractButton {
                            id: sleepButton
                            anchors.verticalCenter: parent.verticalCenter
                            readonly property bool timerActive: bar.sleepTimerActive
                            implicitHeight: 28
                            // Stable active width prevents jitter as countdown ticks 10:00 → 9:59 → 0:09.
                            implicitWidth: timerActive ? Math.max(62, sleepRow.implicitWidth + 14) : 28
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
                            contentItem: Item {
                                Row {
                                    id: sleepRow
                                    anchors.centerIn: parent
                                    spacing: 4
                                    Glyph {
                                        id: sleepMoon
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 14
                                        height: 14
                                        path: Icons.moon
                                        color: narration.sleepFading ? theme.cloudInk
                                             : sleepButton.timerActive ? theme.accent : bar.deckSoft
                                        SequentialAnimation on opacity {
                                            running: narration.sleepFading && expandedCol.visible && !theme.reducedMotion
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
                                        color: narration.sleepFading ? theme.cloudInk : bar.deckSoft
                                        font.pixelSize: 12
                                        font.weight: narration.sleepSecondsLeft > 0 ? Font.DemiBold : Font.Normal
                                        font.features: ({ "tnum": 1 })
                                    }
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
                        }

                        AbstractButton {
                            id: trayBtn
                            visible: bar.width >= 420
                            anchors.verticalCenter: parent.verticalCenter
                            implicitWidth: 28
                            implicitHeight: 28
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
                                    width: 14
                                    height: 14
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
                            implicitWidth: 28
                            implicitHeight: 28
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
                                    width: 13
                                    height: 13
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

                // --- Row 2: Interactive Chapter Audio Timeline & Scrubber ---
                RowLayout {
                    id: timelineRow
                    Layout.fillWidth: true
                    Layout.preferredHeight: 22
                    Layout.leftMargin: 2
                    Layout.rightMargin: 2
                    spacing: 10

                    readonly property real displayFrac: scrubber.dragging ? scrubber.dragFrac : bar.playbackFrac
                    readonly property real shownElapsed: scrubber.dragging ? scrubber.dragFrac * bar.totalSec : bar.elapsedSec
                    readonly property real shownRemaining: Math.max(0, bar.totalSec - shownElapsed)
                    readonly property string chapterName: reader.titleOfChapter(narration.chapter)

                    Label {
                        Layout.minimumWidth: bar.totalSec >= 3600 ? 44 : 32
                        horizontalAlignment: Text.AlignRight
                        text: bar.formatClock(timelineRow.shownElapsed)
                        color: scrubber.dragging ? bar.deckFg : bar.deckMuted
                        font.pixelSize: 11
                        font.features: ({ "tnum": 1 })
                    }

                    Item {
                        id: scrubber
                        Layout.fillWidth: true
                        implicitHeight: 18
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
                        Layout.maximumWidth: Math.max(88, bar.width * 0.36)

                        Label {
                            Layout.minimumWidth: bar.totalSec >= 3600 ? 50 : 38
                            horizontalAlignment: Text.AlignRight
                            text: bar.totalSec > 0 ? "−" + bar.formatClock(timelineRow.shownRemaining) : "−0:00"
                            color: bar.deckMuted
                            font.pixelSize: 11
                            font.features: ({ "tnum": 1 })
                        }
                        Label {
                            visible: timelineRow.chapterName !== "" && bar.width >= 480
                            Layout.fillWidth: true
                            Layout.maximumWidth: Math.max(56, bar.width * 0.22)
                            text: "· " + timelineRow.chapterName
                            color: bar.deckMuted
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                    }
                }

                // --- Conditional Row 3: Honest Status / Preparing / Offline Fallback ---
                RowLayout {
                    id: statusBannerRow
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
    }

    // Live floating scrub timestamp pill while dragging (outside contentClip so never clipped).
    Rectangle {
        id: scrubTip
        visible: scrubber.dragging && bar.totalSec > 0 && bar.expanded
        z: 20
        readonly property real knobX: 14 + scrubber.x + scrubber.width * timelineRow.displayFrac
        x: Math.max(8, Math.min(bar.width - width - 8, knobX - width / 2))
        y: 10 + expandedRow1.height + 8 - height - 6
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

    // Speed popover attached directly to bar so resting & expanded share one crisp menu.
    Popup {
        id: speedMenu
        y: -height - 10
        readonly property real anchorX: (bar.width - 12) - rightRow.implicitWidth + rateButton.x + rateButton.width / 2
        x: Math.max(0, Math.min(bar.width - width, anchorX - width / 2))
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

    // Sleep timer popover attached directly to bar.
    Popup {
        id: sleepMenu
        y: -height - 10
        readonly property real anchorX: (bar.width - 12) - rightRow.implicitWidth + sleepButton.x + sleepButton.width / 2
        x: Math.max(0, Math.min(bar.width - width, anchorX - width / 2))
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
                visible: bar.sleepTimerActive
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
                visible: bar.sleepTimerActive
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
