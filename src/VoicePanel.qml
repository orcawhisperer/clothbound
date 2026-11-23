import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

import "Format.js" as Format
import "Icons.js" as Icons
import "Ui.js" as Ui

// Who reads the book, and how. The narrator comes from the voices that speak
// the book's language, the classic voices, the reader's own, or one designed
// for this book, each heard reading the page before it's chosen; the style
// says how much the reading follows the mood of the story.
FocusScope {
    id: panel

    property bool open: false
    property var look
    // A passage from the page in view, for previews.
    property string sample: ""
    property string query: ""
    // Narration paused for a preview picks up again when the panel closes.
    property bool resumeAfter: false
    property string draftName: ""
    property string draftDescription: ""

    signal closed()
    signal sampleWanted()

    // Which voices the panel shows: "builtin" or "gemini".
    property string source: "gemini"

    function show() {
        open = true
        query = ""
        source = builtinVoice || (!ai.hasKey && voicePacks.supported) ? "builtin" : "gemini"
        voices.refresh()
        sampleWanted()
        forceActiveFocus()
    }
    function close() {
        if (!open)
            return
        open = false
        voices.stopPreview()
        if (resumeAfter) {
            resumeAfter = false
            narration.resume()
        }
        closed()
    }
    function preview(id) {
        if (voices.previewing === id) {
            voices.stopPreview()
            return
        }
        if (narration.playing) {
            narration.pause()
            resumeAfter = true
        }
        voices.preview(id, sample)
    }
    function choose(voice) {
        voices.stopPreview()
        if (voice.kind === "builtin" && !voice.installed) {
            pendingVoice = voice.id
            voicePacks.installVoice(voice.id)
            return
        }
        narration.setVoice(voice.id, voice.title || voice.name)
    }
    function details(voice) {
        const parts = []
        if (voice.kind === "builtin") {
            parts.push(voice.engine, voice.languageName)
            if (voice.grade)
                parts.push(qsTr("grade %1").arg(voice.grade))
            else if (voice.quality)
                parts.push(voice.quality === "high" ? qsTr("high quality") : voice.quality === "medium" ? qsTr("medium quality")
                                                                                                       : qsTr("low quality"))
            const speed = voice.installed ? localSpeech.speedOf(voice.id) : 0
            if (!voice.installed)
                parts.push(qsTr("%1 MB to download").arg(Math.round(voice.bytes / 1e6).toLocaleString(Qt.locale(), 'f', 0)))
            else if (speed > 0 && translator.language !== "")
                parts.push(qsTr("%1× real time here").arg(speed.toLocaleString(Qt.locale(), 'f', 1)))
            return parts.join(" · ")
        }
        if (voice.kind === "designed") {
            parts.push(qsTr("Designed"))
            if (voice.expires && !isNaN(voice.expires.getTime()) && translator.language !== "")
                parts.push(qsTr("kept until %1").arg(voice.expires.toLocaleDateString(Qt.locale(), "MMM yyyy")))
            return parts.join(" · ")
        }
        const gender = voice.gender === "male" ? qsTr("male") : voice.gender === "female" ? qsTr("female") : ""
        const pitch = voice.pitch === "low" ? qsTr("low") : voice.pitch === "medium" ? qsTr("medium")
                    : voice.pitch === "high" ? qsTr("high") : ""
        if (gender || pitch)
            parts.push([gender, pitch].filter(word => word).join(", "))
        if (voice.age > 0)
            parts.push(qsTr("age %1").arg(voice.age))
        return parts.join(" · ")
    }

    // Downloading for offline listening: this chapter, or to the end.
    readonly property int lastChapter: reader.chapterCount - 1
    readonly property var chapterEstimate: open && voices.count >= 0 ? download.estimate(reader.chapter, reader.chapter) : ({})
    readonly property var restEstimate: open && voices.count >= 0 ? download.estimate(reader.chapter, lastChapter) : ({})
    function money(dollars) {
        return dollars < 0.01 ? qsTr("under $0.01") : qsTr("about %1").arg("$" + dollars.toLocaleString(Qt.locale(), 'f', 2))
    }
    readonly property bool downloadingThisBook: download.active && download.bookId === reader.bookId

    readonly property string language: reader.language
    // `count` changes whenever the voices do, which keeps these fresh.
    readonly property var suggestions: voices.count >= 0 ? voices.suggestions(language) : []
    readonly property var narrators: suggestions.filter(voice => voice.kind === "library")
    readonly property var classics: suggestions.filter(voice => voice.kind === "classic")
    readonly property var yours: voices.count >= 0 ? voices.designedVoices() : []
    readonly property var results: query.trim() !== "" && voices.count >= 0 ? voices.search(query, "", 80) : []
    readonly property var current: voices.count >= 0 && narration.voice
        ? (narration.voice.startsWith("builtin:") ? voicePacks.voice(narration.voice) : voices.voice(narration.voice)) : ({})

    // Built-in voices: made on this computer, downloaded on request.
    readonly property bool builtinVoice: (narration.voice || "").startsWith("builtin:")
    readonly property var builtins: open && voicePacks.megabytes >= 0 ? voicePacks.voices(language) : []
    readonly property var builtinInstalled: builtins.filter(voice => voice.installed)
    // The best few that are here (and the one reading), until asked for more.
    readonly property var builtinShown: allBuiltins ? builtins
        : builtinInstalled.filter(voice => voice.recommended || voice.id === narration.voice)
    readonly property var builtinOffer: open && voicePacks.megabytes >= 0 ? voicePacks.offer(language) : ({})
    property bool allBuiltins: false
    // Chosen while it downloads: it becomes the voice when it's ready.
    property string pendingVoice: ""
    Connections {
        target: voicePacks
        function onReady(voiceId) {
            if (voiceId !== panel.pendingVoice)
                return
            panel.pendingVoice = ""
            const voice = voicePacks.voice(voiceId)
            narration.setVoice(voice.id, voice.name)
        }
    }

    Connections {
        target: voices
        function onCastingChanged() {
            if (voices.casting === "proposed") {
                panel.draftName = voices.proposal.name || ""
                panel.draftDescription = voices.proposal.description || ""
            }
        }
    }

    Keys.onEscapePressed: close()

    width: 420
    x: open ? parent.width - width : parent.width + 24
    Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    visible: x < parent.width + 20

    Material.theme: look.dark ? Material.Dark : Material.Light
    Material.accent: look.link
    Material.roundedScale: theme.radius

    Rectangle {
        anchors.fill: parent
        color: panel.look.popover
        Rectangle { width: 1; height: parent.height; color: panel.look.border }
    }
    // The panel keeps its clicks and the wheel: nothing falls through to the page.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }
    Rectangle {
        anchors.right: parent.left
        width: 18
        height: parent.height
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: "transparent" }
            GradientStop { position: 1; color: Ui.tint("black", panel.look.dark ? 0.35 : 0.12) }
        }
    }

    component Heading: Label {
        color: panel.look.muted
        font.pixelSize: 12
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.8
        Layout.topMargin: 16
        Layout.fillWidth: true
        elide: Text.ElideRight
    }

    component Note: Label {
        Layout.fillWidth: true
        color: panel.look.muted
        font.pixelSize: 12
        wrapMode: Text.Wrap
    }

    component Choice: AbstractButton {
        id: choice
        property bool selected: false
        Layout.fillWidth: true
        implicitHeight: 34
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        background: Rectangle {
            radius: Math.min(6, theme.radius)
            color: choice.selected ? Ui.tint(panel.look.link, 0.2) : choice.hovered ? Ui.tint(panel.look.fg, 0.07) : Ui.tint(panel.look.fg, 0.035)
            border.width: choice.selected ? 1.5 : 0
            border.color: panel.look.link
        }
        contentItem: Label {
            text: choice.text
            color: panel.look.fg
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    component Action: AbstractButton {
        id: action
        property bool primary: false
        implicitHeight: 34
        implicitWidth: actionLabel.implicitWidth + 28
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        background: Rectangle {
            radius: Math.min(8, theme.radius)
            color: action.primary ? (action.hovered ? Qt.lighter(panel.look.link, 1.08) : panel.look.link)
                 : action.hovered ? Ui.tint(panel.look.fg, 0.1) : Ui.tint(panel.look.fg, 0.06)
            opacity: action.enabled ? 1 : 0.5
        }
        contentItem: Label {
            id: actionLabel
            text: action.text
            color: action.primary ? Ui.readableOn(panel.look.link) : panel.look.fg
            font.pixelSize: 13
            font.weight: action.primary ? Font.DemiBold : Font.Normal
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    // Plays a voice reading the page; a second press stops it.
    component Listen: AbstractButton {
        id: listen
        property string voiceId
        // What a press does instead of a preview of the page.
        property var press: null
        readonly property bool playing: voices.previewing === voiceId
        implicitWidth: 34
        implicitHeight: 34
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        background: Rectangle {
            radius: 17
            color: listen.playing ? Ui.tint(panel.look.link, 0.22) : listen.hovered ? Ui.tint(panel.look.fg, 0.1) : Ui.tint(panel.look.fg, 0.05)
        }
        contentItem: Item {
            Glyph {
                anchors.centerIn: parent
                anchors.horizontalCenterOffset: listen.playing ? 0 : 1
                width: 15
                height: 15
                visible: !(listen.playing && voices.previewBuffering)
                path: listen.playing ? Icons.pause : Icons.play
                stroke: 2.2
                color: listen.playing ? panel.look.link : panel.look.fg
            }
            BusyIndicator {
                anchors.centerIn: parent
                implicitWidth: 26
                implicitHeight: 26
                visible: listen.playing && voices.previewBuffering
                running: visible
            }
        }
        onClicked: press ? press() : panel.preview(voiceId)
        ToolTip.visible: hovered
        ToolTip.text: playing ? qsTr("Stop") : qsTr("Hear it read this page")
        ToolTip.delay: 600
    }

    component BuiltinVoices: ColumnLayout {
        Layout.fillWidth: true
        visible: voicePacks.supported
        spacing: 6


        // None yet for this book's language: the best one, ready to download.
        Rectangle {
            Layout.fillWidth: true
            Layout.topMargin: 4
            visible: panel.builtinInstalled.length === 0 && !!panel.builtinOffer.voice
            radius: Math.min(10, theme.radius)
            color: Ui.tint(panel.look.fg, 0.04)
            border.color: panel.look.border
            implicitHeight: offerColumn.implicitHeight + 24
            ColumnLayout {
                id: offerColumn
                x: 12
                y: 12
                width: parent.width - 24
                spacing: 8
                Label {
                    Layout.fillWidth: true
                    text: panel.builtinOffer.voice
                          ? qsTr("Read this book offline with %1 (%2, %3).").arg(panel.builtinOffer.voice.name)
                                .arg(panel.builtinOffer.voice.engine).arg(panel.builtinOffer.voice.languageName) : ""
                    color: panel.look.fg
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }
                Note {
                    visible: !voicePacks.busy
                    text: panel.builtinOffer.voice
                          ? qsTr("It downloads %1 MB once.").arg(Math.round(panel.builtinOffer.bytes / 1e6).toLocaleString(Qt.locale(), 'f', 0)) : ""
                }
                Action {
                    visible: !voicePacks.busy
                    primary: true
                    text: qsTr("Download")
                    onClicked: panel.choose(panel.builtinOffer.voice)
                }
            }
        }

        // A download under way.
        ColumnLayout {
            Layout.fillWidth: true
            visible: voicePacks.busy
            spacing: 6
            ProgressBar {
                Layout.fillWidth: true
                value: voicePacks.progress
            }
            RowLayout {
                Layout.fillWidth: true
                Note {
                    Layout.fillWidth: true
                    text: qsTr("Downloading %1…").arg(voicePacks.installing)
                }
                Action {
                    text: qsTr("Cancel")
                    onClicked: {
                        panel.pendingVoice = ""
                        voicePacks.cancel()
                    }
                }
            }
        }
        Label {
            Layout.fillWidth: true
            visible: voicePacks.problem !== ""
            text: voicePacks.problem
            color: theme.colors.orange || "#e5a050"
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        Note {
            visible: panel.builtins.length === 0
            text: qsTr("No built-in voice speaks this book's language yet.")
        }

        Repeater {
            model: panel.builtinShown
            delegate: VoiceRow {
                required property var modelData
                voice: modelData
            }
        }
        Action {
            visible: panel.builtins.length > panel.builtinShown.length || panel.allBuiltins
            text: panel.allBuiltins ? qsTr("Fewer")
                : qsTr("More built-in voices (%1)").arg(panel.builtins.length - panel.builtinShown.length)
            onClicked: panel.allBuiltins = !panel.allBuiltins
        }
    }

    component VoiceRow: AbstractButton {
        id: row
        property var voice: ({})
        readonly property bool chosen: narration.voice === voice.id
        // Deleting a designed voice is for good, so it takes a second press.
        property bool confirming: false
        Timer {
            running: row.confirming
            interval: 4000
            onTriggered: row.confirming = false
        }
        Layout.fillWidth: true
        implicitHeight: Math.max(54, rowColumn.implicitHeight + 16)
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        onClicked: panel.choose(voice)
        background: Rectangle {
            radius: Math.min(8, theme.radius)
            color: row.chosen ? Ui.tint(panel.look.link, 0.15) : row.hovered ? Ui.tint(panel.look.fg, 0.06) : "transparent"
            border.width: row.chosen ? 1.5 : 0
            border.color: panel.look.link
        }
        contentItem: RowLayout {
            spacing: 10
            ColumnLayout {
                id: rowColumn
                Layout.fillWidth: true
                Layout.leftMargin: 10
                spacing: 2
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        text: row.voice.title || row.voice.name || ""
                        color: panel.look.fg
                        font.pixelSize: 14
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                        Layout.maximumWidth: 180
                    }
                    Label {
                        Layout.fillWidth: true
                        text: panel.details(row.voice)
                        color: panel.look.muted
                        font.pixelSize: 12
                        elide: Text.ElideRight
                    }
                }
                Label {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: row.voice.summary || ""
                    color: panel.look.muted
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    maximumLineCount: 2
                    elide: Text.ElideRight
                }
            }
            AbstractButton {
                visible: row.voice.kind === "designed" && (row.hovered || row.confirming)
                implicitHeight: 30
                implicitWidth: row.confirming ? confirmLabel.implicitWidth + 20 : 30
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                background: Rectangle {
                    radius: Math.min(6, theme.radius)
                    color: row.confirming ? Ui.tint(theme.colors.red || "#e05d5d", 0.2) : parent.hovered ? Ui.tint(panel.look.fg, 0.1) : "transparent"
                }
                contentItem: Item {
                    Glyph {
                        anchors.centerIn: parent
                        visible: !row.confirming
                        width: 16
                        height: 16
                        path: Icons.trash
                        color: panel.look.muted
                    }
                    Label {
                        id: confirmLabel
                        anchors.centerIn: parent
                        visible: row.confirming
                        text: qsTr("Delete")
                        color: theme.colors.red || "#e05d5d"
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                }
                onClicked: {
                    if (!row.confirming) {
                        row.confirming = true
                        return
                    }
                    row.confirming = false
                    if (narration.voice === row.voice.id)
                        narration.setVoice("sulafat", "Sulafat")
                    voices.forget(row.voice.id)
                }
                ToolTip.visible: hovered && !row.confirming
                ToolTip.text: qsTr("Delete this voice")
                ToolTip.delay: 600
            }
            Listen {
                visible: !(row.voice.kind === "builtin" && !row.voice.installed)
                voiceId: row.voice.id || ""
                Layout.rightMargin: 8
            }
        }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: content.implicitHeight + 48
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: content
            x: 22
            y: 22
            width: panel.width - 44
            spacing: 6

            RowLayout {
                Layout.fillWidth: true
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Narrator")
                    color: panel.look.fg
                    font.pixelSize: 18
                    font.weight: Font.DemiBold
                }
                IconButton {
                    glyph: Icons.close
                    tone: panel.look.fg
                    tip: qsTr("Close (Esc)")
                    onClicked: panel.close()
                }
            }

            // The voice reading now.
            VoiceRow {
                visible: !!panel.current.id
                voice: panel.current
            }
            Note {
                visible: !panel.current.id && narration.voiceName !== ""
                text: narration.voiceName
            }

            // Where the voice comes from: made here, or Gemini's.
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 6
                spacing: 8
                visible: voicePacks.supported
                Choice {
                    Layout.preferredWidth: 1
                    text: qsTr("On this computer")
                    selected: panel.source === "builtin"
                    onClicked: panel.source = "builtin"
                }
                Choice {
                    Layout.preferredWidth: 1
                    text: qsTr("Gemini")
                    selected: panel.source === "gemini"
                    onClicked: panel.source = "gemini"
                }
            }
            Note {
                visible: voicePacks.supported
                text: panel.source === "builtin" ? qsTr("Free, private, and offline. Plainer than Gemini's voices, and without styles.")
                    : qsTr("The most natural voices, with styles; they use your own Gemini key.")
            }

            // The voices made here.
            BuiltinVoices {
                visible: voicePacks.supported && panel.source === "builtin"
            }

            // Gemini's voices.
            ColumnLayout {
                id: geminiSide
                Layout.fillWidth: true
                visible: panel.source === "gemini" || !voicePacks.supported
                spacing: 6

                // Without a key, a way to add one.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    visible: !ai.hasKey
                    radius: Math.min(10, theme.radius)
                    color: Ui.tint(panel.look.fg, 0.04)
                    border.color: panel.look.border
                    implicitHeight: keyColumn.implicitHeight + 24
                    ColumnLayout {
                        id: keyColumn
                        x: 12
                        y: 12
                        width: parent.width - 24
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Gemini's voices need your own key. It's free to get, and you choose what it costs.")
                            color: panel.look.fg
                            font.pixelSize: 13
                            wrapMode: Text.Wrap
                        }
                        Action {
                            primary: true
                            text: qsTr("Add your key")
                            onClicked: {
                                panel.close()
                                win.showAiSettings()
                            }
                        }
                    }
                }

                Heading { text: qsTr("Style") }
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Choice { text: qsTr("Plain"); selected: panel.builtinVoice || narration.performance === "plain"; onClicked: narration.performance = "plain" }
                    Choice { text: qsTr("Natural"); enabled: !panel.builtinVoice; selected: !panel.builtinVoice && narration.performance === "natural"; onClicked: narration.performance = "natural" }
                    Choice { text: qsTr("Dramatic"); enabled: !panel.builtinVoice; selected: !panel.builtinVoice && narration.performance === "dramatic"; onClicked: narration.performance = "dramatic" }
                }
                Note {
                    text: panel.builtinVoice ? qsTr("Built-in voices read evenly; the other styles need a Gemini voice.")
                        : narration.performance === "plain" ? qsTr("An even, clear reading with no direction. Good for study and reference.")
                        : narration.performance === "dramatic" ? qsTr("A fuller performance, for adventure, horror and genre fiction.")
                        : qsTr("A storyteller's reading: the mood follows the scene, never acted.")
                }

                // Casting: a narrator designed for this book.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 14
                    visible: ai.canDesignVoices
                    radius: Math.min(10, theme.radius)
                    color: Ui.tint(panel.look.link, 0.07)
                    border.color: Ui.tint(panel.look.link, 0.28)
                    implicitHeight: cast.implicitHeight + 28

                    ColumnLayout {
                        id: cast
                        x: 14
                        y: 14
                        width: parent.width - 28
                        spacing: 8

                        RowLayout {
                            spacing: 8
                            Glyph {
                                width: 18
                                height: 18
                                path: Icons.sparkle
                                color: panel.look.link
                            }
                            Label {
                                Layout.fillWidth: true
                                text: voices.casting === "designed" ? (voices.designed.name || "") : qsTr("A narrator for this book")
                                color: panel.look.fg
                                font.pixelSize: 14
                                font.weight: Font.DemiBold
                                elide: Text.ElideRight
                            }
                            Listen {
                                visible: voices.casting === "designed"
                                voiceId: voices.designed.id || ""
                                press: () => voices.previewing === voiceId ? voices.stopPreview() : voices.playSample()
                            }
                        }

                        Note {
                            visible: voices.casting === ""
                            text: qsTr("Gemini reads a passage, describes a narrator to suit the book, and designs the voice. About $0.03, and half a minute.")
                        }
                        Action {
                            visible: voices.casting === ""
                            primary: true
                            text: qsTr("Design a narrator")
                            onClicked: voices.cast(reader.passageFrom(reader.chapter, 4000))
                        }

                        RowLayout {
                            visible: voices.casting === "thinking" || voices.casting === "designing"
                            spacing: 10
                            BusyIndicator {
                                implicitWidth: 28
                                implicitHeight: 28
                                running: parent.visible
                            }
                            Note {
                                text: voices.casting === "thinking" ? qsTr("Reading the book…")
                                    : qsTr("Designing “%1”. This takes about half a minute.").arg(panel.draftName)
                            }
                        }

                        TextField {
                            Layout.fillWidth: true
                            visible: voices.casting === "proposed"
                            text: panel.draftName
                            placeholderText: qsTr("Name")
                            color: panel.look.fg
                            font.pixelSize: 14
                            onTextEdited: panel.draftName = text
                        }
                        TextArea {
                            Layout.fillWidth: true
                            visible: voices.casting === "proposed"
                            text: panel.draftDescription
                            placeholderText: qsTr("How the voice sounds")
                            wrapMode: TextArea.Wrap
                            color: panel.look.fg
                            font.pixelSize: 13
                            onTextChanged: if (activeFocus) panel.draftDescription = text
                        }
                        Note {
                            visible: voices.casting === "proposed"
                            text: [voices.proposal.gender === "male" ? qsTr("Male voice") : voices.proposal.gender === "female" ? qsTr("Female voice") : "",
                                   voices.proposal.language ? Qt.locale(voices.proposal.language).nativeLanguageName + " (" + voices.proposal.language + ")" : ""]
                                  .filter(part => part).join(" · ")
                        }
                        RowLayout {
                            visible: voices.casting === "proposed"
                            spacing: 8
                            Action {
                                primary: true
                                text: qsTr("Design this voice")
                                enabled: panel.draftDescription.trim() !== ""
                                onClicked: voices.design({ name: panel.draftName, description: panel.draftDescription,
                                                           gender: voices.proposal.gender, language: voices.proposal.language })
                            }
                            Action {
                                text: qsTr("Suggest another")
                                onClicked: voices.cast(reader.passageFrom(reader.chapter, 4000))
                            }
                        }

                        Note {
                            visible: voices.casting === "designed"
                            text: voices.designed.summary || ""
                            maximumLineCount: 3
                            elide: Text.ElideRight
                        }
                        RowLayout {
                            visible: voices.casting === "designed"
                            spacing: 8
                            Action {
                                primary: true
                                text: narration.voice === voices.designed.id ? qsTr("Reading the book") : qsTr("Use this voice")
                                enabled: narration.voice !== voices.designed.id
                                onClicked: panel.choose(voices.designed)
                            }
                            Action {
                                text: qsTr("Try again")
                                onClicked: voices.design({ name: panel.draftName, description: panel.draftDescription,
                                                           gender: voices.proposal.gender, language: voices.proposal.language })
                            }
                            Action {
                                text: qsTr("Change it")
                                onClicked: voices.revise()
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            visible: voices.casting === "failed"
                            text: voices.castingProblem
                            color: theme.colors.orange || "#e5a050"
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                        Action {
                            visible: voices.casting === "failed"
                            text: qsTr("Try again")
                            onClicked: voices.cast(reader.passageFrom(reader.chapter, 4000))
                        }
                    }
                }

                // Gemini's narration, downloaded to hear offline (built-in voices
                // are offline already).
                readonly property bool offlineApplies: !panel.builtinVoice && ai.canSpeak
                Heading {
                    visible: parent.offlineApplies || panel.downloadingThisBook
                    text: qsTr("Offline")
                }
                Note {
                    visible: parent.offlineApplies && !panel.downloadingThisBook
                    text: qsTr("Download the narration to listen without a connection. It's made at Gemini's slower Flex tier, for half the price.")
                }
                RowLayout {
                    visible: parent.offlineApplies && !panel.downloadingThisBook
                    Layout.fillWidth: true
                    spacing: 8
                    Action {
                        text: qsTr("This chapter")
                        enabled: (panel.chapterEstimate.chapters || 0) > 0
                        onClicked: download.start(reader.chapter, reader.chapter)
                    }
                    Note {
                        Layout.fillWidth: true
                        text: (panel.chapterEstimate.chapters || 0) > 0 && translator.language !== ""
                              ? Format.duration(panel.chapterEstimate.minutes) + " · " + panel.money(panel.chapterEstimate.cost) : ""
                    }
                }
                RowLayout {
                    visible: parent.offlineApplies && !panel.downloadingThisBook && (panel.restEstimate.chapters || 0) > 1
                    Layout.fillWidth: true
                    spacing: 8
                    Action {
                        text: qsTr("To the end")
                        onClicked: download.start(reader.chapter, panel.lastChapter)
                    }
                    Note {
                        Layout.fillWidth: true
                        text: translator.language !== ""
                              ? qsTr("%n chapters", "", panel.restEstimate.chapters || 0) + " · " + Format.duration(panel.restEstimate.minutes || 0)
                                + " · " + panel.money(panel.restEstimate.cost || 0) : ""
                    }
                }
                Note {
                    visible: parent.offlineApplies && !panel.downloadingThisBook && (panel.restEstimate.cost || 0) > 0
                             && !ai.fitsBudget(panel.restEstimate.cost)
                    text: qsTr("Downloading to the end would pass your monthly budget; it stops there.")
                    color: theme.colors.orange || "#e5a050"
                }

                // A download under way, or done.
                ColumnLayout {
                    visible: panel.downloadingThisBook
                    Layout.fillWidth: true
                    spacing: 8
                    ProgressBar {
                        Layout.fillWidth: true
                        value: download.fraction
                        indeterminate: download.running && download.fraction === 0
                    }
                    Note {
                        text: download.finished ? (download.firstChapter === download.lastChapter
                                                   ? qsTr("This chapter is ready to listen offline.")
                                                   : qsTr("Ready to listen offline, to the end of the book."))
                            : translator.language === "" ? ""
                            : qsTr("%1 of %2 ready").arg(Format.duration(download.minutesReady)).arg(Format.duration(download.minutesTotal))
                              + (download.running && download.secondsLeft > 0
                                 ? " · " + qsTr("about %1 left").arg(Format.duration(download.secondsLeft / 60)) : "")
                              + (!download.running ? " · " + qsTr("paused") : "")
                    }
                    Label {
                        Layout.fillWidth: true
                        visible: download.problem !== ""
                        text: download.problem
                        color: theme.colors.orange || "#e5a050"
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                    RowLayout {
                        spacing: 8
                        Action {
                            visible: !download.finished
                            text: download.running ? qsTr("Pause") : qsTr("Go on")
                            onClicked: download.running ? download.pause() : download.resume()
                        }
                        Action {
                            text: download.finished ? qsTr("Done") : qsTr("Stop")
                            onClicked: download.cancel()
                        }
                    }
                }

                // Every voice, by any word about it: a name, an accent, a manner.
                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 16
                    implicitHeight: 36
                    radius: Math.min(8, theme.radius)
                    color: Ui.tint(panel.look.fg, searchInput.activeFocus ? 0.09 : 0.05)
                    border.width: searchInput.activeFocus ? 1.5 : 0
                    border.color: panel.look.link
                    Glyph {
                        id: searchIcon
                        anchors.left: parent.left
                        anchors.leftMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        width: 16
                        height: 16
                        path: Icons.search
                        color: panel.look.muted
                    }
                    TextInput {
                        id: searchInput
                        anchors.left: searchIcon.right
                        anchors.right: parent.right
                        anchors.leftMargin: 8
                        anchors.rightMargin: 10
                        anchors.verticalCenter: parent.verticalCenter
                        color: panel.look.fg
                        selectionColor: Ui.tint(panel.look.link, 0.4)
                        font.pixelSize: 14
                        clip: true
                        text: panel.query
                        onTextEdited: panel.query = text
                        Keys.onEscapePressed: (event) => {
                            if (text !== "") {
                                panel.query = ""
                                event.accepted = true
                            } else {
                                event.accepted = false
                            }
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            visible: !parent.text && !parent.activeFocus
                            text: voices.count > 0 ? qsTr("Search %Ln voices", "", voices.count) : qsTr("Search voices")
                            color: panel.look.muted
                            font.pixelSize: 14
                        }
                    }
                }

                Note {
                    visible: voices.loading || voices.problem !== ""
                    text: voices.loading ? qsTr("Fetching the voices…") : voices.problem
                    color: voices.problem !== "" ? (theme.colors.orange || "#e5a050") : panel.look.muted
                }

                // Search results take the place of the lists.
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: panel.query.trim() !== ""
                    spacing: 2
                    Note {
                        visible: panel.results.length === 0 && !voices.loading
                        Layout.topMargin: 6
                        text: qsTr("No voice matches.")
                    }
                    Repeater {
                        model: panel.results
                        delegate: VoiceRow {
                            required property var modelData
                            voice: modelData
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    visible: panel.query.trim() === ""
                    spacing: 2

                    Heading {
                        visible: panel.yours.length > 0
                        text: qsTr("Your voices")
                    }
                    Repeater {
                        model: panel.yours
                        delegate: VoiceRow {
                            required property var modelData
                            voice: modelData
                        }
                    }

                    Heading {
                        visible: panel.narrators.length > 0
                        text: qsTr("Narrators for this book")
                    }
                    Repeater {
                        model: panel.narrators
                        delegate: VoiceRow {
                            required property var modelData
                            voice: modelData
                        }
                    }

                    Heading {
                        visible: panel.classics.length > 0
                        text: qsTr("Classic voices")
                    }
                    Note {
                        visible: panel.classics.length > 0
                        text: qsTr("They read any language.")
                    }
                    Repeater {
                        model: panel.classics
                        delegate: VoiceRow {
                            required property var modelData
                            voice: modelData
                        }
                    }
                }
            }
        }
    }
}
