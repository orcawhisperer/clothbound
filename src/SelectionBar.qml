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
    onVisibleChanged: {
        if (!visible) {
            defining = false
        } else {
            if (anchorRect && anchorArea)
                place(anchorRect, anchorArea)
            if (!theme.reducedMotion) {
                appearAnim.restart()
            } else {
                opacity = 1
                scale = 1
            }
        }
    }
    onTargetChanged: defining = false

    readonly property var colors: ["yellow", "green", "blue", "pink"]
    readonly property var swatches: Themes.swatches
    readonly property string selectedWords: (target && (target.text || target.quote) ? String(target.text || target.quote) : "").trim()
    readonly property bool defineMatchesSelection: companion.feature === "define"
        && (companion.subject.trim().toLowerCase() === selectedWords.toLowerCase() || selectedWords === "")
    readonly property bool isLocalDefine: companion["whereLocal"] !== undefined
                                        ? Boolean(companion["whereLocal"])
                                        : (ai.routeFor("define") === "local" && ai.localIsLoopback)

    property var anchorRect: null
    property var anchorArea: null

    function parseDefinition(raw) {
        const cleaned = companion.sanitizeMarkdown(raw || "").trim()
        if (!cleaned)
            return { pos: "", body: "", sourceNote: "" }
        let lines = cleaned.split(/\r?\n/).map(l => l.trim()).filter(l => l.length > 0)
        if (!lines.length)
            return { pos: "", body: "", sourceNote: "" }

        // Separate an optional trailing StarDict dictionary line ("— WordNet (r) 3.0").
        let sourceNote = ""
        if (lines.length > 1 && /^[—–-]\s+/.test(lines[lines.length - 1])) {
            sourceNote = lines.pop().replace(/^[—–-]\s+/, "").trim()
        }

        // Drop a redundant first line that merely repeats the headword (common in StarDict/sdcv).
        const head = bar.selectedWords.toLowerCase().replace(/^[^\p{L}\p{N}]+|[^\p{L}\p{N}]+$/gu, "")
        if (lines.length > 1 && head) {
            const firstPlain = lines[0].replace(/[*_`]/g, "").trim().toLowerCase()
            if (firstPlain === head)
                lines.shift()
        }

        const first = lines[0] || ""
        // Extract an optional leading "**word** *adjective* — rest", "*noun* — rest",
        // StarDict "adj. definition", or WordNet "adj 1: definition".
        const posPattern = /^(?:\*\*[^*]+\*\*\s*)?(?:(?:\*|_|[(])\s*(noun|verb|adjective|adverb|pronoun|preposition|conjunction|interjection|idiom|phrase|exclamation|determiner|n\.|v\.|adj\.|adv\.)\s*(?:\*|_|[)])(?:\s*[:—–-]\s*|\s+)|(n\.|v\.|adj\.|adv\.)(?:\s*[:—–-]\s*|\s+)|(noun|verb|adjective|adverb|pronoun|preposition|conjunction|interjection|idiom|phrase|exclamation|determiner|adj|adv)(?:\s*[:—–-]\s*|\s+\d+\s*:\s*))(.*)$/i
        const m = first.match(posPattern)
        if (m) {
            let pos = (m[1] || m[2] || m[3] || "").toLowerCase()
            if (pos === "n.") pos = "noun"
            else if (pos === "v.") pos = "verb"
            else if (pos === "adj." || pos === "adj") pos = "adjective"
            else if (pos === "adv." || pos === "adv") pos = "adverb"
            const rest = (m[4] || "").trim()
            const tail = lines.slice(1).join("\n\n")
            const body = [rest, tail].filter(s => s.length > 0).join("\n\n")
            return { pos: pos, body: body || cleaned, sourceNote: sourceNote }
        }
        if (head && first.toLowerCase().startsWith("**" + head + "**")) {
            const after = first.slice(head.length + 4).replace(/^[\s:—–-]+/, "").trim()
            const restLines = [after].concat(lines.slice(1)).filter(s => s.length > 0).join("\n\n")
            if (restLines)
                return { pos: "", body: restLines, sourceNote: sourceNote }
        }
        return { pos: "", body: lines.join("\n\n"), sourceNote: sourceNote }
    }

    readonly property var parsedDefine: defineMatchesSelection
        ? parseDefinition(companion.answer) : ({ pos: "", body: "", sourceNote: "" })

    readonly property real maxPopoverWidth: anchorArea && anchorArea.width > 0
        ? Math.max(220, anchorArea.width - 20) : 520
    readonly property real desiredWidth: defining ? Math.max(row.implicitWidth + 12, 356) : row.implicitWidth + 12
    readonly property real maxDefineBodyHeight: anchorArea && anchorArea.height > 0
        ? Math.max(72, Math.min(200, anchorArea.height - 140)) : 180

    // Below the selection first (09_design_language: "Selection tools sit under
    // the selection"), flipping above only when there is no room below, and
    // strictly clamped inside viewport bounds even on narrow or resized windows.
    function place(rect, area) {
        anchorRect = rect
        anchorArea = area
        if (!rect || !area)
            return
        const margin = 10
        const w = bar.width
        const h = bar.height
        const maxX = Math.max(margin, area.width - w - margin)
        x = Math.max(margin, Math.min(maxX, rect.x + rect.width / 2 - w / 2))
        const below = rect.y + rect.height + 12
        const above = rect.y - h - 12
        const maxY = Math.max(margin, area.height - h - margin)
        if (below + h <= area.height - margin)
            y = Math.max(margin, below)
        else if (above >= margin)
            y = Math.min(maxY, above)
        else
            y = Math.max(margin, Math.min(maxY, below))
    }

    Connections {
        target: bar.anchorArea
        function onWidthChanged() {
            if (bar.visible && bar.anchorRect && bar.anchorArea)
                bar.place(bar.anchorRect, bar.anchorArea)
        }
        function onHeightChanged() {
            if (bar.visible && bar.anchorRect && bar.anchorArea)
                bar.place(bar.anchorRect, bar.anchorArea)
        }
    }

    onWidthChanged: if (visible && anchorRect && anchorArea) place(anchorRect, anchorArea)
    onHeightChanged: if (visible && anchorRect && anchorArea) place(anchorRect, anchorArea)

    ParallelAnimation {
        id: appearAnim
        NumberAnimation { target: bar; property: "opacity"; from: 0.0; to: 1.0; duration: 120; easing.type: Easing.OutCubic }
        NumberAnimation { target: bar; property: "scale"; from: 0.96; to: 1.0; duration: 130; easing.type: Easing.OutCubic }
    }

    transformOrigin: Item.Top
    width: Math.min(desiredWidth, maxPopoverWidth)
    height: defining ? 40 + definition.implicitHeight + 14 : 40
    radius: Ui.radius(12, theme.radius)
    color: look.popover
    border.width: 1
    border.color: look.dark ? Qt.rgba(1, 1, 1, 0.09) : look.line

    Keys.onEscapePressed: bar.visible = false

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
        implicitWidth: wordLabel.implicitWidth + 14
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        HoverHandler { cursorShape: Qt.PointingHandCursor }
        background: Rectangle {
            radius: Ui.radius(7, theme.radius)
            color: word.on ? Ui.tint(bar.look.fg, 0.10)
                 : word.pressed ? Ui.tint(bar.look.fg, 0.14)
                 : word.hovered ? Ui.tint(bar.look.fg, 0.07) : "transparent"
            Behavior on color { enabled: !theme.reducedMotion; ColorAnimation { duration: 90 } }
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

    Flickable {
        id: rowScroll
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 6
        anchors.rightMargin: 6
        y: 0
        height: 40
        clip: true
        interactive: contentWidth > width
        contentWidth: row.implicitWidth
        contentHeight: height
        boundsBehavior: Flickable.StopAtBounds

        Row {
            id: row
            x: Math.max(0, (rowScroll.width - width) / 2)
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
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    Accessible.name: qsTr("Highlight %1").arg(modelData)
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
                onClicked: {
                    if (bar.defining && bar.defineMatchesSelection && !companion.busy) {
                        bar.defining = false
                    } else {
                        bar.defining = true
                        bar.defineHere()
                    }
                }
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
            spacing: 7

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
                visible: companion.busy && bar.defineMatchesSelection && bar.parsedDefine.body === ""
                spacing: 8
                BusyIndicator { width: 16; height: 16; padding: 0; running: parent.visible }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: companion.status !== "" ? companion.status : qsTr("Looking it up…")
                    color: bar.look.muted
                    font.pixelSize: 12
                }
            }

            Column {
                width: parent.width
                visible: bar.defineMatchesSelection && !companion.busy && companion.problem !== ""
                spacing: 8
                Label {
                    width: parent.width
                    text: companion.problem
                    color: bar.look.muted
                    font.pixelSize: 12
                    wrapMode: Text.Wrap
                    lineHeight: 1.4
                }
                Row {
                    spacing: 12
                    Label {
                        text: qsTr("Try again")
                        color: bar.look.accentInk || bar.look.link
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        TapHandler { onTapped: bar.defineHere() }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                    }
                    Label {
                        text: qsTr("Open Companion")
                        color: bar.look.accentInk || bar.look.link
                        font.pixelSize: 12
                        font.weight: Font.Medium
                        TapHandler { onTapped: bar.moreInCompanion() }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                    }
                }
            }

            Flickable {
                id: defScroll
                width: parent.width
                visible: bar.defineMatchesSelection && bar.parsedDefine.body !== ""
                height: visible ? Math.min(defText.implicitHeight, bar.maxDefineBodyHeight) : 0
                contentWidth: width
                contentHeight: defText.implicitHeight
                clip: true
                interactive: contentHeight > height
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: ScrollBar { policy: defScroll.contentHeight > defScroll.height ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff }

                TextEdit {
                    id: defText
                    width: defScroll.width - (defScroll.contentHeight > defScroll.height ? 8 : 0)
                    readOnly: true
                    selectByMouse: true
                    wrapMode: Text.Wrap
                    textFormat: TextEdit.MarkdownText
                    text: bar.parsedDefine.body
                    color: bar.look.fg
                    selectionColor: Ui.tint(bar.look.link, 0.35)
                    font.pixelSize: 13
                    Keys.onEscapePressed: bar.visible = false
                }
            }

            Row {
                width: parent.width
                topPadding: 4
                visible: bar.defineMatchesSelection && !companion.busy && bar.parsedDefine.body !== ""
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
                    text: bar.parsedDefine.sourceNote !== ""
                          ? (companion.where ? companion.where + " · " + bar.parsedDefine.sourceNote : bar.parsedDefine.sourceNote)
                          : companion.where
                    color: bar.look.muted
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
                Label {
                    id: moreLink
                    anchors.verticalCenter: parent.verticalCenter
                    text: qsTr("Ask Companion")
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
