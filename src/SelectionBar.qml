import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui
import "Themes.js" as Themes

// What to do with words on the page (04_companion.html): sits below the
// selection (flipping above only when clipped), offering highlight swatches,
// Note, inline Define, Explain, Translate, Copy, Listen, or Remove.
Rectangle {
    id: bar

    property var look
    // A selection: {chapter, start, end, text}; or a highlight: its mark.
    property var target: ({})
    property bool forMark: false

    signal highlight(string color)
    signal note()
    signal copy()
    signal listen()
    // "define", "explain" or "translate", for the companion.
    signal askCompanion(string feature)
    signal remove()
    // Define answered in place, under the buttons; "More in Companion" opens the drawer.
    signal defineHere()
    signal moreInCompanion()
    property bool defining: false
    onVisibleChanged: if (!visible) defining = false
    onTargetChanged: defining = false

    readonly property var colors: ["yellow", "green", "blue", "pink"]
    readonly property var swatches: Themes.swatches
    readonly property string selectedWords: (target && (target.text || target.quote) ? String(target.text || target.quote) : "").trim()
    readonly property bool isLocalDefine: ai.routeFor("define") === "local" && ai.localIsLoopback

    property var anchorRect: null
    property var anchorArea: null

    function parseDefinition(raw) {
        const cleaned = companion.sanitizeMarkdown(raw || "").trim()
        if (!cleaned)
            return { pos: "", body: "" }
        const lines = cleaned.split(/\r?\n/).map(l => l.trim()).filter(l => l.length > 0)
        if (!lines.length)
            return { pos: "", body: "" }
        const first = lines[0]
        const head = bar.selectedWords.toLowerCase()
        // Extract an optional leading "**word** *adjective* — rest" or "*noun* — rest" header.
        const posPattern = /^(?:\*\*[^*]+\*\*\s*)?(?:[*_(]\s*)?(noun|verb|adjective|adverb|pronoun|preposition|conjunction|interjection|idiom|phrase|exclamation|determiner|n\.|v\.|adj\.|adv\.)(?:\s*[*_)])?(?:\s*[:—–-]\s*(.*))?$/i
        const m = first.match(posPattern)
        if (m) {
            const rest = (m[2] || "").trim()
            const tail = lines.slice(1).join("\n\n")
            const body = [rest, tail].filter(s => s.length > 0).join("\n\n")
            return { pos: m[1].toLowerCase(), body: body || cleaned }
        }
        if (head && first.toLowerCase().startsWith("**" + head + "**")) {
            const after = first.slice(head.length + 4).replace(/^[\s:—–-]+/, "").trim()
            const restLines = [after].concat(lines.slice(1)).filter(s => s.length > 0).join("\n\n")
            if (restLines)
                return { pos: "", body: restLines }
        }
        return { pos: "", body: cleaned }
    }

    readonly property var parsedDefine: companion.feature === "define" ? parseDefinition(companion.answer) : ({ pos: "", body: "" })

    // Below the selection first (09_design_language: "Selection tools sit under
    // the selection"), flipping above only when there is no room below.
    function place(rect, area) {
        anchorRect = rect
        anchorArea = area
        if (!rect || !area)
            return
        const w = bar.width
        const h = bar.height
        x = Math.max(8, Math.min(area.width - w - 8, rect.x + rect.width / 2 - w / 2))
        const below = rect.y + rect.height + 12
        const above = rect.y - h - 12
        if (below + h <= area.height - 8)
            y = below
        else if (above >= 8)
            y = above
        else
            y = Math.max(8, Math.min(area.height - h - 8, below))
    }
    onWidthChanged: if (visible && anchorRect && anchorArea) place(anchorRect, anchorArea)
    onHeightChanged: if (visible && anchorRect && anchorArea) place(anchorRect, anchorArea)

    width: defining ? Math.max(row.implicitWidth + 12, 348) : row.implicitWidth + 12
    height: defining ? 40 + definition.implicitHeight + 14 : 40
    radius: Ui.radius(12, theme.radius)
    color: look.popover
    border.width: 1
    border.color: look.dark ? Qt.rgba(1, 1, 1, 0.08) : look.line

    // Keep clicks on padding or dividers from turning the page underneath.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    // Soft layered elevation shadow (04_companion: 0 20px 50px rgba(0,0,0,0.5)).
    Rectangle {
        z: -2
        anchors.fill: parent
        anchors.leftMargin: -4
        anchors.rightMargin: -4
        anchors.topMargin: 8
        anchors.bottomMargin: -14
        radius: parent.radius > 0 ? parent.radius + 4 : 0
        color: "black"
        opacity: bar.look.dark ? 0.34 : 0.10
    }
    Rectangle {
        z: -1
        anchors.fill: parent
        anchors.topMargin: 3
        anchors.bottomMargin: -5
        radius: parent.radius
        color: "black"
        opacity: bar.look.dark ? 0.28 : 0.08
    }

    // A word button on the popover: Note, Define, Explain, Translate.
    component Word: AbstractButton {
        id: word
        property bool on: false
        anchors.verticalCenter: parent.verticalCenter
        implicitHeight: 28
        implicitWidth: wordLabel.implicitWidth + 16
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        background: Rectangle {
            radius: Ui.radius(7, theme.radius)
            color: word.on ? Ui.tint(bar.look.fg, 0.09)
                 : word.pressed ? Ui.tint(bar.look.fg, 0.14)
                 : word.hovered ? Ui.tint(bar.look.fg, 0.07) : "transparent"
        }
        contentItem: Label {
            id: wordLabel
            text: word.text
            color: bar.look.fg
            font.pixelSize: 13
            font.weight: Font.Medium
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
    }

    Row {
        id: row
        anchors.horizontalCenter: parent.horizontalCenter
        y: (40 - height) / 2
        spacing: 2

        Repeater {
            model: bar.colors
            delegate: AbstractButton {
                id: swatch
                required property string modelData
                readonly property bool current: bar.forMark && bar.target.color === modelData
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: 28
                implicitHeight: 28
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                Accessible.name: bar.forMark ? qsTr("Highlight %1").arg(modelData) : qsTr("Highlight %1").arg(modelData)
                background: Rectangle {
                    radius: Ui.radius(7, theme.radius)
                    color: swatch.hovered ? Ui.tint(bar.look.fg, 0.07) : "transparent"
                }
                contentItem: Item {
                    Rectangle {
                        anchors.centerIn: parent
                        width: swatch.hovered || swatch.current ? 16 : 14
                        height: width
                        radius: width / 2
                        color: bar.swatches[swatch.modelData]
                        border.width: swatch.current ? 2 : 0
                        border.color: bar.look.fg
                        Behavior on width {
                            enabled: !theme.reducedMotion
                            NumberAnimation { duration: 90 }
                        }
                    }
                }
                onClicked: bar.highlight(modelData)
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: bar.forMark ? qsTr("Change the color") : qsTr("Highlight")
            }
        }

        Item {
            anchors.verticalCenter: parent.verticalCenter
            width: 9
            height: 18
            Rectangle {
                anchors.centerIn: parent
                width: 1
                height: 18
                color: bar.look.dark ? Qt.rgba(1, 1, 1, 0.10) : bar.look.line
            }
        }

        Word {
            on: bar.forMark && (bar.target.note || "") !== ""
            text: qsTr("Note")
            onClicked: bar.note()
        }
        Word {
            visible: bar.selectedWords !== "" && bar.selectedWords.split(/\s+/).length <= 3
            on: bar.defining
            text: qsTr("Define")
            onClicked: { bar.defining = true; bar.defineHere() }
        }
        Word { text: qsTr("Explain"); onClicked: bar.askCompanion("explain") }
        Word { text: qsTr("Translate"); onClicked: bar.askCompanion("translate") }

        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: 28
            implicitHeight: 28
            tabFocusable: false
            glyph: Icons.copy
            look: bar.look
            tone: bar.look.inkSoft || bar.look.fg
            tip: qsTr("Copy")
            onClicked: bar.copy()
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: 28
            implicitHeight: 28
            tabFocusable: false
            visible: !bar.forMark
            glyph: Icons.headphones
            look: bar.look
            tone: bar.look.inkSoft || bar.look.fg
            tip: qsTr("Listen from here")
            onClicked: bar.listen()
        }
        IconButton {
            anchors.verticalCenter: parent.verticalCenter
            implicitWidth: 28
            implicitHeight: 28
            tabFocusable: false
            visible: bar.forMark
            glyph: Icons.trash
            look: bar.look
            tone: bar.look.inkSoft || bar.look.fg
            tip: qsTr("Remove the highlight")
            onClicked: bar.remove()
        }
    }

    // The definition, in place (04_companion.html:549-559).
    Column {
        id: definition
        visible: bar.defining
        x: 0
        y: 40
        width: bar.width
        spacing: 0

        Rectangle {
            width: parent.width
            height: 1
            color: bar.look.dark ? Qt.rgba(1, 1, 1, 0.07) : bar.look.line
        }

        Column {
            x: 14
            topPadding: 12
            bottomPadding: 2
            width: parent.width - 28
            spacing: 6

            Row {
                width: parent.width
                visible: bar.selectedWords !== ""
                spacing: 8
                Label {
                    anchors.baseline: posLabel.visible ? posLabel.baseline : undefined
                    text: bar.selectedWords
                    color: bar.look.fg
                    font.family: "Literata"
                    font.pixelSize: 17
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, parent.width - (posLabel.visible ? posLabel.implicitWidth + 8 : 0))
                }
                Label {
                    id: posLabel
                    visible: bar.parsedDefine.pos !== ""
                    text: bar.parsedDefine.pos
                    color: bar.look.muted
                    font.pixelSize: 13
                    font.italic: true
                }
            }

            Row {
                visible: companion.busy && companion.feature === "define"
                spacing: 8
                BusyIndicator { width: 16; height: 16; padding: 0; running: parent.visible }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Looking it up…")
                    color: bar.look.muted
                    font.pixelSize: 12
                }
            }

            Label {
                width: parent.width
                visible: companion.feature === "define" && companion.problem !== ""
                text: companion.problem
                color: bar.look.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            TextEdit {
                width: parent.width
                visible: companion.feature === "define" && companion.answer !== ""
                readOnly: true
                selectByMouse: true
                wrapMode: Text.Wrap
                textFormat: TextEdit.MarkdownText
                text: bar.parsedDefine.body
                color: bar.look.fg
                selectionColor: Ui.tint(bar.look.link, 0.35)
                font.pixelSize: 13
                height: Math.min(implicitHeight, 180)
                clip: true
            }

            Row {
                width: parent.width
                topPadding: 4
                visible: companion.feature === "define" && !companion.busy && companion.answer !== ""
                spacing: 6
                Glyph {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 12
                    height: 12
                    stroke: 2
                    path: bar.isLocalDefine ? Icons.computer : Icons.cloud
                    color: bar.look.muted
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    width: Math.max(40, parent.width - 12 - moreLink.implicitWidth - 18)
                    text: companion.where
                    color: bar.look.muted
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
                Label {
                    id: moreLink
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("More in Companion")
                    color: bar.look.accentInk || bar.look.link
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    TapHandler { onTapped: bar.moreInCompanion() }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                }
            }
        }
    }
}
