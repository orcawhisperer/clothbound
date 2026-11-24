import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Shapes
import "components"

import "components/Icons.js" as Icons
import "components/Ui.js" as Ui

// First-launch 800×480 split welcome dialog (08_welcome.html): a 320 px warm
// paper illustration pane on the left for all three steps, and Literata 30 px
// titles, tactile keycaps, and ghost Skip / secondary action / primary CTA on
// the right. Also serves the post-update "What's new" card.
FocusScope {
    id: welcome

    property bool open: false
    // "welcome" or "whatsnew".
    property string mode: "welcome"
    property int page: 0

    signal closed()
    signal addBooks()
    signal showIntelligence()

    readonly property var pages: [
        {
            kicker: qsTr("Welcome"),
            title: qsTr("Your books, and a calm page to read them on"),
            lead: qsTr("EPUB and PDF in one library. Clothbound only reads your files; it never changes them."),
            points: [
                { key: "+", text: qsTr("Add the folder your books live in. New ones there join by themselves.") },
                { key: "⇧", text: qsTr("Or drop books and folders anywhere on the window.") }
            ],
            action: qsTr("Add a folder…")
        },
        {
            kicker: qsTr("Read, and listen"),
            title: qsTr("Turn the page, or let it be read to you"),
            lead: qsTr("Everything works from the keyboard, and with a mouse or touchpad too."),
            points: [
                { key: "Space", text: qsTr("Next page. Select words to highlight, note or look them up.") },
                { key: "P", text: qsTr("Read aloud from here, the sentence shown as it's read. A free voice is made on this computer.") },
                { key: "?", text: qsTr("Every key, wherever you are.") }
            ],
            action: ""
        },
        {
            kicker: qsTr("Going further"),
            title: qsTr("A companion that never reads ahead"),
            lead: qsTr("Ask about the book, recap where you left off, see who's who. With Gemini and your own key, or a model on this computer."),
            points: [
                { key: "C", text: qsTr("Open the companion. It knows the book only up to your page.") },
                { key: "✦", text: qsTr("Settings › Intelligence sets up a model here, or finds yours.") }
            ],
            action: qsTr("Set up Intelligence")
        }
    ]
    readonly property var news: [
        qsTr("Add from URL (Ctrl+Shift+U): save web articles offline as EPUBs on a new Articles shelf, or fetch arXiv papers, EPUBs, and PDFs directly."),
        qsTr("OPDS catalogs (Ctrl+Shift+O): browse free public-domain libraries or connect your own Calibre-Web, Kavita, or Komga server."),
        qsTr("Adaptive Story & Study Companion (C) with Smart Scope, offline LaTeX math, spoken replies, and a resizable panel."),
        qsTr("Visual PDF Snip & Explain (X) with Gemini or local Ollama vision models, plus Quick-Look citation peek and BibTeX/APA copy."),
        qsTr("Built-in local MCP server (clothbound mcp) so your own coding or research agents can search your offline library privately."),
        qsTr("Zero-flash page turns, soft page-turn sound, slim floating listening pill, and snappy scroll physics.")
    ]

    function show(which) {
        mode = which
        page = 0
        open = true
        forceActiveFocus()
    }
    function close() {
        if (!open)
            return
        open = false
        closed()
    }
    function next() {
        if (mode === "welcome" && page < pages.length - 1)
            page++
        else
            close()
    }

    visible: opacity > 0
    opacity: open ? 1 : 0
    Behavior on opacity { NumberAnimation { duration: 150; easing.type: Easing.OutCubic } }

    Keys.onEscapePressed: close()
    Keys.onReturnPressed: next()
    Keys.onEnterPressed: next()
    Keys.onSpacePressed: next()
    Keys.onRightPressed: next()
    Keys.onLeftPressed: if (page > 0) page--

    // Scrim
    Rectangle {
        anchors.fill: parent
        color: theme.scrim
        MouseArea { anchors.fill: parent; acceptedButtons: Qt.AllButtons; onWheel: (wheel) => wheel.accepted = true }
    }

    // Floating modal frame with soft shadow.
    Item {
        id: frame
        anchors.centerIn: parent
        readonly property bool split: welcome.mode === "welcome" && parent.width >= 760
        width: split ? Math.min(800, parent.width - 48) : Math.min(560, parent.width - 48)
        height: split ? Math.min(480, parent.height - 48)
                      : Math.min(parent.height - 48, Math.max(400, copyBody.implicitHeight + footer.implicitHeight + 92))
        scale: welcome.open || theme.reducedMotion ? 1.0 : 0.98
        Behavior on scale {
            enabled: !theme.reducedMotion
            NumberAnimation { duration: 180; easing.type: Easing.OutQuart }
        }

        // Soft layered modal shadow (works identically in rounded and sharp modes).
        Repeater {
            model: [
                { spread: 2, yOff: 4, alpha: 0.28 },
                { spread: 6, yOff: 9, alpha: 0.16 },
                { spread: 12, yOff: 15, alpha: 0.08 }
            ]
            Rectangle {
                required property var modelData
                x: -modelData.spread
                y: modelData.yOff
                width: frame.width + modelData.spread * 2
                height: frame.height + modelData.spread
                radius: card.radius > 0 ? card.radius + modelData.spread : 0
                color: theme.shadow
                opacity: modelData.alpha
            }
        }

        Rectangle {
            id: card
            anchors.fill: parent
            radius: Ui.radius(16, theme.radius)
            color: theme.raised
            border.width: 1
            border.color: Ui.tint(theme.ink1, 0.09)
            clip: true

            readonly property var current: welcome.mode === "welcome" ? welcome.pages[welcome.page] : null

            RowLayout {
                anchors.fill: parent
                spacing: 0

                // --- Left 320 px warm paper illustration pane ------------
                Rectangle {
                    id: artPane
                    visible: frame.split
                    Layout.preferredWidth: 320
                    Layout.fillHeight: true
                    color: "#F6F1E7"
                    clip: true

                    // Step 0: Clothbound hardcover volume over warm cream leaf with silk ribbon bookmark.
                    Item {
                        anchors.fill: parent
                        visible: welcome.page === 0

                        // Warm cream leaf peeking behind the hardcover.
                        Rectangle {
                            x: 72
                            y: 96
                            width: 170
                            height: 230
                            radius: 3
                            color: "#FDFBF6"
                            rotation: -5
                            Rectangle {
                                z: -1
                                anchors.fill: parent
                                anchors.topMargin: 10
                                anchors.margins: -3
                                radius: 5
                                color: Qt.rgba(60 / 255, 40 / 255, 10 / 255, 0.14)
                            }
                        }

                        // Front clothbound hardcover book with bound spine & silk ribbon.
                        Item {
                            x: 84
                            y: 106
                            width: 170
                            height: 230

                            Rectangle {
                                anchors.fill: parent
                                anchors.topMargin: 10
                                anchors.margins: -3
                                radius: 7
                                color: Qt.rgba(60 / 255, 40 / 255, 10 / 255, 0.18)
                            }

                            // Silk swallowtail bookmark ribbon emerging beneath the text block.
                            Shape {
                                x: 112
                                y: 214
                                width: 22
                                height: 34
                                preferredRendererType: Shape.CurveRenderer
                                ShapePath {
                                    strokeWidth: 0
                                    fillColor: theme.accent
                                    PathSvg { path: "M0 0 H22 V32 L11 23 L0 32 Z" }
                                }
                            }

                            // Woven indigo hardcover board.
                            Rectangle {
                                anchors.fill: parent
                                radius: 6
                                color: "#2C3E63"
                                border.width: 1
                                border.color: "#1E2C47"
                                clip: true

                                // Stitched dark-cloth spine panel.
                                Rectangle {
                                    anchors.top: parent.top
                                    anchors.bottom: parent.bottom
                                    anchors.left: parent.left
                                    width: 24
                                    color: "#1F2D49"
                                    Rectangle {
                                        anchors.right: parent.right
                                        anchors.top: parent.top
                                        anchors.bottom: parent.bottom
                                        width: 1
                                        color: Qt.rgba(1, 1, 1, 0.14)
                                    }
                                }

                                // Debossed cover frame and gilt title rules.
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.leftMargin: 34
                                    anchors.rightMargin: 14
                                    anchors.topMargin: 16
                                    anchors.bottomMargin: 16
                                    radius: 3
                                    color: "transparent"
                                    border.width: 1
                                    border.color: Qt.rgba(232 / 255, 179 / 255, 57 / 255, 0.35)

                                    Column {
                                        anchors.centerIn: parent
                                        width: parent.width - 24
                                        spacing: 10

                                        Rectangle {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            width: parent.width * 0.78
                                            height: 6
                                            radius: 3
                                            color: "#F4EDE0"
                                        }
                                        Rectangle {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            width: parent.width * 0.52
                                            height: 4
                                            radius: 2
                                            color: theme.accent
                                            opacity: 0.9
                                        }
                                        Rectangle {
                                            anchors.horizontalCenter: parent.horizontalCenter
                                            width: parent.width * 0.36
                                            height: 2
                                            radius: 1
                                            color: "#F4EDE0"
                                            opacity: 0.35
                                        }
                                    }
                                }
                            }
                        }
                    }

                    // Step 1: Highlighted Literata excerpt + floating dark audio pill.
                    Item {
                        anchors.fill: parent
                        visible: welcome.page === 1

                        Column {
                            x: 44
                            y: 108
                            width: 232
                            spacing: 10

                            Label {
                                width: parent.width
                                text: qsTr("“Don’t follow you,” said Filby.")
                                color: "#6E665A"
                                font.family: "Literata"
                                font.pixelSize: 15
                                lineHeight: 1.5
                                wrapMode: Text.Wrap
                            }

                            Rectangle {
                                width: parent.width
                                height: activeQuote.implicitHeight + 6
                                radius: 4
                                color: theme.accentRead

                                Label {
                                    id: activeQuote
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: 4
                                    anchors.rightMargin: 4
                                    text: qsTr("“Can a cube that does not last for any time at all, have a real existence?”")
                                    color: "#26221D"
                                    font.family: "Literata"
                                    font.pixelSize: 15
                                    lineHeight: 1.5
                                    wrapMode: Text.Wrap
                                }
                            }

                            Label {
                                width: parent.width
                                text: qsTr("Filby became pensive.")
                                color: "#6E665A"
                                font.family: "Literata"
                                font.pixelSize: 15
                                lineHeight: 1.5
                                wrapMode: Text.Wrap
                            }
                        }

                        // Dark floating mini player pill with equalizer bars.
                        Rectangle {
                            x: 48
                            anchors.bottom: parent.bottom
                            anchors.bottomMargin: 92
                            width: 224
                            height: 44
                            radius: 22
                            color: "#1C1B19"

                            Row {
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: 12
                                spacing: 4

                                Rectangle {
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: 26
                                    height: 26
                                    radius: 13
                                    color: theme.accent
                                    Glyph {
                                        anchors.centerIn: parent
                                        width: 12
                                        height: 12
                                        filled: true
                                        path: Icons.playSolid
                                        color: theme.accentOn
                                    }
                                }

                                Item { width: 8; height: 1 }

                                Repeater {
                                    model: [10, 18, 24, 14, 20, 8, 16, 22, 12]
                                    delegate: Rectangle {
                                        required property int modelData
                                        required property int index
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 3
                                        height: modelData
                                        radius: 1.5
                                        color: index < 6 ? "#CFC9BE" : "#6E6A63"
                                    }
                                }
                            }
                        }
                    }

                    // Step 2: Delicate 96 px sparkle + light-theme provenance pills.
                    Column {
                        anchors.centerIn: parent
                        visible: welcome.page === 2
                        spacing: 18

                        Shape {
                            anchors.horizontalCenter: parent.horizontalCenter
                            width: 96
                            height: 96
                            preferredRendererType: Shape.CurveRenderer

                            ShapePath {
                                strokeColor: "#26221D"
                                strokeWidth: 0.9
                                joinStyle: ShapePath.RoundJoin
                                fillColor: theme.accentRead
                                PathSvg { path: "M48 12 L55.2 32.8 L76 40 L55.2 47.2 L48 68 L40.8 47.2 L20 40 L40.8 32.8 Z" }
                            }
                            ShapePath {
                                strokeColor: "#26221D"
                                strokeWidth: 0.9
                                joinStyle: ShapePath.RoundJoin
                                fillColor: "transparent"
                                PathSvg { path: "M76 64 L78.8 71.2 L86 74 L78.8 76.8 L76 84 L73.2 76.8 L66 74 L73.2 71.2 Z" }
                            }
                        }

                        Row {
                            anchors.horizontalCenter: parent.horizontalCenter
                            spacing: 8

                            Rectangle {
                                height: 24
                                width: localPillLbl.implicitWidth + 20
                                radius: 12
                                color: "#E3EFE3"
                                Label {
                                    id: localPillLbl
                                    anchors.centerIn: parent
                                    text: qsTr("On this computer")
                                    color: "#24502C"
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                }
                            }
                            Rectangle {
                                height: 24
                                width: cloudPillLbl.implicitWidth + 20
                                radius: 12
                                color: "#F4E6CC"
                                Label {
                                    id: cloudPillLbl
                                    anchors.centerIn: parent
                                    text: qsTr("Gemini, your key")
                                    color: "#6A4410"
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                }
                            }
                        }
                    }
                }

                // --- Right copy & footer pane ----------------------------
                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    Flickable {
                        anchors.top: parent.top
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: footer.top
                        anchors.topMargin: frame.split ? 40 : 28
                        anchors.leftMargin: frame.split ? 40 : 28
                        anchors.rightMargin: frame.split ? 40 : 28
                        anchors.bottomMargin: 12
                        contentHeight: copyBody.implicitHeight
                        clip: true
                        boundsBehavior: Flickable.StopAtBounds
                        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                        ColumnLayout {
                            id: copyBody
                            width: parent.width
                            spacing: 14

                            Label {
                                Layout.fillWidth: true
                                text: card.current ? card.current.kicker : qsTr("What's new")
                                color: theme.accentInk
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                font.capitalization: Font.AllUppercase
                                font.letterSpacing: 0.66
                            }

                            Label {
                                Layout.fillWidth: true
                                text: card.current ? card.current.title : qsTr("What's new in Clothbound %1").arg(Qt.application.version)
                                color: theme.ink1
                                font.family: "Literata"
                                font.pixelSize: card.current ? 30 : 24
                                font.weight: Font.Medium
                                font.letterSpacing: -0.3
                                lineHeight: 1.15
                                wrapMode: Text.Wrap
                            }

                            Label {
                                Layout.fillWidth: true
                                visible: !!card.current
                                text: card.current ? card.current.lead : ""
                                color: theme.quote
                                font.pixelSize: 14
                                lineHeight: 1.5
                                wrapMode: Text.Wrap
                            }

                            // Welcome points with tactile 28×22 physical keycaps.
                            ColumnLayout {
                                visible: !!card.current
                                Layout.fillWidth: true
                                Layout.topMargin: 4
                                spacing: 10

                                Repeater {
                                    model: card.current ? card.current.points : []
                                    delegate: RowLayout {
                                        required property var modelData
                                        Layout.fillWidth: true
                                        spacing: 12

                                        KeyHint {
                                            Layout.alignment: Qt.AlignTop
                                            Layout.topMargin: 1
                                            text: modelData.key
                                            keycap: true
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData.text
                                            color: theme.ink2
                                            font.pixelSize: 13
                                            lineHeight: 1.48
                                            wrapMode: Text.Wrap
                                        }
                                    }
                                }
                            }

                            // Post-update "What's new" points (preserved Clothbound feature).
                            ColumnLayout {
                                visible: !card.current
                                Layout.fillWidth: true
                                Layout.topMargin: 4
                                spacing: 10

                                Repeater {
                                    model: !card.current ? welcome.news : []
                                    delegate: RowLayout {
                                        required property string modelData
                                        Layout.fillWidth: true
                                        spacing: 10
                                        Rectangle {
                                            Layout.alignment: Qt.AlignTop
                                            Layout.topMargin: 7
                                            Layout.preferredWidth: 6
                                            Layout.preferredHeight: 6
                                            radius: 3
                                            color: theme.accent
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: modelData
                                            color: theme.ink2
                                            font.pixelSize: 13
                                            lineHeight: 1.45
                                            wrapMode: Text.Wrap
                                        }
                                    }
                                }
                            }
                        }
                    }

                    RowLayout {
                        id: footer
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        anchors.leftMargin: frame.split ? 40 : 28
                        anchors.rightMargin: frame.split ? 40 : 28
                        anchors.bottomMargin: 28
                        spacing: 8

                        // Step dots (18 px active pill, 6 px inactive).
                        Row {
                            visible: welcome.mode === "welcome"
                            spacing: 6
                            Accessible.name: qsTr("Step %1 of %2").arg(welcome.page + 1).arg(welcome.pages.length)
                            Repeater {
                                model: welcome.pages.length
                                delegate: Rectangle {
                                    required property int index
                                    width: index === welcome.page ? 18 : 6
                                    height: 6
                                    radius: 3
                                    color: index === welcome.page ? theme.accent : Ui.tint(theme.ink1, 0.22)
                                    Behavior on width {
                                        enabled: !theme.reducedMotion
                                        NumberAnimation { duration: 150 }
                                    }
                                }
                            }
                        }

                        Item { Layout.fillWidth: true }

                        TextButton {
                            visible: welcome.mode === "welcome" && welcome.page < welcome.pages.length - 1
                            ghost: true
                            text: qsTr("Skip")
                            onClicked: welcome.close()
                        }
                        TextButton {
                            visible: !!card.current && card.current.action !== ""
                            text: card.current ? card.current.action : ""
                            onClicked: {
                                if (welcome.page === 0) {
                                    welcome.addBooks()
                                    welcome.next()
                                } else {
                                    welcome.close()
                                    welcome.showIntelligence()
                                }
                            }
                        }
                        TextButton {
                            primary: true
                            text: welcome.mode === "welcome" && welcome.page < welcome.pages.length - 1
                                  ? qsTr("Next") : qsTr("Start reading")
                            onClicked: welcome.next()
                        }
                    }
                }
            }
        }
    }
}
