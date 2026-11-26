import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../audio"
import "../components"

import "../components/Format.js" as Format
import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui

// Reading-stage chrome overlay: top/bottom theme-matched paper backdrops,
// top navigation & running head, inset bottom progress scrubber & folios,
// listening follow-voice pill, first-book tips strip, jump-back pill,
// GPU-accelerated bookmark dog-ear, and first-listen offer popover.
Item {
    id: root

    property var look
    property bool pdf: false
    property bool chromeShown: false
    property bool modalPanelOpen: false
    property bool scrollLayout: false
    property bool pdfZoomedIn: false
    property bool contentsOpen: false
    property real contentsWidth: 0
    property bool contentsResizing: false
    property bool citeMenuOpened: false
    property bool firstBookTipsActive: false
    property var firstBookTipsModel: []
    property bool jumpPillVisible: false
    property string jumpPillLabel: ""
    property int pageBookmark: 0
    property real footerInset: 24
    property real listenBarHeight: 0
    property real rightToolsWidth: 0
    property real rightToolsMargin: 52
    property real listenButtonCenterX: 0
    property Item stageItem: null

    default property alias rightToolsData: rightToolsHost.data

    readonly property alias chromeItem: chrome
    readonly property alias bottomFooterItem: bottomFooter
    readonly property alias jumpPillItem: jumpPill
    readonly property alias followVoicePillItem: followVoicePill
    readonly property alias keysStripItem: keysStrip
    readonly property alias listenOffer: listenOfferLoader
    readonly property alias pageFold: pageFold
    readonly property bool chromeHovered: chromeHover.hovered
    readonly property bool scrubberHovered: scrubberHover.hovered
    readonly property bool scrubberDragging: progressScrubber.dragging
    readonly property bool keysStripWanted: keysStrip.wanted
    readonly property bool listenOfferOpen: listenOfferLoader.open

    signal closeRequested()
    signal toggleContents()
    signal citeMenuRequested(Item anchor, real x, real y)
    signal goToTarget(var target)
    signal showGoTo()
    signal prevPageRequested()
    signal nextPageRequested()
    signal jumpBack()
    signal dismissJumpPill()
    signal dismissFirstBookTips()
    signal toggleBookmark()
    signal showVoices()
    signal focusPageRequested()

    Settings {
        id: folioSettings
        location: settingsUrl
        category: "typography"
        // 0: time left in chapter, 1: time left in book, 2: % · time left
        property int folioMode: 0
    }

    function folioTimeText() {
        if (reader.atEnd)
            return qsTr("Finished")
        const mode = ((folioSettings.folioMode % 3) + 3) % 3
        const loc = Qt.locale()
        const percent = qsTr("%1%").arg(Math.round(reader.progress * 100).toLocaleString(loc, 'f', 0))
        if (mode === 0) {
            if (!root.pdf && reader.minutesLeftInChapter > 0)
                return qsTr("%1 left in chapter").arg(Format.duration(reader.minutesLeftInChapter))
            if (reader.minutesLeftInBook > 0)
                return qsTr("%1 left").arg(Format.duration(reader.minutesLeftInBook))
            return percent
        }
        if (mode === 1) {
            if (reader.minutesLeftInBook > 0)
                return qsTr("%1 left in book").arg(Format.duration(reader.minutesLeftInBook))
            return percent
        }
        if (reader.minutesLeftInBook > 0)
            return qsTr("%1 · %2 left").arg(percent).arg(Format.duration(reader.minutesLeftInBook))
        return percent
    }

    function folioPagesText() {
        const loc = Qt.locale()
        const start = Math.max(1, reader.bookPageStart).toLocaleString(loc, 'f', 0)
        const end = Math.max(1, reader.bookPageEnd).toLocaleString(loc, 'f', 0)
        const total = Math.max(1, reader.bookPages).toLocaleString(loc, 'f', 0)
        const spreadText = reader.bookPageEnd > reader.bookPageStart
            ? qsTr("Pages %1–%2 of %3").arg(start).arg(end).arg(total)
            : qsTr("Page %1 of %2").arg(start).arg(total)
        if (!root.pdf && reader.pageLabel !== "")
            return qsTr("%1 · p. %2").arg(spreadText).arg(reader.pageLabel)
        return spreadText
    }

    // Soft theme-matched paper backdrop behind top header and bottom progress footer.
    Rectangle {
        id: topBackdrop
        z: 2
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 70
        opacity: root.scrollLayout ? 1 : chrome.chromeProgress
        visible: opacity > 0.01
        gradient: Gradient {
            GradientStop { position: 0.0; color: root.look.bg }
            GradientStop { position: 0.68; color: Ui.tint(root.look.bg, 0.95) }
            GradientStop { position: 1.0; color: Ui.tint(root.look.bg, 0) }
        }
        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            y: 48
            height: 1
            color: root.look.line
            opacity: root.pdfZoomedIn || root.scrollLayout ? chrome.chromeProgress * 0.75 : 0
        }
    }
    Rectangle {
        id: bottomBackdrop
        z: 2
        anchors.bottom: parent.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        height: narration.active ? 84 : 108
        opacity: root.scrollLayout ? 1
               : narration.active ? 0.96 : bottomFooter.footerProgress
        visible: opacity > 0.01
        gradient: Gradient {
            GradientStop { position: 0.0; color: Ui.tint(root.look.bg, 0) }
            GradientStop { position: 0.28; color: root.look.bg }
            GradientStop { position: 1.0; color: root.look.bg }
        }
    }

    // --- Top Reading Chrome (02_reading.html:525-547) --------------------
    Item {
        id: chrome
        z: 3
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: theme.reducedMotion ? 0 : Math.round((1 - chromeProgress) * -6)
        height: 48
        property real chromeProgress: root.chromeShown ? 1 : 0
        Behavior on chromeProgress {
            NumberAnimation {
                duration: theme.reducedMotion ? 80 : (root.chromeShown ? 200 : 160)
                easing.type: root.chromeShown ? Easing.OutCubic : Easing.OutQuad
            }
        }
        opacity: chromeProgress
        visible: chromeProgress > 0.01
        HoverHandler { id: chromeHover }

        Row {
            id: leftTools
            anchors.left: parent.left
            anchors.leftMargin: 14
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4

            // Back to the library, as words: the way out is never a guess.
            AbstractButton {
                id: backButton
                anchors.verticalCenter: parent.verticalCenter
                height: 30
                width: backRow.implicitWidth + 16
                hoverEnabled: true
                focusPolicy: root.chromeShown ? Qt.TabFocus : Qt.NoFocus
                Accessible.name: qsTr("Library")
                onClicked: root.closeRequested()
                background: Rectangle {
                    radius: Ui.radius(7, theme.radius)
                    color: backButton.pressed ? Ui.tint(root.look.fg, 0.12)
                         : backButton.hovered ? Ui.tint(root.look.fg, 0.07) : "transparent"
                    border.width: backButton.visualFocus ? 2 : 0
                    border.color: root.look.link
                }
                contentItem: Item {
                    Row {
                        id: backRow
                        x: 6
                        anchors.verticalCenter: parent.verticalCenter
                        spacing: 4
                        Glyph {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 16
                            height: 16
                            stroke: 2
                            path: Icons.chevronLeft
                            color: root.look.inkSoft || root.look.fg
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: qsTr("Library")
                            color: root.look.inkSoft || root.look.fg
                            font.pixelSize: 13
                            font.weight: Font.Medium
                        }
                    }
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Library (Esc)")
                ToolTip.delay: 600
            }

            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: 32
                implicitHeight: 30
                tabFocusable: root.chromeShown
                look: root.look
                glyph: Icons.contents
                active: root.contentsOpen
                tip: qsTr("Contents (T)")
                onClicked: root.toggleContents()
            }

            // Quiet detected DOI / arXiv pill for academic PDFs: one click copies BibTeX / APA.
            AbstractButton {
                id: citeBadge
                visible: reader.open && (reader.detectedDoi !== "" || reader.detectedArxiv !== "")
                anchors.verticalCenter: parent.verticalCenter
                implicitHeight: 26
                implicitWidth: Math.min(180, citeBadgeRow.implicitWidth + 16)
                hoverEnabled: true
                focusPolicy: root.chromeShown ? Qt.TabFocus : Qt.NoFocus
                Accessible.name: qsTr("Copy citation")
                onClicked: root.citeMenuRequested(citeBadge, 0, citeBadge.height + 6)
                background: Rectangle {
                    radius: Ui.radius(13, theme.radius)
                    color: citeBadge.pressed || root.citeMenuOpened ? Ui.tint(root.look.link, 0.16)
                         : citeBadge.hovered ? Ui.tint(root.look.fg, 0.09) : Ui.tint(root.look.fg, 0.05)
                    border.width: citeBadge.visualFocus ? 2 : 1
                    border.color: citeBadge.visualFocus ? root.look.link
                                : root.citeMenuOpened ? Ui.tint(root.look.link, 0.45) : root.look.line
                }
                contentItem: Item {
                    Row {
                        id: citeBadgeRow
                        anchors.centerIn: parent
                        spacing: 5
                        Glyph {
                            anchors.verticalCenter: parent.verticalCenter
                            width: 12
                            height: 12
                            stroke: 1.9
                            path: Icons.copy
                            color: root.citeMenuOpened ? root.look.link : root.look.muted
                        }
                        Label {
                            anchors.verticalCenter: parent.verticalCenter
                            text: reader.detectedArxiv !== "" ? reader.detectedArxiv : ("DOI " + reader.detectedDoi)
                            textFormat: Text.PlainText
                            color: root.look.inkSoft || root.look.fg
                            font.pixelSize: 12
                            font.weight: Font.Medium
                            font.features: ({ "tnum": 1 })
                            elide: Text.ElideRight
                            width: Math.min(implicitWidth, 146)
                        }
                    }
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                ToolTip.visible: hovered && !root.citeMenuOpened
                ToolTip.text: qsTr("Copy citation (BibTeX / APA)")
                ToolTip.delay: 500
            }
        }

        // The book, and where in it: "The Time Machine — Chapter I".
        Item {
            id: heading
            anchors.centerIn: parent
            readonly property real sideReserve: Math.max(leftTools.width + 24, root.rightToolsWidth + root.rightToolsMargin + 14)
            readonly property real room: Math.max(0, parent.width - 2 * sideReserve)
            readonly property bool showChapter: reader.chapterTitle !== "" && reader.chapterTitle !== reader.title
            TextMetrics { id: titleMetrics; text: reader.title; font: titleLabel.font }
            TextMetrics { id: chapterMetrics; text: " — " + reader.chapterTitle; font: chapterLabel.font }
            readonly property real chapterRoom: showChapter ? Math.min(chapterMetrics.advanceWidth + 2, room * 0.45) : 0
            width: titleLabel.width + chapterLabel.width
            height: parent.height
            Label {
                id: titleLabel
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(titleMetrics.advanceWidth + 2, heading.room - heading.chapterRoom)
                text: reader.title
                color: root.look.inkSoft || root.look.fg
                font.pixelSize: 12
                font.weight: Font.Medium
                font.letterSpacing: 0.12
                elide: Text.ElideRight
            }
            Label {
                id: chapterLabel
                anchors.left: titleLabel.right
                anchors.verticalCenter: parent.verticalCenter
                visible: heading.showChapter
                width: visible ? Math.min(chapterMetrics.advanceWidth + 2, heading.room - titleLabel.width) : 0
                text: " — " + reader.chapterTitle
                color: root.look.inkSoft || root.look.fg
                opacity: 0.55
                font.pixelSize: 12
                font.weight: Font.Medium
                font.letterSpacing: 0.12
                elide: Text.ElideRight
            }
        }

        Item {
            id: rightToolsHost
            anchors.fill: parent
        }
    }

    // Top-center voice status / return pill while listening (03_listening.html:588-591).
    AbstractButton {
        id: followVoicePill
        z: 3
        anchors.top: parent.top
        anchors.topMargin: 24 - (theme.reducedMotion ? 0 : Math.round((1 - opacity) * 6))
        anchors.horizontalCenter: parent.horizontalCenter
        readonly property bool shown: narration.active && !root.chromeShown
        opacity: shown ? 1 : 0
        visible: opacity > 0.01
        Behavior on opacity {
            NumberAnimation { duration: theme.reducedMotion ? 80 : 180; easing.type: Easing.OutCubic }
        }
        implicitHeight: 30
        implicitWidth: followPillRow.implicitWidth + 22
        hoverEnabled: true
        focusPolicy: enabled ? Qt.TabFocus : Qt.NoFocus
        enabled: shown && !narration.following
        Accessible.name: narration.following ? qsTr("Pages follow the voice") : qsTr("Back to the voice")
        onClicked: if (!narration.following) narration.following = true
        background: Rectangle {
            radius: Ui.radius(15, theme.radius)
            color: !narration.following
                 ? (followVoicePill.hovered ? Ui.tint(root.look.link, 0.22) : Ui.tint(root.look.link, 0.14))
                 : Ui.tint(root.look.fg, 0.06)
            border.width: followVoicePill.visualFocus ? 2 : 1
            border.color: followVoicePill.visualFocus ? root.look.link
                        : !narration.following ? Ui.tint(root.look.link, 0.35) : Ui.tint(root.look.fg, 0.10)
        }
        contentItem: Item {
            Row {
                id: followPillRow
                anchors.centerIn: parent
                spacing: 6
                Glyph {
                    anchors.verticalCenter: parent.verticalCenter
                    width: 13
                    height: 13
                    stroke: 2
                    path: Icons.waveform
                    color: !narration.following ? root.look.link : (root.look.inkSoft || root.look.fg)
                }
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: narration.following ? qsTr("Pages follow the voice") : qsTr("Back to the voice")
                    color: !narration.following ? root.look.link : (root.look.inkSoft || root.look.fg)
                    font.pixelSize: 12
                    font.weight: Font.Medium
                }
            }
        }
        HoverHandler { cursorShape: !narration.following ? Qt.PointingHandCursor : Qt.ArrowCursor }
    }

    // --- Unified Inset Bottom Footer (02_reading.html:576-595) -----------
    Item {
        id: bottomFooter
        z: 3
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.leftMargin: root.footerInset
        anchors.rightMargin: root.footerInset
        anchors.bottomMargin: 24 - (theme.reducedMotion ? 0 : Math.round((1 - footerProgress) * 8))
        height: footerCol.implicitHeight
        readonly property bool shown: root.chromeShown && !narration.active && !keysStrip.wanted
        property real footerProgress: shown ? 1 : 0
        Behavior on footerProgress {
            NumberAnimation {
                duration: theme.reducedMotion ? 80 : (bottomFooter.shown ? 200 : 160)
                easing.type: bottomFooter.shown ? Easing.OutCubic : Easing.OutQuad
            }
        }
        opacity: footerProgress
        visible: footerProgress > 0.01
        HoverHandler { id: scrubberHover }

        Column {
            id: footerCol
            width: parent.width
            spacing: 8

            ProgressScrubber {
                id: progressScrubber
                width: parent.width
                look: root.look
                onGo: (target) => root.goToTarget(target)
            }

            Item {
                width: parent.width
                height: 24

                // Left: time left (tappable to cycle chapter / book / % · time left; never bare %).
                AbstractButton {
                    id: folioLeftButton
                    anchors.left: parent.left
                    anchors.verticalCenter: parent.verticalCenter
                    enabled: bottomFooter.shown
                    hoverEnabled: true
                    focusPolicy: bottomFooter.shown ? Qt.TabFocus : Qt.NoFocus
                    Accessible.name: folioLeftLabel.text
                    implicitWidth: folioLeftLabel.implicitWidth + 12
                    implicitHeight: 24
                    onClicked: folioSettings.folioMode = (folioSettings.folioMode + 1) % 3
                    background: Rectangle {
                        radius: Ui.radius(6, theme.radius)
                        color: folioLeftButton.hovered ? Ui.tint(root.look.fg, 0.06) : "transparent"
                        border.width: folioLeftButton.visualFocus ? 2 : 0
                        border.color: root.look.link
                    }
                    contentItem: Label {
                        id: folioLeftLabel
                        horizontalAlignment: Text.AlignLeft
                        verticalAlignment: Text.AlignVCenter
                        leftPadding: 4
                        rightPadding: 4
                        text: {
                            void(folioSettings.folioMode)
                            void(reader.progress); void(reader.minutesLeftInChapter); void(reader.minutesLeftInBook)
                            void(reader.atEnd); void(translator.language)
                            return root.folioTimeText()
                        }
                        color: root.look.muted
                        font.pixelSize: 12
                        font.features: ({ "tnum": 1 })
                    }
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    ToolTip.visible: hovered
                    ToolTip.delay: 700
                    ToolTip.text: qsTr("Click to switch time remaining")
                }

                // Right: explicit Previous / Next page buttons flanking the page count
                // ("‹  Pages 3–4 of 148  ›"), so mouse page turns are always intentional.
                Row {
                    id: folioRightRow
                    anchors.right: parent.right
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 2

                    IconButton {
                        id: folioLeftChevron
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !root.scrollLayout
                        implicitWidth: 26
                        implicitHeight: 24
                        tabFocusable: bottomFooter.shown
                        enabled: bottomFooter.shown && (reader.rtl ? !reader.atEnd : !reader.atStart)
                        look: root.look
                        tone: root.look.muted
                        glyph: Icons.chevronLeft
                        tip: reader.rtl ? qsTr("Next page") : qsTr("Previous page")
                        onClicked: reader.rtl ? root.nextPageRequested() : root.prevPageRequested()
                    }

                    AbstractButton {
                        id: folioRightButton
                        anchors.verticalCenter: parent.verticalCenter
                        enabled: bottomFooter.shown
                        hoverEnabled: true
                        focusPolicy: bottomFooter.shown ? Qt.TabFocus : Qt.NoFocus
                        Accessible.name: folioRightLabel.text
                        implicitWidth: folioRightLabel.implicitWidth + 12
                        implicitHeight: 24
                        onClicked: root.showGoTo()
                        background: Rectangle {
                            radius: Ui.radius(6, theme.radius)
                            color: folioRightButton.hovered ? Ui.tint(root.look.fg, 0.06) : "transparent"
                            border.width: folioRightButton.visualFocus ? 2 : 0
                            border.color: root.look.link
                        }
                        contentItem: Label {
                            id: folioRightLabel
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                            leftPadding: 4
                            rightPadding: 4
                            text: {
                                void(reader.bookPageStart); void(reader.bookPageEnd); void(reader.bookPages)
                                void(reader.pageLabel); void(translator.language)
                                return root.folioPagesText()
                            }
                            color: root.look.muted
                            font.pixelSize: 12
                            font.features: ({ "tnum": 1 })
                        }
                        HoverHandler { cursorShape: Qt.PointingHandCursor }
                        ToolTip.visible: hovered
                        ToolTip.delay: 700
                        ToolTip.text: qsTr("Go to a page (G)")
                    }

                    IconButton {
                        id: folioRightChevron
                        anchors.verticalCenter: parent.verticalCenter
                        visible: !root.scrollLayout
                        implicitWidth: 26
                        implicitHeight: 24
                        tabFocusable: bottomFooter.shown
                        enabled: bottomFooter.shown && (reader.rtl ? !reader.atStart : !reader.atEnd)
                        look: root.look
                        tone: root.look.muted
                        glyph: Icons.chevronRight
                        tip: reader.rtl ? qsTr("Previous page") : qsTr("Next page")
                        onClicked: reader.rtl ? root.prevPageRequested() : root.nextPageRequested()
                    }
                }
            }
        }
    }

    // J3 First-book tips: dark 760 px floating bar shown once, then behind ? (12_j3_first_book:489-500).
    Rectangle {
        id: keysStrip
        z: 5
        readonly property bool wanted: root.firstBookTipsActive
        anchors.horizontalCenter: parent.horizontalCenter
        y: wanted ? parent.height - height - 40 : parent.height + 16
        Behavior on y {
            enabled: !theme.reducedMotion
            NumberAnimation { duration: 280; easing.type: Easing.OutQuart }
        }
        opacity: wanted ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: theme.reducedMotion ? 0 : 180 } }
        visible: opacity > 0.01 && y < parent.height
        width: Math.min(parent.width - 48, 760)
        height: 62
        radius: Ui.radius(14, theme.radius)
        color: Qt.rgba(28 / 255, 27 / 255, 25 / 255, 0.95)
        border.width: root.look.dark ? 1 : 0
        border.color: Qt.rgba(1, 1, 1, 0.08)

        Rectangle {
            z: -2
            anchors.fill: parent
            anchors.leftMargin: -4
            anchors.rightMargin: -4
            anchors.topMargin: 10
            anchors.bottomMargin: -16
            radius: parent.radius > 0 ? parent.radius + 4 : 0
            color: root.look.dark ? "#000000" : "#281E0A"
            opacity: 0.24
        }
        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.topMargin: 4
            anchors.bottomMargin: -6
            radius: parent.radius
            color: "#000000"
            opacity: 0.20
        }

        RowLayout {
            anchors.fill: parent
            anchors.leftMargin: 18
            anchors.rightMargin: 14
            spacing: 16
            Repeater {
                model: root.firstBookTipsModel
                delegate: Row {
                    required property var modelData
                    Layout.alignment: Qt.AlignVCenter
                    spacing: 7
                    KeyHint {
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.key
                        keycap: true
                        onDark: true
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: modelData.what
                        color: "#CFC9BE"
                        font.pixelSize: 13
                    }
                }
            }
            Item { Layout.fillWidth: true }
            AbstractButton {
                id: gotItBtn
                Layout.alignment: Qt.AlignVCenter
                implicitHeight: 30
                implicitWidth: gotItLabel.implicitWidth + 28
                hoverEnabled: true
                focusPolicy: keysStrip.wanted ? Qt.TabFocus : Qt.NoFocus
                Accessible.name: qsTr("Got it")
                onClicked: root.dismissFirstBookTips()
                background: Rectangle {
                    radius: Ui.radius(7, theme.radius)
                    color: gotItBtn.pressed ? Qt.rgba(1, 1, 1, 0.20)
                         : gotItBtn.hovered ? Qt.rgba(1, 1, 1, 0.16) : Qt.rgba(1, 1, 1, 0.10)
                    border.width: gotItBtn.visualFocus ? 2 : 0
                    border.color: theme.accent
                }
                contentItem: Label {
                    id: gotItLabel
                    text: qsTr("Got it")
                    color: "#F2EFE9"
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }

    // Dismissible jump-back pill shown after TOC, link, scrubber, search, or Companion jumps.
    Rectangle {
        id: jumpPill
        z: 5
        anchors.left: parent.left
        anchors.leftMargin: 24 + (root.contentsOpen ? root.contentsWidth : 0)
        Behavior on anchors.leftMargin {
            enabled: !theme.reducedMotion && !root.contentsResizing
            NumberAnimation { duration: 220; easing.type: Easing.OutCubic }
        }
        readonly property bool shown: root.jumpPillVisible && reader.canGoBack && root.jumpPillLabel !== ""
        y: shown ? parent.height - height - (narration.active ? (root.listenBarHeight + 28) : bottomFooter.shown ? 86 : 36) : parent.height + 16
        Behavior on y {
            enabled: !theme.reducedMotion
            NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
        }
        opacity: shown ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: theme.reducedMotion ? 0 : 150 } }
        visible: opacity > 0.01
        width: jumpPillRow.implicitWidth + 16
        height: 36
        radius: Ui.radius(18, theme.radius)
        color: root.look.popover
        border.width: 1
        border.color: root.look.line

        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.topMargin: 3
            anchors.bottomMargin: -3
            radius: parent.radius
            color: "black"
            opacity: root.look.dark ? 0.35 : 0.12
        }

        Row {
            id: jumpPillRow
            anchors.centerIn: parent
            spacing: 4

            AbstractButton {
                id: jumpPillAction
                anchors.verticalCenter: parent.verticalCenter
                implicitHeight: 30
                implicitWidth: jumpActionRow.implicitWidth + 12
                hoverEnabled: true
                focusPolicy: jumpPill.shown ? Qt.TabFocus : Qt.NoFocus
                Accessible.name: qsTr("Return to %1").arg(root.jumpPillLabel)
                onClicked: root.jumpBack()
                background: Rectangle {
                    radius: Ui.radius(15, theme.radius)
                    color: jumpPillAction.hovered ? Ui.tint(root.look.link, 0.14) : "transparent"
                    border.width: jumpPillAction.visualFocus ? 2 : 0
                    border.color: root.look.link
                }
                contentItem: Row {
                    id: jumpActionRow
                    spacing: 6
                    leftPadding: 6
                    rightPadding: 6
                    Glyph {
                        anchors.verticalCenter: parent.verticalCenter
                        width: 14
                        height: 14
                        path: Icons.back
                        color: root.look.link
                    }
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        text: qsTr("Return to %1").arg(root.jumpPillLabel)
                        color: root.look.fg
                        font.pixelSize: 13
                        elide: Text.ElideRight
                        width: Math.min(implicitWidth, 240)
                    }
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("Return (Alt+←)")
            }

            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: 26
                implicitHeight: 26
                tabFocusable: jumpPill.shown
                glyph: Icons.close
                look: root.look
                tone: root.look.muted
                tip: qsTr("Dismiss")
                onClicked: root.dismissJumpPill()
            }
        }
    }

    // GPU-accelerated 44×44 tactile paper bookmark dog-ear in the top-right corner.
    ReaderBookmarkFold {
        id: pageFold
        z: 4
        anchors.top: parent.top
        anchors.right: parent.right
        look: root.look
        bookmarked: root.pageBookmark > 0
        visible: !root.modalPanelOpen || root.pageBookmark > 0
        onToggled: root.toggleBookmark()
    }

    // First-listen popover anchored beneath the top-right Listen button (13_j4_first_listen).
    Loader {
        id: listenOfferLoader
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
        sourceComponent: ListenOffer {
            id: listenOfferInner
            parent: root.stageItem || root
            z: 7
            look: root.look
            anchors.top: parent.top
            anchors.topMargin: chrome.height + 6
            anchors.right: parent.right
            anchors.rightMargin: 22
            onMoreVoices: root.showVoices()
            onClosed: root.focusPageRequested()
            Binding {
                target: listenOfferInner
                property: "caretRightOffset"
                value: Math.max(24, Math.min(Math.max(24, listenOfferInner.width - 24),
                                (root.width - 22) - root.listenButtonCenterX))
                when: listenOfferInner.hasOwnProperty("caretRightOffset")
            }
        }
    }
}
