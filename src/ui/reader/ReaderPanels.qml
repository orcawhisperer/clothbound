import QtQuick
import "../audio"
import "../companion"

// Lazy-instantiated reading drawers, modals, and dimmed scrim shared across
// EPUB and PDF sessions so cold reader startup stays lean.
Item {
    id: root

    property var look
    property var activeSettings
    property bool pdf: false
    property var pdfSurface: null
    property Item readingStage: null
    property bool debugModeEnabled: false

    readonly property alias contents: contents
    readonly property alias voicePanel: voicePanel
    readonly property alias appearance: appearance
    readonly property alias noteEditor: noteEditor
    readonly property alias companionPanel: companionPanel
    readonly property alias marksPanel: marksPanel
    readonly property alias searchBar: searchBar
    readonly property alias goTo: goTo

    signal focusPageRequested()
    signal goToRequested(var target)
    signal sampleTextRequested(var callback)
    signal startListeningRequested()
    signal appearanceSettingChanged(string key, var value)
    signal pdfFitChosen(string mode)
    signal bookOverrideToggled(bool enabled)
    signal saveAsNoteRequested(string quote, string note)
    signal searchHitJumped(var hit)

    Loader {
        id: contents
        active: false
        readonly property var loaded: item
        readonly property bool open: loaded ? !!loaded.open : false
        readonly property bool resizing: loaded ? Boolean(loaded["resizing"]) : false
        width: loaded ? loaded.width : 0
        function show() {
            active = true
            if (loaded)
                loaded.show()
        }
        function close() {
            if (loaded)
                loaded.close()
        }
        sourceComponent: ContentsPanel {
            parent: root
            z: 10
            height: root.height
            look: root.look
            onClosed: root.focusPageRequested()
            onChosen: (chapter, fragment) => root.goToRequested({ chapter: chapter, fragment: fragment, instant: true })
        }
    }

    Loader {
        id: voicePanel
        active: false
        readonly property var loaded: item
        readonly property bool open: loaded ? !!loaded.open : false
        function show() {
            active = true
            if (loaded)
                loaded.show()
        }
        function close() {
            if (loaded)
                loaded.close()
        }
        sourceComponent: VoicePanel {
            id: voicePanelInner
            parent: root
            z: 12
            height: root.height
            look: root.look
            onClosed: root.focusPageRequested()
            onSampleWanted: root.sampleTextRequested(function(text) {
                voicePanelInner.sample = text || ""
                if (root.debugModeEnabled)
                    console.log("voice sample:", voicePanelInner.sample)
            })
            onListenRequested: {
                voicePanelInner.close()
                root.startListeningRequested()
            }
        }
    }

    Loader {
        id: appearance
        active: false
        readonly property var loaded: item
        readonly property bool open: loaded ? !!loaded.open : false
        readonly property bool resizing: loaded ? Boolean(loaded["resizing"]) : false
        width: loaded ? loaded.width : 0
        function show() {
            active = true
            if (loaded)
                loaded.show()
        }
        function close() {
            if (loaded)
                loaded.close()
        }
        sourceComponent: AppearancePanel {
            parent: root
            z: 10
            height: root.height
            look: root.look
            settings: root.activeSettings
            pdf: root.pdf
            pdfSurface: root.pdfSurface
            bookOverride: reader.bookAppearanceOverride
            onClosed: root.focusPageRequested()
            onSettingChanged: (key, value) => root.appearanceSettingChanged(key, value)
            onPdfFitChosen: (mode) => root.pdfFitChosen(mode)
            onBookOverrideToggled: (enabled) => root.bookOverrideToggled(enabled)
        }
    }

    Loader {
        id: noteEditor
        active: false
        readonly property var loaded: item
        readonly property bool open: loaded ? !!loaded.open : false
        function show(id) {
            active = true
            if (loaded)
                loaded.show(id)
        }
        function close() {
            if (loaded)
                loaded.close()
        }
        sourceComponent: NoteEditor {
            parent: root
            anchors.centerIn: root
            z: 14
            onClosed: root.focusPageRequested()
            look: root.look
        }
    }

    Loader {
        id: companionPanel
        active: false
        readonly property var loaded: item
        readonly property bool open: loaded ? !!loaded.open : false
        readonly property bool settledOpen: loaded ? !!loaded.settledOpen : false
        readonly property bool sliding: loaded ? Boolean(loaded["sliding"]) : false
        readonly property real slideProgress: loaded ? Number(loaded["slideProgress"] || 0) : 0
        readonly property bool resizing: loaded ? Boolean(loaded["resizing"]) : false
        width: loaded ? loaded.width : 0
        property bool pendingOpen: false
        property string pendingTab: "ask"
        property bool pendingFocus: false
        function show(tab, focusInput) {
            if (active && loaded) {
                pendingOpen = false
                loaded.show(tab, !!focusInput)
                return
            }
            pendingTab = tab || "ask"
            pendingFocus = !!focusInput
            pendingOpen = true
            active = true
            // Start the first slide-in on the next frame so initial QML
            // instantiation never eats the beginning of the slide animation.
            Qt.callLater(() => {
                if (pendingOpen && loaded) {
                    pendingOpen = false
                    loaded.show(pendingTab, pendingFocus)
                }
            })
        }
        function close() {
            pendingOpen = false
            if (loaded)
                loaded.close()
        }
        sourceComponent: CompanionPanel {
            parent: root
            z: 10
            height: root.height
            look: root.look
            onClosed: root.focusPageRequested()
            onJumpRequested: (chapter, offset) => root.goToRequested({ chapter: chapter, offset: offset, instant: true })
            onSaveAsNote: (quote, note) => root.saveAsNoteRequested(quote, note)
        }
    }

    Loader {
        id: marksPanel
        active: false
        readonly property var loaded: item
        readonly property bool open: loaded ? !!loaded.open : false
        readonly property bool resizing: loaded ? Boolean(loaded["resizing"]) : false
        width: loaded ? loaded.width : 0
        function show() {
            active = true
            if (loaded)
                loaded.show()
        }
        function close() {
            if (loaded)
                loaded.close()
        }
        sourceComponent: MarksPanel {
            parent: root
            z: 10
            height: root.height
            look: root.look
            onClosed: root.focusPageRequested()
            onChosen: (chapter, offset) => root.goToRequested({ chapter: chapter, offset: offset, instant: true })
        }
    }

    Loader {
        id: searchBar
        active: false
        readonly property var loaded: item
        readonly property bool open: loaded ? !!loaded.open : false
        readonly property bool editing: loaded ? !!loaded.editing : false
        function show() {
            active = true
            if (loaded)
                loaded.show()
        }
        function close() {
            if (loaded)
                loaded.close()
        }
        function next() {
            if (loaded)
                loaded.next()
        }
        function previous() {
            if (loaded)
                loaded.previous()
        }
        sourceComponent: SearchBar {
            parent: root
            anchors.horizontalCenter: root.readingStage ? root.readingStage.horizontalCenter : root.horizontalCenter
            y: 64
            z: 14
            onClosed: root.focusPageRequested()
            look: root.look
            onJumped: (hit) => root.searchHitJumped(hit)
        }
    }

    Loader {
        id: goTo
        active: false
        readonly property var loaded: item
        readonly property bool open: loaded ? !!loaded.open : false
        function show() {
            active = true
            if (loaded)
                loaded.show()
        }
        function close() {
            if (loaded)
                loaded.close()
        }
        sourceComponent: GoToBox {
            parent: root
            anchors.horizontalCenter: root.readingStage ? root.readingStage.horizontalCenter : root.horizontalCenter
            y: 96
            z: 14
            onClosed: root.focusPageRequested()
            look: root.look
            onGo: (target) => root.goToRequested(target)
        }
    }
}
