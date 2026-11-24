import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui

// Calm modal sheet for online-to-offline URL ingestion (Ctrl+Shift+U):
// pastes or accepts web articles (saved offline as EPUB), arXiv papers
// (PDF + Atom metadata), and direct EPUB or PDF links.
FocusScope {
    id: sheet

    property bool open: false
    property int initialCollectionId: 0
    property bool autoCloseOnSuccess: false

    // Internal UI state
    property int selectedCollectionId: 0
    property string clipboardHintUrl: ""
    property string errorMessage: ""
    property int importedBookId: 0
    property string importedTitle: ""
    property string importedKind: ""

    readonly property bool hasImporter: typeof webImporter !== "undefined" && webImporter !== null
    readonly property bool busy: hasImporter && webImporter.busy
    readonly property string statusText: hasImporter ? webImporter.statusText : ""
    readonly property real importProgress: hasImporter ? webImporter.progress : 0
    readonly property bool succeeded: importedBookId > 0 && !busy
    readonly property string trimmedUrl: urlField.text.trim()
    readonly property string detectedKind: detectKind(trimmedUrl)
    readonly property string detectedArxivId: detectedKind === "arxiv" ? extractArxivId(trimmedUrl) : ""

    // Local 24x24 line icons matching Clothbound's Icons.js stroke grid.
    readonly property string globeIcon: "M12 3 A9 9 0 1 1 12 21 A9 9 0 1 1 12 3 Z M3.6 9 H20.4 M3.6 15 H20.4 M12 3 C14.8 6 14.8 18 12 21 C9.2 18 9.2 6 12 3 Z"
    readonly property string linkIcon: "M10 13 A4 4 0 0 0 15.6 13 L18 10.6 A4 4 0 0 0 12.4 5 L11 6.4 M14 11 A4 4 0 0 0 8.4 11 L6 13.4 A4 4 0 0 0 11.6 19 L13 17.6"
    readonly property string articleIcon: "M5 5.5 H19 Q20 5.5 20 6.5 V17.5 Q20 18.5 19 18.5 H5 Q4 18.5 4 17.5 V6.5 Q4 5.5 5 5.5 Z M8 9.5 H16 M8 12.5 H16 M8 15.5 H12.5"
    readonly property string paperIcon: "M6 3.5 H14 L18.5 8 V20.5 H6 Z M14 3.5 V8 H18.5 M9 12.5 H15 M9 15.5 H13.5"

    readonly property var collectionOptions: {
        const list = [{ id: 0, name: qsTr("Library only") }]
        if (typeof library !== "undefined" && library && library.collections) {
            const cols = library.collections
            for (let i = 0; i < cols.length; ++i)
                list.push({ id: cols[i].id, name: cols[i].name })
        }
        return list
    }

    signal imported(int bookId, string title, string kind)
    signal openBookRequested(int bookId)
    signal closed()

    function detectKind(raw) {
        if (!raw || raw === "")
            return ""
        if (hasImporter && typeof webImporter.detectUrlKind === "function")
            return webImporter.detectUrlKind(raw)
        const s = raw.trim()
        if (/^arxiv:\s*(?:\d{4}\.\d{4,5}|[a-z\-]+(?:\.[A-Z]{2})?\/\d{7})(?:v\d+)?$/i.test(s))
            return "arxiv"
        if (/^https?:\/\/(?:[a-z0-9\-]+\.)?arxiv\.org\/(?:abs|pdf|html|e-print)\//i.test(s))
            return "arxiv"
        if (!/^https?:\/\/[^\s/\\]+/i.test(s))
            return ""
        const cleanPath = s.split(/[?#]/)[0].toLowerCase()
        if (cleanPath.endsWith(".epub") || cleanPath.endsWith(".kepub"))
            return "epub"
        if (cleanPath.endsWith(".pdf"))
            return "pdf"
        return "article"
    }

    function extractArxivId(raw) {
        if (!raw || raw === "")
            return ""
        if (hasImporter && typeof webImporter.parseArxivId === "function")
            return webImporter.parseArxivId(raw)
        return ""
    }

    function detectionBadgeText(kind) {
        if (kind === "arxiv")
            return detectedArxivId !== ""
                   ? qsTr("arXiv paper (%1) · PDF + metadata").arg(detectedArxivId)
                   : qsTr("arXiv paper · PDF + metadata")
        if (kind === "article")
            return qsTr("Web article · saves offline as EPUB")
        if (kind === "epub")
            return qsTr("EPUB book")
        if (kind === "pdf")
            return qsTr("PDF document")
        return ""
    }

    function detectionBadgeGlyph(kind) {
        if (kind === "arxiv")
            return sheet.paperIcon
        if (kind === "epub")
            return Icons.book
        if (kind === "pdf")
            return Icons.page
        return sheet.articleIcon
    }

    function refreshClipboardHint() {
        if (hasImporter && typeof webImporter.clipboardUrl === "function")
            clipboardHintUrl = webImporter.clipboardUrl()
        else
            clipboardHintUrl = ""
    }

    function syncCollectionIndex(targetId) {
        let idx = 0
        for (let i = 0; i < collectionOptions.length; ++i) {
            if (collectionOptions[i].id === targetId) {
                idx = i
                break
            }
        }
        selectedCollectionId = collectionOptions[idx].id
        collectionCombo.currentIndex = idx
    }

    function show(initialUrl, collectionId) {
        errorMessage = ""
        importedBookId = 0
        importedTitle = ""
        importedKind = ""
        urlField.text = initialUrl ? String(initialUrl) : ""

        let colId = 0
        if (typeof collectionId === "number" && collectionId >= 0) {
            colId = collectionId
        } else if (initialCollectionId > 0) {
            colId = initialCollectionId
        } else if (typeof library !== "undefined" && library && library.works && library.works.collectionId > 0) {
            colId = library.works.collectionId
        }
        syncCollectionIndex(colId)
        refreshClipboardHint()

        open = true
        urlField.forceActiveFocus()
        if (urlField.text !== "")
            urlField.selectAll()
    }

    function resetForAnother() {
        errorMessage = ""
        importedBookId = 0
        importedTitle = ""
        importedKind = ""
        urlField.text = ""
        refreshClipboardHint()
        urlField.forceActiveFocus()
    }

    function close() {
        if (!open)
            return
        if (busy && hasImporter)
            webImporter.cancel()
        open = false
        closed()
    }

    function submit() {
        if (busy || detectedKind === "" || !hasImporter)
            return
        errorMessage = ""
        importedBookId = 0
        importedTitle = ""
        importedKind = ""
        webImporter.importUrl(trimmedUrl, selectedCollectionId)
    }

    anchors.fill: parent
    z: 17

    property real reveal: open ? 1 : 0
    Behavior on reveal {
        NumberAnimation {
            duration: theme.reducedMotion ? 110 : (sheet.open ? 230 : 165)
            easing.type: sheet.open ? Easing.OutCubic : Easing.InCubic
        }
    }
    visible: reveal > 0.005
    opacity: reveal

    Keys.onShortcutOverride: (event) => {
        if (sheet.open && sheet.busy && event.key === Qt.Key_Escape)
            event.accepted = true
    }
    Keys.onEscapePressed: {
        if (busy && hasImporter)
            webImporter.cancel()
        else
            close()
    }

    Connections {
        target: sheet.hasImporter ? webImporter : null
        function onFinished(bookId, title, kind, sourceUrl) {
            if (!sheet.open)
                return
            sheet.errorMessage = ""
            sheet.importedBookId = bookId
            sheet.importedTitle = title
            sheet.importedKind = kind
            sheet.imported(bookId, title, kind)
            if (sheet.autoCloseOnSuccess) {
                sheet.open = false
                sheet.closed()
            } else {
                openNowBtn.forceActiveFocus()
            }
        }
        function onFailed(error) {
            if (!sheet.open)
                return
            sheet.errorMessage = error || qsTr("Could not import this link.")
            urlField.forceActiveFocus()
        }
        function onCancelled() {
            if (!sheet.open)
                return
            sheet.errorMessage = ""
            urlField.forceActiveFocus()
        }
    }

    // Dimmed backdrop; stays open on accidental outside click while actively downloading.
    Rectangle {
        anchors.fill: parent
        color: theme.scrim
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onClicked: if (!sheet.busy) sheet.close()
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        anchors.verticalCenterOffset: theme.reducedMotion ? 0 : Math.round((1 - sheet.reveal) * 10)
        width: Math.min(540, sheet.width - 48)
        height: Math.min(sheet.height - 64, contentCol.implicitHeight + 48)
        radius: Ui.radius(14, theme.radius)
        color: theme.raised
        border.width: 1
        border.color: Ui.tint(theme.ink1, theme.light ? 0.12 : 0.09)
        scale: theme.reducedMotion ? 1.0 : (0.965 + 0.035 * sheet.reveal)

        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            onWheel: (wheel) => wheel.accepted = true
        }

        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.topMargin: 14
            anchors.margins: -6
            radius: parent.radius > 0 ? parent.radius + 6 : 0
            color: sheet.palette.shadow
            opacity: 0.5
        }

        ColumnLayout {
            id: contentCol
            anchors.fill: parent
            anchors.margins: 24
            spacing: 14

            // Header
            RowLayout {
                Layout.fillWidth: true
                spacing: 10

                Glyph {
                    Layout.preferredWidth: 20
                    Layout.preferredHeight: 20
                    path: sheet.globeIcon
                    color: theme.accentInk
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Add from URL")
                    color: theme.ink1
                    font.family: "Literata"
                    font.pixelSize: 20
                    font.weight: Font.Medium
                }
                KeyHint {
                    text: "Ctrl Shift U"
                }
                IconButton {
                    glyph: Icons.close
                    tip: sheet.busy ? qsTr("Cancel and close (Esc)") : qsTr("Close (Esc)")
                    onClicked: sheet.close()
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Save a web article offline as an EPUB, fetch an arXiv paper with metadata, or download an EPUB or PDF.")
                color: theme.ink3
                font.pixelSize: 13
                lineHeight: 1.35
                wrapMode: Text.Wrap
            }

            // Subtle non-hijacking clipboard suggestion pill
            AbstractButton {
                id: clipboardPill
                visible: sheet.clipboardHintUrl !== "" && sheet.trimmedUrl === "" && !sheet.busy && !sheet.succeeded
                Layout.fillWidth: true
                implicitHeight: 34
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.name: qsTr("Paste link from clipboard: %1").arg(sheet.clipboardHintUrl)
                onClicked: {
                    urlField.text = sheet.clipboardHintUrl
                    sheet.clipboardHintUrl = ""
                    urlField.forceActiveFocus()
                    urlField.cursorPosition = urlField.text.length
                }

                background: Rectangle {
                    radius: Ui.radius(8, theme.radius)
                    color: clipboardPill.pressed ? Ui.tint(theme.accent, 0.18)
                         : clipboardPill.hovered ? Ui.tint(theme.accent, 0.12) : theme.accentSoft
                    border.width: clipboardPill.visualFocus ? 2 : 1
                    border.color: clipboardPill.visualFocus ? theme.accent : Ui.tint(theme.accent, 0.28)
                    Behavior on color { enabled: !theme.reducedMotion; ColorAnimation { duration: 100 } }
                }

                contentItem: RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 12
                    spacing: 8

                    Glyph {
                        Layout.preferredWidth: 14
                        Layout.preferredHeight: 14
                        path: sheet.linkIcon
                        color: theme.accentInk
                    }
                    Label {
                        text: qsTr("Paste link from clipboard")
                        color: theme.accentInk
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }
                    Label {
                        Layout.fillWidth: true
                        text: sheet.clipboardHintUrl
                        textFormat: Text.PlainText
                        color: theme.ink3
                        font.pixelSize: 12
                        elide: Text.ElideMiddle
                    }
                }
                HoverHandler { cursorShape: Qt.PointingHandCursor }
            }

            // URL input field
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 8

                TextField {
                    id: urlField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 38
                    enabled: !sheet.busy
                    placeholderText: qsTr("Paste an article, arXiv paper, EPUB, or PDF link…")
                    rightPadding: clearBtn.visible ? 36 : 10
                    Accessible.name: qsTr("URL to import")
                    onTextChanged: {
                        if (sheet.errorMessage !== "")
                            sheet.errorMessage = ""
                        if (sheet.importedBookId > 0) {
                            sheet.importedBookId = 0
                            sheet.importedTitle = ""
                            sheet.importedKind = ""
                        }
                    }
                    Keys.onReturnPressed: sheet.submit()
                    Keys.onEnterPressed: sheet.submit()
                    Keys.onEscapePressed: sheet.close()

                    IconButton {
                        id: clearBtn
                        visible: urlField.text !== "" && !sheet.busy
                        anchors.right: parent.right
                        anchors.rightMargin: 4
                        anchors.verticalCenter: parent.verticalCenter
                        width: 26
                        height: 26
                        glyph: Icons.close
                        tone: theme.ink3
                        tip: qsTr("Clear")
                        onClicked: {
                            urlField.text = ""
                            sheet.refreshClipboardHint()
                            urlField.forceActiveFocus()
                        }
                    }
                }

                // Live kind detection badge or gentle validation hint
                RowLayout {
                    Layout.fillWidth: true
                    visible: sheet.trimmedUrl !== "" && !sheet.succeeded
                    spacing: 8

                    Rectangle {
                        visible: sheet.detectedKind !== ""
                        implicitHeight: 24
                        implicitWidth: badgeRow.implicitWidth + 18
                        radius: height / 2
                        color: sheet.detectedKind === "arxiv" ? theme.cloudSoft
                             : sheet.detectedKind === "article" ? theme.accentSoft
                             : Ui.tint(theme.ink1, 0.08)
                        border.width: 1
                        border.color: sheet.detectedKind === "arxiv" ? Ui.tint(theme.cloud, 0.30)
                                    : sheet.detectedKind === "article" ? Ui.tint(theme.accent, 0.25)
                                    : theme.hairline

                        Row {
                            id: badgeRow
                            anchors.centerIn: parent
                            spacing: 6
                            Glyph {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 13
                                height: 13
                                path: sheet.detectionBadgeGlyph(sheet.detectedKind)
                                color: sheet.detectedKind === "arxiv" ? theme.cloudInk
                                     : sheet.detectedKind === "article" ? theme.accentInk
                                     : theme.ink2
                            }
                            Label {
                                anchors.verticalCenter: parent.verticalCenter
                                text: sheet.detectionBadgeText(sheet.detectedKind)
                                textFormat: Text.PlainText
                                color: sheet.detectedKind === "arxiv" ? theme.cloudInk
                                     : sheet.detectedKind === "article" ? theme.accentInk
                                     : theme.ink2
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        visible: sheet.detectedKind === ""
                        text: qsTr("Enter a link starting with http://, https://, or arxiv:")
                        color: theme.ink3
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }

                    Item { Layout.fillWidth: true; visible: sheet.detectedKind !== "" }
                }
            }

            // Optional collection selector
            RowLayout {
                Layout.fillWidth: true
                spacing: 12

                Label {
                    text: qsTr("Add to")
                    color: theme.ink2
                    font.pixelSize: 13
                }

                ComboBox {
                    id: collectionCombo
                    Layout.fillWidth: true
                    enabled: !sheet.busy
                    model: sheet.collectionOptions
                    textRole: "name"
                    valueRole: "id"
                    Accessible.name: qsTr("Target collection")
                    onActivated: (index) => {
                        if (index >= 0 && index < sheet.collectionOptions.length)
                            sheet.selectedCollectionId = sheet.collectionOptions[index].id
                    }
                }
            }

            // Live progress state while fetching / extracting / packaging
            Rectangle {
                Layout.fillWidth: true
                visible: sheet.busy
                implicitHeight: progressCol.implicitHeight + 20
                radius: Ui.radius(10, theme.radius)
                color: Ui.tint(theme.ink1, 0.04)
                border.width: 1
                border.color: theme.hairline

                ColumnLayout {
                    id: progressCol
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8

                        Label {
                            Layout.fillWidth: true
                            text: sheet.statusText !== "" ? sheet.statusText : qsTr("Working…")
                            textFormat: Text.PlainText
                            color: theme.ink1
                            font.pixelSize: 12
                            font.weight: Font.Medium
                            elide: Text.ElideRight
                        }

                        Label {
                            visible: sheet.importProgress > 0 && sheet.importProgress <= 1
                            text: Math.round(sheet.importProgress * 100) + "%"
                            color: theme.ink3
                            font.pixelSize: 11
                            font.features: ({ "tnum": 1 })
                        }
                    }

                    ProgressBar {
                        Layout.fillWidth: true
                        indeterminate: sheet.importProgress < 0
                        value: Math.max(0, Math.min(1, sheet.importProgress))
                    }
                }
            }

            // Error state
            Rectangle {
                Layout.fillWidth: true
                visible: sheet.errorMessage !== "" && !sheet.busy
                implicitHeight: errorRow.implicitHeight + 20
                radius: Ui.radius(10, theme.radius)
                color: Ui.tint(theme.danger, 0.10)
                border.width: 1
                border.color: Ui.tint(theme.danger, 0.30)

                RowLayout {
                    id: errorRow
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    Glyph {
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        Layout.alignment: Qt.AlignTop
                        path: Icons.infoCircle
                        color: theme.danger
                    }
                    Label {
                        Layout.fillWidth: true
                        text: sheet.errorMessage
                        textFormat: Text.PlainText
                        color: theme.danger
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                    }
                }
            }

            // Success confirmation banner with quick open
            Rectangle {
                Layout.fillWidth: true
                visible: sheet.succeeded
                implicitHeight: successRow.implicitHeight + 22
                radius: Ui.radius(10, theme.radius)
                color: theme.positiveSoft
                border.width: 1
                border.color: Ui.tint(theme.positive, 0.32)

                RowLayout {
                    id: successRow
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 10

                    Glyph {
                        Layout.preferredWidth: 18
                        Layout.preferredHeight: 18
                        path: Icons.checkCircle
                        color: theme.positiveInk
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        Label {
                            Layout.fillWidth: true
                            text: sheet.importedTitle !== ""
                                  ? qsTr("Added “%1”").arg(sheet.importedTitle)
                                  : qsTr("Added to your library")
                            textFormat: Text.PlainText
                            color: theme.ink1
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                        }
                        Label {
                            Layout.fillWidth: true
                            text: sheet.importedKind === "article"
                                  ? qsTr("Saved offline as an EPUB and ready to read.")
                                  : sheet.importedKind === "arxiv"
                                  ? qsTr("Downloaded arXiv PDF with paper metadata.")
                                  : qsTr("Saved offline and ready to read.")
                            color: theme.ink3
                            font.pixelSize: 12
                            wrapMode: Text.Wrap
                        }
                    }
                }
            }

            // Footer actions
            RowLayout {
                Layout.fillWidth: true
                Layout.topMargin: 4
                spacing: 8

                TextButton {
                    visible: sheet.succeeded
                    ghost: true
                    text: qsTr("Add another")
                    onClicked: sheet.resetForAnother()
                }

                Item { Layout.fillWidth: true }

                TextButton {
                    visible: sheet.busy
                    text: qsTr("Cancel")
                    onClicked: if (sheet.hasImporter) webImporter.cancel()
                }

                TextButton {
                    visible: !sheet.busy && !sheet.succeeded
                    ghost: true
                    text: qsTr("Cancel")
                    onClicked: sheet.close()
                }

                TextButton {
                    visible: !sheet.busy && !sheet.succeeded
                    primary: true
                    enabled: sheet.detectedKind !== "" && sheet.hasImporter
                    glyph: Icons.download
                    hint: "↵"
                    text: sheet.detectedKind === "article" ? qsTr("Save offline") : qsTr("Add to library")
                    onClicked: sheet.submit()
                }

                TextButton {
                    visible: sheet.succeeded
                    text: qsTr("Done")
                    onClicked: sheet.close()
                }

                TextButton {
                    id: openNowBtn
                    visible: sheet.succeeded
                    primary: true
                    glyph: Icons.book
                    hint: "↵"
                    text: qsTr("Open now")
                    Keys.onReturnPressed: clicked()
                    Keys.onEnterPressed: clicked()
                    onClicked: {
                        const bid = sheet.importedBookId
                        sheet.open = false
                        sheet.closed()
                        sheet.openBookRequested(bid)
                    }
                }
            }
        }
    }
}
