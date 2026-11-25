import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui

// Journey J4 — First listen: an app-chrome anchored popover opening under the
// headphones button (or P) when no voice is installed yet. Offers one voice
// suited to the book's language with an avatar card, downloads in place with
// a 6 px progress bar while you keep reading, and confirms "Ready. Next time
// P just starts." when complete.
FocusScope {
    id: offer

    property var look
    property bool open: false
    // Distance from the right edge of this popover to the center of the
    // toolbar headphones button, for positioning the top caret arrow.
    property real caretRightOffset: 64
    readonly property real radius: Ui.radius(14, theme.radius)

    // "idle", "busy", or "done".
    property string step: "idle"
    property var offeredVoice: null
    property real offeredBytes: 0
    property string pendingVoice: ""

    readonly property var livePack: open ? voicePacks.offer(reader.language) : ({})
    readonly property var voice: offeredVoice || livePack.voice || null
    readonly property real bytes: offeredBytes > 0 ? offeredBytes : (livePack.bytes || 0)

    readonly property var avatarPalette: ["#3E5F99", "#7A4E2D", "#2F6B57", "#6B4A82", "#8A3F4A", "#4A5A6B"]
    function avatarColor(name) {
        return avatarPalette[Ui.hash(String(name || "")) % avatarPalette.length]
    }

    signal moreVoices()
    signal closed()

    function show() {
        const pack = voicePacks.offer(reader.language)
        if (pack && pack.voice) {
            offeredVoice = pack.voice
            offeredBytes = pack.bytes || 0
        } else {
            offeredVoice = null
            offeredBytes = 0
        }
        step = (voicePacks.busy && pendingVoice !== "") ? "busy" : "idle"
        open = true
        forceActiveFocus()
    }

    // Closing the popover hides it cleanly without aborting an in-flight
    // download ("Keep reading; it starts by itself."). Only the explicit
    // Cancel button cancels the download.
    function close() {
        if (!open)
            return
        open = false
        closed()
    }

    // Ensure cleanup runs whether closed via close() or external `open = false`
    // (such as EpubReader's Esc handler or closeBook()).
    onOpenChanged: {
        if (!open) {
            voices.stopPreview()
            if (step === "done") {
                step = "idle"
                offeredVoice = null
                offeredBytes = 0
            }
        }
    }

    Component.onDestruction: voices.stopPreview()

    // Wear app chrome (independent of the reading theme underneath).
    palette.window: theme.raised
    palette.windowText: theme.ink1
    palette.base: theme.fill
    palette.alternateBase: theme.fillHover
    palette.text: theme.ink1
    palette.button: theme.fill
    palette.buttonText: theme.ink1
    palette.mid: theme.ink3
    palette.midlight: theme.hairline
    palette.dark: theme.separator
    palette.light: theme.overlay
    palette.shadow: theme.shadow
    palette.highlight: theme.accent
    palette.highlightedText: theme.accentOn
    palette.link: theme.accentInk
    palette.linkVisited: theme.accentSoft
    palette.toolTipBase: theme.overlay
    palette.toolTipText: theme.ink1

    width: Math.min(400, (parent ? parent.width : 1280) - 44)
    height: contentCol.implicitHeight
    visible: opacity > 0.01
    opacity: open ? 1 : 0
    transformOrigin: Item.TopRight
    scale: open || theme.reducedMotion ? 1.0 : 0.965
    transform: Translate {
        y: offer.open || theme.reducedMotion ? 0 : -4
        Behavior on y {
            enabled: !theme.reducedMotion
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }
    }
    Behavior on opacity {
        NumberAnimation { duration: theme.reducedMotion ? 0 : (offer.open ? 160 : 120); easing.type: Easing.OutCubic }
    }
    Behavior on scale {
        enabled: !theme.reducedMotion
        NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
    }
    Accessible.role: Accessible.Dialog
    Accessible.name: qsTr("Read aloud")

    Keys.onShortcutOverride: (event) => {
        if (offer.open && event.key === Qt.Key_Escape)
            event.accepted = true
    }
    Keys.onEscapePressed: (event) => {
        if (offer.open) {
            offer.close()
            event.accepted = true
        }
    }

    // Soft layered popover shadow matching app-chrome modals.
    Repeater {
        model: [
            { spread: 1, yOff: 3, alpha: 0.26 },
            { spread: 5, yOff: 8, alpha: 0.16 },
            { spread: 10, yOff: 14, alpha: 0.08 }
        ]
        Rectangle {
            required property var modelData
            z: -2
            x: -modelData.spread
            y: modelData.yOff
            width: offer.width + modelData.spread * 2
            height: offer.height + modelData.spread
            radius: offer.radius > 0 ? offer.radius + modelData.spread : 0
            color: theme.shadow
            opacity: modelData.alpha
        }
    }

    // Card surface behind the content and callout caret mask.
    Rectangle {
        anchors.fill: parent
        radius: offer.radius
        color: theme.raised
        border.width: 1
        border.color: theme.hairline
    }

    // Top rotated-45° callout caret pointing to the headphones button.
    Rectangle {
        id: caret
        x: Math.max(20, Math.min(offer.width - 32, offer.width - offer.caretRightOffset - 6))
        y: -6
        width: 12
        height: 12
        rotation: 45
        color: theme.raised
        border.width: 1
        border.color: theme.hairline
    }
    // Masks the lower half of the rotated caret border inside the card.
    Rectangle {
        x: caret.x - 4
        y: 1
        width: 20
        height: 10
        color: theme.raised
    }

    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    Connections {
        target: voicePacks
        function onReady(voiceId) {
            if (voiceId !== offer.pendingVoice)
                return
            offer.pendingVoice = ""
            const v = voicePacks.voice(voiceId)
            if (v && v.id) {
                offer.offeredVoice = v
                narration.setVoice(v.id, v.name)
            }
            if (offer.open) {
                offer.step = "done"
            } else {
                offer.step = "idle"
                if (!narration.active && narration.hasPendingStart)
                    narration.startPending()
            }
        }
        function onFailed() {
            if (offer.pendingVoice === "")
                return
            offer.pendingVoice = ""
            offer.step = "idle"
        }
        function onProgressChanged() {
            // If an in-flight download was cancelled elsewhere (e.g. VoicePanel),
            // never leave this popover stuck in "busy".
            if (offer.step === "busy" && !voicePacks.busy && offer.pendingVoice !== "") {
                const v = voicePacks.voice(offer.pendingVoice)
                if (!v || !v.installed) {
                    offer.pendingVoice = ""
                    offer.step = "idle"
                }
            }
        }
    }

    ColumnLayout {
        id: contentCol
        anchors.left: parent.left
        anchors.right: parent.right
        spacing: 0

        // Header: 18 18 14 px.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: 18
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            Layout.bottomMargin: 14
            spacing: 6

            Label {
                Layout.fillWidth: true
                text: qsTr("Read this book aloud")
                color: theme.ink1
                font.pixelSize: 15
                font.weight: Font.DemiBold
            }
            Label {
                Layout.fillWidth: true
                text: offer.voice !== null
                      ? qsTr("A voice made on this computer: free, private, and it works offline. It downloads once.")
                      : qsTr("Choose a narration voice to read this book aloud.")
                color: theme.ink3
                font.pixelSize: 13
                wrapMode: Text.Wrap
                lineHeight: 1.5
            }
        }

        // Embedded voice preview card: margin 0 12, padding 12.
        Rectangle {
            visible: offer.voice !== null
            Layout.fillWidth: true
            Layout.leftMargin: 12
            Layout.rightMargin: 12
            radius: Ui.radius(10, theme.radius)
            color: Ui.tint(theme.ink1, 0.05)
            implicitHeight: voiceCardRow.implicitHeight + 24

            RowLayout {
                id: voiceCardRow
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.verticalCenter: parent.verticalCenter
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 12

                Rectangle {
                    Layout.preferredWidth: 36
                    Layout.preferredHeight: 36
                    Layout.alignment: Qt.AlignVCenter
                    radius: 18
                    color: offer.avatarColor(offer.voice ? offer.voice.name : "E")
                    Label {
                        anchors.centerIn: parent
                        text: offer.voice && offer.voice.name ? String(offer.voice.name).charAt(0).toUpperCase() : "?"
                        color: "#F2F2F3"
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 2

                    Label {
                        Layout.fillWidth: true
                        text: offer.voice
                              ? offer.voice.name + (offer.voice.accent ? " · " + offer.voice.accent : "")
                              : ""
                        color: theme.ink1
                        font.pixelSize: 13
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Label {
                        Layout.fillWidth: true
                        text: {
                            if (!offer.voice)
                                return ""
                            const engine = offer.voice.engine || qsTr("Built-in")
                            if (offer.step === "done" || offer.voice.installed)
                                return qsTr("Suits this book · %1 · Ready").arg(engine)
                            const mb = Math.max(1, Math.round(offer.bytes / 1e6))
                            return qsTr("Suits this book · %1 · %2 MB").arg(engine).arg(mb.toLocaleString(Qt.locale(), 'f', 0))
                        }
                        color: theme.ink3
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }

                // Sample preview circle: live once installed; honest tooltip before download.
                AbstractButton {
                    id: sampleBtn
                    readonly property bool playable: offer.voice !== null && (offer.step === "done" || !!offer.voice.installed)
                    readonly property bool playing: offer.voice !== null && voices.previewing === offer.voice.id
                    Layout.preferredWidth: 30
                    Layout.preferredHeight: 30
                    Layout.alignment: Qt.AlignVCenter
                    enabled: playable
                    hoverEnabled: true
                    focusPolicy: playable ? Qt.TabFocus : Qt.NoFocus
                    opacity: playable ? 1.0 : 0.40
                    Accessible.name: playing ? qsTr("Stop sample") : qsTr("Hear a sample")
                    onClicked: {
                        if (!offer.voice)
                            return
                        if (playing)
                            voices.stopPreview()
                        else
                            voices.preview(offer.voice.id, reader.snippet)
                    }
                    background: Rectangle {
                        radius: 15
                        color: sampleBtn.playing ? theme.accentSoft
                             : sampleBtn.pressed && sampleBtn.playable ? theme.fillPressed
                             : sampleBtn.hovered && sampleBtn.playable ? theme.fillHover
                             : Ui.tint(theme.ink1, 0.08)
                        border.width: sampleBtn.playing ? 1 : 0
                        border.color: Ui.tint(theme.accent, 0.45)
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: -2
                            visible: sampleBtn.visualFocus
                            radius: 17
                            color: "transparent"
                            border.width: 2
                            border.color: theme.accent
                        }
                    }
                    contentItem: Item {
                        Glyph {
                            anchors.centerIn: parent
                            anchors.horizontalCenterOffset: sampleBtn.playing ? 0 : 1
                            width: 12
                            height: 12
                            visible: !(sampleBtn.playing && voices.previewBuffering)
                            path: sampleBtn.playing ? Icons.pauseSolid : Icons.playSolid
                            filled: true
                            color: sampleBtn.playing ? theme.accentInk : theme.ink1
                        }
                        BusyIndicator {
                            anchors.centerIn: parent
                            implicitWidth: 20
                            implicitHeight: 20
                            visible: sampleBtn.playing && voices.previewBuffering
                            running: visible
                        }
                    }
                    HoverHandler { cursorShape: sampleBtn.playable ? Qt.PointingHandCursor : Qt.ArrowCursor }
                    ToolTip.visible: hovered
                    ToolTip.text: playable
                                  ? (playing ? qsTr("Stop sample") : qsTr("Hear a sample"))
                                  : qsTr("Sample plays once downloaded")
                    ToolTip.delay: 400
                }
            }
        }

        // 3-State Action Footer: padding 14 18 18, gap 10.
        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: 14
            Layout.leftMargin: 18
            Layout.rightMargin: 18
            Layout.bottomMargin: 18
            spacing: 10

            // --- State 1: idle ---
            RowLayout {
                visible: offer.step === "idle"
                Layout.fillWidth: true
                spacing: 8

                TextButton {
                    visible: offer.voice !== null
                    Layout.fillWidth: true
                    primary: true
                    size: "large"
                    enabled: !voicePacks.busy
                    text: voicePacks.busy ? qsTr("Download in progress…") : qsTr("Download and listen")
                    onClicked: {
                        if (voicePacks.busy || !offer.voice)
                            return
                        offer.pendingVoice = offer.voice.id
                        offer.step = "busy"
                        voicePacks.installVoice(offer.voice.id)
                    }
                }
                TextButton {
                    visible: offer.voice === null
                    Layout.fillWidth: true
                    primary: true
                    size: "large"
                    text: qsTr("Choose a voice…")
                    onClicked: {
                        offer.close()
                        offer.moreVoices()
                    }
                }
                TextButton {
                    size: "large"
                    text: qsTr("Not now")
                    onClicked: offer.close()
                }
            }

            Label {
                Layout.fillWidth: true
                visible: offer.step === "idle" && voicePacks.problem !== ""
                text: voicePacks.problem
                color: theme.danger
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            AbstractButton {
                id: otherVoicesLink
                visible: offer.step === "idle" && offer.voice !== null
                Layout.fillWidth: true
                implicitHeight: otherVoicesLabel.implicitHeight + 4
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                onClicked: {
                    offer.close()
                    offer.moreVoices()
                }
                background: Rectangle {
                    radius: Ui.radius(5, theme.radius)
                    color: "transparent"
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -2
                        visible: otherVoicesLink.visualFocus
                        radius: Ui.radius(7, theme.radius)
                        color: "transparent"
                        border.width: 2
                        border.color: theme.accent
                    }
                }
                contentItem: Label {
                    id: otherVoicesLabel
                    text: qsTr("Other voices, or Gemini's more natural ones…")
                    color: otherVoicesLink.hovered ? theme.ink1 : theme.ink3
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
            }

            // --- State 2: busy ---
            RowLayout {
                visible: offer.step === "busy"
                Layout.fillWidth: true

                Label {
                    Layout.fillWidth: true
                    text: voicePacks.progress >= 1.0
                          ? qsTr("Getting %1 ready…").arg(offer.voice ? offer.voice.name : qsTr("voice"))
                          : qsTr("Downloading %1…").arg(offer.voice ? offer.voice.name : qsTr("voice"))
                    color: theme.ink1
                    font.pixelSize: 12
                    font.features: ({ "tnum": 1 })
                    elide: Text.ElideRight
                }
                Label {
                    visible: voicePacks.progress < 1.0
                    text: Math.round(Math.max(0, Math.min(1, voicePacks.progress)) * 100).toLocaleString(Qt.locale(), 'f', 0) + "%"
                    color: theme.ink3
                    font.pixelSize: 12
                    font.features: ({ "tnum": 1 })
                }
            }

            // 6 px progress track matching 13_j4_first_listen.html.
            Rectangle {
                visible: offer.step === "busy"
                Layout.fillWidth: true
                Layout.preferredHeight: 6
                radius: 3
                color: Ui.tint(theme.ink1, 0.10)
                clip: true

                Rectangle {
                    visible: voicePacks.progress > 0 && voicePacks.progress < 1.0
                    width: parent.width * Math.max(0.02, Math.min(1, voicePacks.progress))
                    height: parent.height
                    radius: 3
                    color: theme.accent
                    Behavior on width {
                        enabled: !theme.reducedMotion
                        NumberAnimation { duration: 160 }
                    }
                }
                Rectangle {
                    id: busyGlide
                    visible: voicePacks.progress <= 0 || voicePacks.progress >= 1.0
                    width: parent.width * 0.34
                    height: parent.height
                    radius: 3
                    color: theme.accent
                    XAnimator on x {
                        running: offer.visible && offer.step === "busy" && (voicePacks.progress <= 0 || voicePacks.progress >= 1.0) && !theme.reducedMotion
                        from: -busyGlide.width
                        to: busyGlide.parent ? busyGlide.parent.width : 360
                        duration: 1100
                        loops: Animation.Infinite
                        easing.type: Easing.InOutQuad
                    }
                }
            }

            RowLayout {
                visible: offer.step === "busy"
                Layout.fillWidth: true
                spacing: 8

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Keep reading; it starts by itself.")
                    color: theme.ink4
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
                TextButton {
                    size: "compact"
                    text: qsTr("Cancel")
                    onClicked: {
                        voicePacks.cancel()
                        offer.pendingVoice = ""
                        offer.step = "idle"
                    }
                }
            }

            // --- State 3: done ---
            RowLayout {
                visible: offer.step === "done"
                Layout.fillWidth: true
                spacing: 8

                Glyph {
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16
                    stroke: 2.4
                    path: Icons.check
                    color: theme.positive
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Ready. Next time P just starts.")
                    color: theme.positiveInk
                    font.pixelSize: 13
                    font.weight: Font.Medium
                }
            }

            TextButton {
                visible: offer.step === "done"
                Layout.fillWidth: true
                primary: true
                size: "large"
                text: qsTr("Listen from this page")
                onClicked: {
                    voices.stopPreview()
                    offer.open = false
                    offer.step = "idle"
                    if (!narration.active && narration.hasPendingStart)
                        narration.startPending()
                    else if (narration.active && !narration.playing)
                        narration.resume()
                    else if (!narration.active && reader.open)
                        narration.start(reader.chapter, reader.pageStart)
                }
            }
        }
    }
}
