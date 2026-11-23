import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtWebChannel
import QtWebEngine

import "Format.js" as Format
import "Icons.js" as Icons
import "Themes.js" as Themes
import "Ui.js" as Ui

// Reading a book: the page edge to edge in the reading theme, a quiet
// running head and folio, and everything else one key away. The page itself
// is the web surface (reader.html), driven through `dogear`.
FocusScope {
    id: view

    property var settings
    readonly property var look: Themes.palette(settings.theme, theme)
    // For driving the panel from outside (and from tests).
    readonly property alias voicePanel: voicePanel
    readonly property bool panelOpen: contents.open || appearance.open || goTo.open || voicePanel.open
    property bool chromeShown: false
    property bool surfaceReady: false
    property bool listening: false

    signal closeRequested()

    Material.theme: look.dark ? Material.Dark : Material.Light
    Material.accent: look.link

    function js(code, callback) {
        if (callback)
            web.runJavaScript(code, callback)
        else
            web.runJavaScript(code)
    }
    function style() {
        const margins = { narrow: [0.035, 40], normal: [0.06, 34], wide: [0.1, 30] }[settings.margins] || [0.06, 34]
        return {
            theme: look,
            font: settings.font,
            fontScale: settings.fontScale,
            lineHeight: settings.lineHeight,
            margin: margins[0],
            measure: margins[1],
            justify: settings.justify,
            hyphenate: settings.hyphenate,
            layout: settings.layout,
            animate: settings.animate,
            publisher: settings.publisher,
            marginTop: 54,
            // Room for the listen bar under the text while listening.
            marginBottom: narration.active ? 84 : 54,
            radius: theme.radius,
            strings: { goToNote: qsTr("Go to note") },
        }
    }
    function openBook() {
        if (!surfaceReady || !reader.open)
            return
        js("dogear.open(" + JSON.stringify(reader.manifest) + "," + JSON.stringify(reader.startPosition) + ","
           + JSON.stringify(style()) + ")")
    }
    // Turning pages by hand while listening leaves the voice where it is;
    // "Follow along" brings the page back to it.
    function leaveVoice() {
        if (narration.active)
            narration.following = false
    }
    function next() { leaveVoice(); js("dogear.next()") }
    function prev() { leaveVoice(); js("dogear.prev()") }
    function nextChapter() { leaveVoice(); js("dogear.nextChapter()") }
    function prevChapter() { leaveVoice(); js("dogear.prevChapter()") }
    function back() { leaveVoice(); js("dogear.back()") }
    function goTo(target) { leaveVoice(); js("dogear.goTo(" + JSON.stringify(target) + ")") }

    // Listening starts from the top of the page in view.
    function listen() {
        if (narration.active) {
            narration.stop()
            return
        }
        js("JSON.stringify(dogear.listeningStart())", function(result) {
            const place = JSON.parse(result || "null")
            if (place)
                narration.start(place.chapter, place.offset)
        })
    }
    function closePanels() {
        contents.close()
        appearance.close()
        goTo.close()
        voicePanel.close()
    }
    function dismiss() {
        if (panelOpen) {
            closePanels()
            return
        }
        js("dogear.escape()", function(closed) {
            if (!closed)
                view.closeRequested()
        })
    }
    function cycleTheme() {
        const order = Themes.order
        settings.theme = order[(order.indexOf(settings.theme) + 1) % order.length]
        toast.show(qsTr("%1 theme").arg(Themes.name(settings.theme)))
    }
    function resize(step) {
        settings.fontScale = step === 0 ? 1.15 : Math.max(0.7, Math.min(2.0, Math.round((settings.fontScale + step) * 100) / 100))
    }
    function showContents() { contents.show() }
    function showAppearance() { appearance.show() }
    function showVoices() {
        contents.close()
        appearance.close()
        voicePanel.show()
    }
    function showChrome() {
        chromeShown = true
        chromeTimer.restart()
    }

    // Typography and theme changes reach the page together, once.
    Timer {
        id: styleTimer
        interval: 50
        onTriggered: if (view.surfaceReady && reader.open) view.js("dogear.setStyle(" + JSON.stringify(view.style()) + ")")
    }
    onLookChanged: styleTimer.restart()
    Connections {
        target: view.settings
        function onFontChanged() { styleTimer.restart() }
        function onFontScaleChanged() { styleTimer.restart() }
        function onLineHeightChanged() { styleTimer.restart() }
        function onMarginsChanged() { styleTimer.restart() }
        function onJustifyChanged() { styleTimer.restart() }
        function onHyphenateChanged() { styleTimer.restart() }
        function onLayoutChanged() { styleTimer.restart() }
        function onAnimateChanged() { styleTimer.restart() }
        function onPublisherChanged() { styleTimer.restart() }
    }

    Connections {
        target: narration
        function onBlocksWanted(chapter) { view.js("dogear.requestSpeakable(" + chapter + ")") }
        function onSpeaking(chapter, start, end) {
            if (chapter === reader.chapter)
                view.js("dogear.speak(" + start + "," + end + "," + narration.following + ")")
            else if (narration.following)
                view.js("dogear.goTo({chapter:" + chapter + ",offset:" + start + ",instant:true,quiet:true})"
                        + ".then(() => dogear.speak(" + start + "," + end + ",true))")
        }
        function onStateChanged() {
            if (narration.active !== view.listening) {
                view.listening = narration.active
                styleTimer.restart()
            }
            if (!narration.active)
                view.js("dogear.highlight('dg-speaking', [])")
        }
        function onNeedsKey() {
            toast.show(qsTr("Listening uses your Gemini key. Add it to start."))
            win.showAiSettings()
        }
    }

    Connections {
        target: reader
        function onSurfaceReady() {
            view.surfaceReady = true
            view.openBook()
        }
        function onBookChanged() {
            if (reader.open) {
                view.closePanels()
                view.chromeShown = false
                view.openBook()
            }
        }
        function onTap(zone) {
            if (zone === "center")
                view.chromeShown ? (view.chromeShown = false) : view.showChrome()
        }
        function onExternalLink(url) {
            toast.show(qsTr("Opening %1 in your browser").arg(url.replace(/^https?:\/\//, "").split("/")[0]))
            Qt.openUrlExternally(url)
        }
        function onKey(key, modifiers) { view.pageKey(key, modifiers) }
    }

    // Keys typed while the page had the focus come back here.
    function pageKey(key, modifiers) {
        const shift = modifiers & 1, ctrl = modifiers & 2, alt = modifiers & 4
        if (key === "ArrowRight" || key === "PageDown" || key === "l" || key === "j" || (key === " " && !shift)) next()
        else if (key === "ArrowLeft" || key === "PageUp" || key === "h" || key === "k" || (key === " " && shift)) prev()
        else if (key === "Escape") dismiss()
        else if (key === "]") nextChapter()
        else if (key === "[") prevChapter()
        else if (key === "t" || key === "T") contents.show()
        else if (key === "a" || key === "A") appearance.show()
        else if (key === "g" || key === "G") goTo.show()
        else if (key === "d" || key === "D") cycleTheme()
        else if (key === "+" || key === "=") resize(0.05)
        else if (key === "-") resize(-0.05)
        else if (key === "0") resize(0)
        else if (key === "f" || key === "F" || key === "F11") win.toggleFullScreen()
        else if (key === "?") win.helpVisible = !win.helpVisible
        else if ((key === "q" || key === "Q") && !ctrl) Qt.quit()
        else if (alt && key === "ArrowLeft") back()
    }

    Rectangle {
        anchors.fill: parent
        color: view.look.bg
    }

    WebEngineView {
        id: web
        anchors.fill: parent
        profile: readerProfile
        backgroundColor: view.look.bg
        activeFocusOnPress: false
        focus: false
        webChannel: channel
        url: "dogear://local/app/reader.html"
        settings.javascriptCanOpenWindows: false
        settings.localContentCanAccessRemoteUrls: false
        settings.localContentCanAccessFileUrls: false
        settings.pluginsEnabled: false
        settings.pdfViewerEnabled: false
        settings.focusOnNavigationEnabled: false
        settings.showScrollBars: false
        settings.errorPageEnabled: false
        settings.spatialNavigationEnabled: false
        settings.linksIncludedInFocusChain: false
        onContextMenuRequested: (request) => request.accepted = true
        onNewWindowRequested: (request) => {}
        onNavigationRequested: (request) => {
            if (!request.url.toString().startsWith("dogear://"))
                request.reject()
        }
        onJavaScriptConsoleMessage: (level, message, line, source) => {
            if (level === WebEngineView.ErrorMessageLevel || debugMode)
                console.warn(source.split("/").pop() + ":" + line + ": " + message)
        }
        onLoadingChanged: (request) => {
            if (debugMode || request.status === WebEngineView.LoadFailedStatus)
                console.warn("reader surface:", request.status, request.url, request.errorString)
        }
        onRenderProcessTerminated: (status, code) => {
            view.surfaceReady = false
            reload()
        }
    }

    WebChannel {
        id: channel
        Component.onCompleted: registerObjects({ bridge: reader })
    }

    // In scroll mode the text passes under the running head and folio;
    // it fades into the page there rather than colliding with them.
    Rectangle {
        visible: view.settings.layout === "scroll"
        anchors.top: parent.top
        width: parent.width
        height: 52
        gradient: Gradient {
            GradientStop { position: 0.45; color: view.look.bg }
            GradientStop { position: 1; color: Ui.tint(view.look.bg, 0) }
        }
    }
    Rectangle {
        visible: view.settings.layout === "scroll"
        anchors.bottom: parent.bottom
        width: parent.width
        height: 52
        gradient: Gradient {
            GradientStop { position: 0; color: Ui.tint(view.look.bg, 0) }
            GradientStop { position: 0.55; color: view.look.bg }
        }
    }

    // --- Running head and folio -------------------------------------------
    Label {
        anchors.top: parent.top
        anchors.topMargin: 18
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width * 0.6, 640)
        horizontalAlignment: Text.AlignHCenter
        opacity: view.chromeShown ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: 160 } }
        text: reader.chapterTitle
        color: view.look.muted
        font.pixelSize: 12
        font.letterSpacing: 0.4
        elide: Text.ElideRight
    }

    Item {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: 28
        anchors.rightMargin: 28
        height: 44
        opacity: view.chromeShown || narration.active ? 0 : 1
        Behavior on opacity { NumberAnimation { duration: 160 } }

        Label {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            text: reader.minutesLeftInChapter > 0 && translator.language !== ""
                  ? qsTr("%1 left in chapter").arg(Format.duration(reader.minutesLeftInChapter)) : ""
            color: view.look.muted
            font.pixelSize: 12
        }
        Label {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: {
                const percent = qsTr("%1%").arg(Math.round(reader.progress * 100).toLocaleString(Qt.locale(translator.language), 'f', 0))
                return reader.pageLabel !== "" ? qsTr("p. %1").arg(reader.pageLabel) + "  ·  " + percent : percent
            }
            color: view.look.muted
            font.pixelSize: 12
        }
    }

    // --- Chrome ------------------------------------------------------------
    Timer {
        id: chromeTimer
        interval: 3200
        onTriggered: if (!chromeHover.hovered && !scrubberHover.hovered) view.chromeShown = false
    }
    // The chrome comes when the pointer moves to the top edge, not merely
    // because it rests there when a book opens.
    property real lastPointerY: -1
    HoverHandler {
        id: topEdge
        onPointChanged: {
            const y = point.position.y
            if (view.lastPointerY >= 0 && y !== view.lastPointerY && y < 64 && !view.panelOpen)
                view.showChrome()
            view.lastPointerY = y
        }
    }

    Rectangle {
        id: chrome
        anchors.left: parent.left
        anchors.right: parent.right
        height: 60
        y: view.chromeShown ? 0 : -height - 4
        Behavior on y { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        color: view.look.popover
        Rectangle { anchors.bottom: parent.bottom; width: parent.width; height: 1; color: view.look.border }
        HoverHandler { id: chromeHover }

        Row {
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            IconButton {
                glyph: Icons.back
                tone: view.look.fg
                tip: qsTr("Library (Esc)")
                onClicked: view.closeRequested()
            }
            IconButton {
                glyph: Icons.contents
                tone: view.look.fg
                tip: qsTr("Contents (T)")
                active: contents.open
                onClicked: contents.open ? contents.close() : contents.show()
            }
        }

        Column {
            anchors.centerIn: parent
            width: Math.min(parent.width - 420, 640)
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: reader.title
                color: view.look.fg
                font.pixelSize: 14
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
            Label {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: reader.author
                color: view.look.muted
                font.pixelSize: 12
                elide: Text.ElideRight
            }
        }

        Row {
            anchors.right: parent.right
            anchors.rightMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            IconButton {
                tone: view.look.fg
                tip: qsTr("Appearance (A)")
                active: appearance.open
                contentItem: Label {
                    text: "Aa"
                    font.family: "Literata"
                    font.pixelSize: 16
                    color: appearance.open ? view.look.link : view.look.fg
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: appearance.open ? appearance.close() : appearance.show()
            }
            IconButton {
                glyph: Icons.fullscreen
                tone: view.look.fg
                tip: qsTr("Full screen (F)")
                onClicked: win.toggleFullScreen()
            }
        }
    }

    Rectangle {
        id: bottomChrome
        anchors.left: parent.left
        anchors.right: parent.right
        height: 72
        y: view.chromeShown ? parent.height - height : parent.height + 4
        Behavior on y { NumberAnimation { duration: 180; easing.type: Easing.OutCubic } }
        color: view.look.popover
        Rectangle { width: parent.width; height: 1; color: view.look.border }
        HoverHandler { id: scrubberHover }

        Label {
            anchors.left: parent.left
            anchors.leftMargin: 28
            anchors.top: parent.top
            anchors.topMargin: 10
            text: reader.chapterTitle
            color: view.look.fg
            font.pixelSize: 12
            elide: Text.ElideRight
            width: parent.width * 0.6
        }
        Label {
            anchors.right: parent.right
            anchors.rightMargin: 28
            anchors.top: parent.top
            anchors.topMargin: 10
            text: reader.minutesLeftInBook > 0 && translator.language !== ""
                  ? qsTr("%1 left in book").arg(Format.duration(reader.minutesLeftInBook)) : ""
            color: view.look.muted
            font.pixelSize: 12
        }
        ProgressScrubber {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.leftMargin: 28
            anchors.rightMargin: 28
            anchors.bottomMargin: 2
            height: 44
            look: view.look
            onGo: (target) => view.goTo(target)
        }
    }

    ListenBar {
        id: listenBar
        // Centered in the room the side panels leave.
        readonly property real leftInset: contents.open ? contents.width : 0
        readonly property real rightInset: voicePanel.open ? voicePanel.width : appearance.open ? appearance.width : 0
        x: leftInset + (parent.width - leftInset - rightInset - width) / 2
        Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        y: narration.active ? parent.height - height - (view.chromeShown ? 84 : 14) : parent.height + 10
        Behavior on y { NumberAnimation { duration: 200; easing.type: Easing.OutCubic } }
        visible: y < parent.height
        look: view.look
        onFollowRequested: narration.following = true
        onVoiceRequested: voicePanel.open ? voicePanel.close() : view.showVoices()
    }

    ContentsPanel {
        id: contents
        height: parent.height
        look: view.look
        onChosen: (chapter, fragment) => view.goTo({ chapter: chapter, fragment: fragment, instant: true })
    }

    VoicePanel {
        id: voicePanel
        height: parent.height
        look: view.look
        onSampleWanted: view.js("dogear.sampleText()", function(text) {
            voicePanel.sample = text || ""
            if (debugMode)
                console.log("voice sample:", voicePanel.sample)
        })
    }

    AppearancePanel {
        id: appearance
        height: parent.height
        look: view.look
        settings: view.settings
    }

    GoToBox {
        id: goTo
        anchors.horizontalCenter: parent.horizontalCenter
        y: 96
        look: view.look
        onGo: (target) => view.goTo(target)
    }

    // --- Keys ----------------------------------------------------------------
    readonly property bool keys: visible && !panelOpen && !win.helpVisible
    Shortcut { sequences: ["Right", "PgDown", "L", "J", "Space"]; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.next() }
    Shortcut { sequences: ["Left", "PgUp", "H", "K", "Shift+Space"]; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.prev() }
    Shortcut { sequence: "]"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.nextChapter() }
    Shortcut { sequence: "["; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.prevChapter() }
    Shortcut { sequence: "Alt+Left"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.back() }
    Shortcut { sequence: "T"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: contents.show() }
    Shortcut { sequence: "A"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: appearance.show() }
    Shortcut { sequence: "G"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: goTo.show() }
    Shortcut { sequence: "D"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.cycleTheme() }
    Shortcut { sequences: ["+", "=", "Ctrl++", "Ctrl+="]; context: Qt.ApplicationShortcut; enabled: view.visible; onActivated: view.resize(0.05) }
    Shortcut { sequences: ["-", "Ctrl+-"]; context: Qt.ApplicationShortcut; enabled: view.visible; onActivated: view.resize(-0.05) }
    Shortcut { sequences: ["0", "Ctrl+0"]; context: Qt.ApplicationShortcut; enabled: view.visible; onActivated: view.resize(0) }
    Shortcut { sequence: "Tab"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.chromeShown ? (view.chromeShown = false) : view.showChrome() }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: view.visible && !win.helpVisible; onActivated: view.dismiss() }
    // Listening.
    Shortcut { sequence: "L"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.listen() }
    Shortcut { sequence: "V"; context: Qt.ApplicationShortcut; enabled: view.keys; onActivated: view.showVoices() }
    Shortcut { sequence: "P"; context: Qt.ApplicationShortcut; enabled: view.keys && narration.active; onActivated: narration.toggle() }
    Shortcut { sequence: ","; context: Qt.ApplicationShortcut; enabled: view.keys && narration.active; onActivated: narration.previousParagraph() }
    Shortcut { sequence: "."; context: Qt.ApplicationShortcut; enabled: view.keys && narration.active; onActivated: narration.nextParagraph() }
    Shortcut { sequences: ["<", "Shift+,"]; context: Qt.ApplicationShortcut; enabled: view.keys && narration.active; onActivated: narration.rate = narration.rate - 0.1 }
    Shortcut { sequences: [">", "Shift+."]; context: Qt.ApplicationShortcut; enabled: view.keys && narration.active; onActivated: narration.rate = narration.rate + 0.1 }
    Shortcut { sequence: "Media Play"; context: Qt.ApplicationShortcut; enabled: narration.active; onActivated: narration.resume() }
    Shortcut { sequence: "Media Pause"; context: Qt.ApplicationShortcut; enabled: narration.active; onActivated: narration.pause() }
    Shortcut { sequence: "Toggle Media Play/Pause"; context: Qt.ApplicationShortcut; enabled: narration.active; onActivated: narration.toggle() }
}
