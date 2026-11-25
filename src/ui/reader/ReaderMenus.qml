import QtQuick
import QtQuick.Controls

import "../components/Ui.js" as Ui

// Context and chrome menus for the reading surface: right-click page menu,
// academic citation copy menu (BibTeX/APA/DOI/arXiv), and overflow reader menu.
Item {
    id: menus

    property var look
    property bool pdf: false
    property var selection: ({})
    property int pageBookmark: 0

    readonly property color menuBg: menus.look ? menus.look.popover : "#FDFAF3"
    readonly property color menuFg: menus.look ? menus.look.fg : "#26221D"
    readonly property color menuMuted: menus.look ? menus.look.muted : "#6E665A"
    readonly property color menuLine: menus.look ? menus.look.line : "#D8D2C6"
    readonly property color menuAccent: menus.look ? menus.look.link : "#3E5F99"
    readonly property color menuAccentFg: Ui.readableOn(menuAccent)
    readonly property color menuShadow: (menus.look && menus.look.dark) ? "#73000000" : "#26000000"

    readonly property alias pageMenu: pageMenuPopup
    readonly property alias citeMenu: citeMenuPopup
    readonly property alias readerMore: readerMorePopup
    readonly property bool anyOpened: pageMenuPopup.opened || citeMenuPopup.opened || readerMorePopup.opened

    signal listenFromPlace(int chapter, real offset)
    signal highlightSelection()
    signal noteSelection()
    signal copySelection()
    signal askCompanion(string feature, var target)
    signal illustrateRequested(bool fromSelection, var place)
    signal toggleSnip()
    signal copyCitation(string style)
    signal toggleBookmark()
    signal showContents()
    signal showMarks()
    signal showSearch()
    signal showGoTo()
    signal toggleFullScreen()
    signal toastMessage(string message)

    function closeAll() {
        if (pageMenuPopup.opened)
            pageMenuPopup.close()
        if (readerMorePopup.opened)
            readerMorePopup.close()
        if (citeMenuPopup.opened)
            citeMenuPopup.close()
        pageMenuPopup.place = ({})
    }

    Menu {
        id: citeMenuPopup
        topPadding: 6
        bottomPadding: 6
        // Menus reparent to Overlay.overlay, so bind the active reading-theme
        // palette explicitly (prevents dark desktop white text on light Paper/Sepia popups).
        palette.window: menus.menuBg
        palette.windowText: menus.menuFg
        palette.text: menus.menuFg
        palette.buttonText: menus.menuFg
        palette.mid: menus.menuMuted
        palette.light: menus.menuBg
        palette.dark: menus.menuLine
        palette.highlight: menus.menuAccent
        palette.highlightedText: menus.menuAccentFg
        palette.link: menus.menuAccent
        palette.shadow: menus.menuShadow
        background: Rectangle {
            implicitWidth: 248
            radius: Math.min(10, theme.radius)
            color: menus.menuBg
            border.color: menus.menuLine
            Rectangle {
                z: -1
                anchors.fill: parent
                anchors.topMargin: 4
                anchors.bottomMargin: -4
                radius: parent.radius
                color: "black"
                opacity: menus.look && menus.look.dark ? 0.35 : 0.12
            }
        }
        MenuItem {
            text: qsTr("Copy BibTeX")
            onTriggered: menus.copyCitation("bibtex")
        }
        MenuItem {
            text: qsTr("Copy APA")
            onTriggered: menus.copyCitation("apa")
        }
        MenuSeparator { visible: reader.detectedDoi !== "" || reader.detectedArxiv !== "" }
        MenuItem {
            visible: reader.detectedDoi !== ""
            height: visible ? implicitHeight : 0
            text: qsTr("Copy DOI")
            onTriggered: {
                reader.copyText(reader.detectedDoi)
                menus.toastMessage(qsTr("Copied DOI"))
            }
        }
        MenuItem {
            visible: reader.detectedArxiv !== ""
            height: visible ? implicitHeight : 0
            text: qsTr("Copy %1").arg(reader.detectedArxiv)
            onTriggered: {
                reader.copyText(reader.detectedArxiv)
                menus.toastMessage(qsTr("Copied arXiv ID"))
            }
        }
        MenuSeparator { visible: reader.detectedDoi !== "" || reader.detectedArxiv !== "" }
        MenuItem {
            visible: reader.detectedDoi !== ""
            height: visible ? implicitHeight : 0
            text: qsTr("Open on doi.org")
            onTriggered: reader.openExternal("https://doi.org/" + reader.detectedDoi)
        }
        MenuItem {
            visible: reader.detectedArxiv !== ""
            height: visible ? implicitHeight : 0
            text: qsTr("Open on arXiv")
            onTriggered: reader.openExternal("https://arxiv.org/abs/" + reader.detectedArxiv.replace(/^arXiv:/i, ""))
        }
    }

    Menu {
        id: readerMorePopup
        width: 260
        palette.window: menus.menuBg
        palette.windowText: menus.menuFg
        palette.text: menus.menuFg
        palette.buttonText: menus.menuFg
        palette.mid: menus.menuMuted
        palette.light: menus.menuBg
        palette.dark: menus.menuLine
        palette.highlight: menus.menuAccent
        palette.highlightedText: menus.menuAccentFg
        palette.link: menus.menuAccent
        palette.shadow: menus.menuShadow
        MenuItem { text: qsTr("Contents") + "\tT"; onTriggered: menus.showContents() }
        MenuItem { text: qsTr("Highlights, notes, and bookmarks") + "\tM"; onTriggered: menus.showMarks() }
        MenuItem { text: (menus.pageBookmark ? qsTr("Remove the bookmark") : qsTr("Bookmark this page")) + "\tB"; onTriggered: menus.toggleBookmark() }
        MenuItem { text: qsTr("Search in the book") + "\t/"; onTriggered: menus.showSearch() }
        MenuItem { text: qsTr("Go to a page") + "\tG"; onTriggered: menus.showGoTo() }
        MenuSeparator {}
        MenuItem { text: qsTr("Full screen") + "\tF"; onTriggered: menus.toggleFullScreen() }
    }

    Menu {
        id: pageMenuPopup
        property var place: ({})
        readonly property bool hasSelection: !!(menus.selection && menus.selection.text)
        topPadding: 6
        bottomPadding: 6
        palette.window: menus.menuBg
        palette.windowText: menus.menuFg
        palette.text: menus.menuFg
        palette.buttonText: menus.menuFg
        palette.mid: menus.menuMuted
        palette.light: menus.menuBg
        palette.dark: menus.menuLine
        palette.highlight: menus.menuAccent
        palette.highlightedText: menus.menuAccentFg
        palette.link: menus.menuAccent
        palette.shadow: menus.menuShadow
        background: Rectangle {
            implicitWidth: 230
            radius: Math.min(10, theme.radius)
            color: menus.menuBg
            border.color: menus.menuLine
            Rectangle {
                z: -1
                anchors.fill: parent
                anchors.topMargin: 4
                anchors.bottomMargin: -4
                radius: parent.radius
                color: "black"
                opacity: menus.look && menus.look.dark ? 0.35 : 0.12
            }
        }

        MenuItem {
            text: narration.active ? qsTr("Listen from here instead") : qsTr("Listen from here")
            onTriggered: menus.listenFromPlace(pageMenuPopup.place.chapter || 0, pageMenuPopup.place.offset || 0)
        }
        MenuSeparator { visible: pageMenuPopup.hasSelection }
        MenuItem {
            visible: pageMenuPopup.hasSelection
            height: visible ? implicitHeight : 0
            text: qsTr("Highlight")
            onTriggered: menus.highlightSelection()
        }
        MenuItem {
            visible: pageMenuPopup.hasSelection
            height: visible ? implicitHeight : 0
            text: qsTr("Add a note")
            onTriggered: menus.noteSelection()
        }
        MenuItem {
            visible: pageMenuPopup.hasSelection
            height: visible ? implicitHeight : 0
            text: qsTr("Copy")
            onTriggered: menus.copySelection()
        }
        MenuSeparator { visible: pageMenuPopup.hasSelection }
        MenuItem {
            visible: pageMenuPopup.hasSelection
            height: visible ? implicitHeight : 0
            text: qsTr("Define")
            onTriggered: menus.askCompanion("define", menus.selection)
        }
        MenuItem {
            visible: pageMenuPopup.hasSelection
            height: visible ? implicitHeight : 0
            text: qsTr("Explain")
            onTriggered: menus.askCompanion("explain", menus.selection)
        }
        MenuItem {
            visible: pageMenuPopup.hasSelection
            height: visible ? implicitHeight : 0
            text: qsTr("Translate")
            onTriggered: menus.askCompanion("translate", menus.selection)
        }
        MenuItem {
            visible: illustrator.available
            height: visible ? implicitHeight : 0
            text: pageMenuPopup.hasSelection ? qsTr("Illustrate these words") : qsTr("Illustrate this moment")
            onTriggered: menus.illustrateRequested(pageMenuPopup.hasSelection, pageMenuPopup.place)
        }
        MenuSeparator {}
        MenuItem {
            visible: menus.pdf
            height: visible ? implicitHeight : 0
            text: qsTr("Snip region…") + "\tX"
            onTriggered: menus.toggleSnip()
        }
        MenuItem {
            text: qsTr("Copy BibTeX")
            onTriggered: menus.copyCitation("bibtex")
        }
        MenuItem {
            text: qsTr("Copy APA")
            onTriggered: menus.copyCitation("apa")
        }
        MenuSeparator {}
        MenuItem {
            text: menus.pageBookmark ? qsTr("Remove the bookmark") : qsTr("Bookmark this page")
            onTriggered: menus.toggleBookmark()
        }
    }
}
