import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Format.js" as Format
import "Icons.js" as Icons
import "Ui.js" as Ui

// Everything about one book, beside the grid: its cover, description, how
// long it takes to read, collections, and, for a work with several editions,
// the editions to choose between.
FocusScope {
    id: details

    property int bookId: 0
    property string workKey
    property bool open: false

    readonly property var book: bookId > 0 && revision >= 0 ? library.book(bookId) : ({})
    readonly property var editions: workKey !== "" && revision >= 0 ? library.editions(workKey) : []
    readonly property var bookStats: bookId > 0 && revision >= 0 ? library.bookReadingStats(bookId) : ({})
    readonly property var allCollections: revision >= 0 ? library.collections : []
    property int revision: 0
    property bool addingShelf: false

    signal read(int bookId)
    signal listen(int bookId)
    signal closed()

    function show(id, key) {
        bookId = id
        workKey = key
        addingShelf = false
        newShelfInput.text = ""
        flick.contentY = 0
        open = true
        forceActiveFocus()
    }
    function close() {
        if (!open)
            return
        addingShelf = false
        open = false
        closed()
    }
    function step(delta) {
        if (editions.length < 2)
            return
        let at = 0
        for (let i = 0; i < editions.length; ++i)
            if (editions[i].id === bookId)
                at = i
        bookId = editions[Math.max(0, Math.min(editions.length - 1, at + delta))].id
    }
    function formatReadDuration(seconds) {
        const mins = Math.round((seconds || 0) / 60)
        if (seconds <= 0)
            return ""
        if (mins < 1)
            return qsTr("Under 1 min")
        return Format.duration(mins)
    }
    function createAndAssignShelf() {
        const name = newShelfInput.text.trim()
        if (name === "" || details.bookId <= 0)
            return
        const colId = library.createCollection(name)
        if (colId > 0)
            library.addToCollection(details.bookId, colId)
        newShelfInput.text = ""
        addingShelf = false
        details.forceActiveFocus()
    }

    Connections {
        target: library
        function onBooksChanged() { details.revision++ }
        function onBookChanged(id) { if (id === details.bookId) details.revision++ }
        function onCollectionsChanged() { details.revision++ }
        function onReadingStatsChanged() { details.revision++ }
    }

    visible: open || panel.x < width
    Keys.onEscapePressed: {
        if (addingShelf) {
            addingShelf = false
            details.forceActiveFocus()
        } else {
            close()
        }
    }
    Keys.onReturnPressed: {
        if (!addingShelf && (details.book.problem || "") === "")
            read(bookId)
    }
    Keys.onEnterPressed: {
        if (!addingShelf && (details.book.problem || "") === "")
            read(bookId)
    }
    Keys.onUpPressed: if (!addingShelf) step(-1)
    Keys.onDownPressed: if (!addingShelf) step(1)

    // A scrim over the grid; clicking it closes the panel.
    Rectangle {
        anchors.fill: parent
        color: theme.scrim
        opacity: details.open ? 1 : 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        TapHandler { onTapped: details.close() }
    }

    Rectangle {
        id: panel
        width: Math.min(480, details.width - 40)
        height: details.height
        x: details.open ? details.width - width : details.width
        Behavior on x {
            enabled: !theme.reducedMotion
            NumberAnimation { duration: 350; easing.type: Easing.OutQuart }
        }
        color: theme.raised
        border.width: 1
        border.color: theme.hairline

        // Clicks inside the panel stay inside it.
        TapHandler {}

        // Pinned top header so the close button never collides with scrolled text.
        Rectangle {
            id: panelHeader
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.right: parent.right
            height: 52
            z: 3
            color: theme.raised

            Label {
                anchors.left: parent.left
                anchors.leftMargin: 32
                anchors.right: closeBtn.left
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                text: details.book.title || ""
                textFormat: Text.PlainText
                color: theme.ink1
                font.family: "Literata"
                font.pixelSize: 15
                font.weight: Font.Medium
                elide: Text.ElideRight
                opacity: flick.contentY > 280 ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 150 } }
            }

            IconButton {
                id: closeBtn
                anchors.right: parent.right
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                glyph: Icons.close
                tip: qsTr("Close (Esc)")
                onClicked: details.close()
            }

            Rectangle {
                anchors.bottom: parent.bottom
                anchors.left: parent.left
                anchors.right: parent.right
                height: 1
                color: theme.hairline
                opacity: flick.contentY > 4 ? 1 : 0
                Behavior on opacity { NumberAnimation { duration: 140 } }
            }
        }

        Flickable {
            id: flick
            anchors.top: panelHeader.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            contentHeight: content.implicitHeight + 48
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            ScrollBar.vertical: ScrollBar {}

            ColumnLayout {
                id: content
                x: 32
                y: 12
                width: panel.width - 64
                spacing: 6

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 300
                    Rectangle {
                        x: cover.x + cover.box.x
                        y: cover.y + cover.box.y + 8
                        width: cover.box.width
                        height: cover.box.height
                        radius: cover.radius
                        color: "black"
                        opacity: theme.light ? 0.16 : 0.42
                    }
                    CoverImage {
                        id: cover
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: 200
                        height: 300
                        source: details.book.cover || ""
                        title: details.book.title || ""
                        author: details.book.author || ""
                    }
                }

                Item { Layout.preferredHeight: 18 }

                Label {
                    Layout.fillWidth: true
                    text: details.book.title || ""
                    textFormat: Text.PlainText
                    color: theme.ink1
                    font.family: "Literata"
                    font.pixelSize: 24
                    font.weight: Font.Medium
                    wrapMode: Text.Wrap
                    lineHeight: 1.12
                }
                Label {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: details.book.author || ""
                    textFormat: Text.PlainText
                    color: theme.ink2
                    font.pixelSize: 15
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: details.book.editionLine || ""
                    textFormat: Text.PlainText
                    color: theme.ink3
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }

                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    visible: (details.book.problem || "") !== ""
                    text: details.book.problem || ""
                    textFormat: Text.PlainText
                    color: theme.danger
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }

                RowLayout {
                    Layout.topMargin: 16
                    Layout.fillWidth: true
                    spacing: 8

                    TextButton {
                        Layout.fillWidth: true
                        primary: true
                        size: "large"
                        enabled: (details.book.problem || "") === ""
                        glyph: Icons.book
                        hint: "↵"
                        text: {
                            const _lang = translator.language
                            return details.book.finished ? qsTr("Read again")
                                : (details.book.progress || 0) > 0.005
                                  ? qsTr("Continue · %1%").arg(Math.round(details.book.progress * 100).toLocaleString(Qt.locale(), "f", 0))
                                  : qsTr("Read")
                        }
                        onClicked: details.read(details.bookId)
                    }

                    TextButton {
                        size: "large"
                        enabled: (details.book.problem || "") === ""
                        glyph: Icons.headphones
                        text: qsTr("Listen")
                        onClicked: details.listen(details.bookId)
                    }
                }

                // Facts, in a quiet two-column table.
                GridLayout {
                    Layout.topMargin: 18
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 6

                    component Fact: Label {
                        color: theme.ink3
                        font.pixelSize: 13
                    }
                    component Value: Label {
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        color: theme.ink1
                        font.pixelSize: 13
                        font.features: ({ "tnum": 1 })
                        wrapMode: Text.Wrap
                    }

                    Fact { text: qsTr("Format") }
                    Value {
                        text: {
                            const _lang = translator.language
                            return (details.book.format || "").toUpperCase() + " · "
                                   + Format.size(details.book.size || 0, Qt.locale())
                        }
                    }

                    Fact { text: qsTr("Length"); visible: lengthValue.text !== "" }
                    Value {
                        id: lengthValue
                        visible: text !== ""
                        text: {
                            const parts = []
                            if ((details.book.pages || 0) > 0)
                                parts.push(qsTr("%n pages", "", details.book.pages))
                            if (details.book.format === "epub" && (details.book.chapters || 0) > 0)
                                parts.push(qsTr("%n chapters", "", details.book.chapters))
                            return parts.join(" · ")
                        }
                    }

                    Fact { text: details.book.progress > 0.005 && !details.book.finished ? qsTr("Time left") : qsTr("Reading time"); visible: timeValue.text !== "" }
                    Value {
                        id: timeValue
                        visible: text !== ""
                        text: (details.book.minutesLeft || 0) > 0 && translator.language !== "" ? qsTr("About %1").arg(Format.duration(details.book.minutesLeft)) : ""
                    }

                    Fact { text: qsTr("Time read"); visible: readTimeValue.text !== "" }
                    Value {
                        id: readTimeValue
                        visible: text !== ""
                        text: {
                            const sec = details.book.secondsRead || 0
                            if (sec <= 0)
                                return ""
                            const base = details.formatReadDuration(sec)
                            const days = details.bookStats.daysRead || 0
                            if (days > 0)
                                return base + " · " + qsTr("%n days", "", days)
                            return base
                        }
                    }

                    Fact { text: qsTr("Last read"); visible: lastReadValue.text !== "" }
                    Value {
                        id: lastReadValue
                        visible: text !== ""
                        text: {
                            const _lang = translator.language
                            const day = details.bookStats.lastDay || ""
                            if (day === "")
                                return ""
                            const d = Date.fromLocaleString(Qt.locale("C"), day, "yyyy-MM-dd")
                            return isNaN(d.getTime()) ? day : d.toLocaleDateString(Qt.locale(), Locale.ShortFormat)
                        }
                    }

                    Fact { text: qsTr("Language"); visible: languageValue.text !== "" }
                    Value {
                        id: languageValue
                        visible: text !== ""
                        text: details.book.language ? Qt.locale(details.book.language).nativeLanguageName || details.book.language : ""
                    }

                    Fact { text: qsTr("ISBN"); visible: (details.book.isbn || "") !== "" }
                    Value { visible: text !== ""; text: details.book.isbn || "" }

                    Fact { text: qsTr("Series"); visible: (details.book.series || "") !== "" }
                    Value {
                        visible: text !== ""
                        text: {
                            const s = details.book.series || ""
                            const idx = details.book.seriesIndex || 0
                            if (s === "")
                                return ""
                            return idx > 0 ? (s + " · #" + (Math.floor(idx) === idx ? Math.floor(idx) : idx)) : s
                        }
                    }
                }

                // Collections membership with color swatches.
                Label {
                    Layout.topMargin: 18
                    text: qsTr("Collections")
                    color: theme.ink4
                    font.pixelSize: 11
                    font.weight: Font.DemiBold
                    font.letterSpacing: 0.22
                }
                Flow {
                    Layout.fillWidth: true
                    spacing: 6

                    Repeater {
                        model: details.allCollections
                        delegate: AbstractButton {
                            id: shelfPill
                            required property var modelData
                            readonly property bool member: details.revision >= 0 && library.isInCollection(details.bookId, modelData.id)
                            implicitHeight: 30
                            implicitWidth: shelfPillRow.implicitWidth + 20
                            hoverEnabled: true
                            focusPolicy: Qt.TabFocus
                            onClicked: {
                                if (member)
                                    library.removeFromCollection(details.bookId, modelData.id)
                                else
                                    library.addToCollection(details.bookId, modelData.id)
                            }
                            background: Rectangle {
                                radius: Ui.radius(15, theme.radius)
                                color: shelfPill.member ? theme.accentSoft
                                     : shelfPill.hovered ? Ui.tint(theme.ink1, 0.08) : Ui.tint(theme.ink1, 0.04)
                                border.width: shelfPill.visualFocus ? 2 : 1
                                border.color: shelfPill.visualFocus || shelfPill.member ? theme.accent : Ui.tint(theme.ink1, 0.10)
                            }
                            contentItem: Item {
                                Row {
                                    id: shelfPillRow
                                    anchors.centerIn: parent
                                    spacing: 6
                                    Rectangle {
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 8
                                        height: 8
                                        radius: Ui.radius(2, theme.radius)
                                        color: Ui.collectionColor(shelfPill.modelData.name)
                                    }
                                    Label {
                                        anchors.verticalCenter: parent.verticalCenter
                                        text: shelfPill.modelData.name
                                        textFormat: Text.PlainText
                                        color: theme.ink1
                                        font.pixelSize: 12
                                        font.weight: shelfPill.member ? Font.Medium : Font.Normal
                                    }
                                    Glyph {
                                        visible: shelfPill.member
                                        anchors.verticalCenter: parent.verticalCenter
                                        width: 12
                                        height: 12
                                        stroke: 2.3
                                        path: Icons.check
                                        color: theme.accentInk
                                    }
                                }
                            }
                        }
                    }

                    AbstractButton {
                        id: addShelfBtn
                        visible: !details.addingShelf
                        implicitHeight: 30
                        implicitWidth: addShelfLabel.implicitWidth + 20
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        onClicked: {
                            details.addingShelf = true
                            newShelfInput.forceActiveFocus()
                        }
                        background: Rectangle {
                            radius: Ui.radius(15, theme.radius)
                            color: addShelfBtn.hovered ? Ui.tint(theme.ink1, 0.08) : "transparent"
                            border.width: addShelfBtn.visualFocus ? 2 : 1
                            border.color: addShelfBtn.visualFocus ? theme.accent : Ui.tint(theme.ink1, 0.14)
                        }
                        contentItem: Label {
                            id: addShelfLabel
                            text: qsTr("+ New collection")
                            color: theme.ink3
                            font.pixelSize: 12
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    visible: details.addingShelf
                    spacing: 6
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 32
                        radius: Ui.radius(7, theme.radius)
                        color: Ui.tint(theme.ink1, 0.06)
                        border.width: 1.5
                        border.color: theme.accent
                        TextInput {
                            id: newShelfInput
                            anchors.fill: parent
                            anchors.leftMargin: 10
                            anchors.rightMargin: 10
                            verticalAlignment: TextInput.AlignVCenter
                            color: theme.ink1
                            font.pixelSize: 13
                            selectByMouse: true
                            Keys.onReturnPressed: details.createAndAssignShelf()
                            Keys.onEnterPressed: details.createAndAssignShelf()
                            Keys.onEscapePressed: {
                                details.addingShelf = false
                                details.forceActiveFocus()
                            }
                            Text {
                                anchors.fill: parent
                                verticalAlignment: Text.AlignVCenter
                                visible: newShelfInput.text === ""
                                text: qsTr("Collection name…")
                                color: theme.ink4
                                font: newShelfInput.font
                            }
                        }
                    }
                    TextButton {
                        primary: true
                        text: qsTr("Add")
                        enabled: newShelfInput.text.trim() !== ""
                        onClicked: details.createAndAssignShelf()
                    }
                }

                // Editions of the same work, to choose between.
                Label {
                    Layout.topMargin: 22
                    visible: details.editions.length > 1
                    text: qsTr("%n editions", "", details.editions.length)
                    color: theme.ink1
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    visible: details.editions.length > 1
                    text: qsTr("↑ ↓ to choose, ↵ to read")
                    color: theme.ink3
                    font.pixelSize: 12
                }
                Repeater {
                    model: details.editions.length > 1 ? details.editions : []
                    delegate: Rectangle {
                        id: edition
                        required property var modelData
                        readonly property bool chosen: modelData.id === details.bookId
                        Layout.fillWidth: true
                        Layout.preferredHeight: 76
                        radius: Ui.radius(8, theme.radius)
                        color: chosen ? theme.accentSoft : editionHover.hovered ? Ui.tint(theme.ink1, 0.06) : "transparent"
                        border.width: chosen ? 1.5 : 0
                        border.color: theme.accent

                        CoverImage {
                            id: editionCover
                            x: 10
                            y: 8
                            width: 40
                            height: 60
                            source: edition.modelData.cover
                            title: edition.modelData.title
                            author: edition.modelData.author
                        }
                        Column {
                            anchors.left: editionCover.right
                            anchors.leftMargin: 14
                            anchors.right: parent.right
                            anchors.rightMargin: 12
                            anchors.verticalCenter: parent.verticalCenter
                            spacing: 3
                            Label {
                                width: parent.width
                                text: edition.modelData.editionLine || edition.modelData.title
                                textFormat: Text.PlainText
                                color: theme.ink1
                                font.pixelSize: 13
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }
                            Label {
                                width: parent.width
                                text: {
                                    const parts = [edition.modelData.format.toUpperCase()]
                                    if (edition.modelData.finished)
                                        parts.push(qsTr("Finished"))
                                    else if (edition.modelData.progress > 0.005)
                                        parts.push(qsTr("%1% read").arg(Math.round(edition.modelData.progress * 100)))
                                    if (edition.modelData.title !== details.book.title)
                                        parts.push(edition.modelData.title)
                                    return parts.join(" · ")
                                }
                                textFormat: Text.PlainText
                                color: theme.ink3
                                font.pixelSize: 12
                                font.features: ({ "tnum": 1 })
                                elide: Text.ElideRight
                            }
                        }
                        HoverHandler { id: editionHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: details.bookId = edition.modelData.id
                            onDoubleTapped: {
                                if ((edition.modelData.problem || "") === "")
                                    details.read(edition.modelData.id)
                            }
                        }
                    }
                }

                Label {
                    Layout.topMargin: 22
                    visible: (details.book.description || "") !== ""
                    text: qsTr("About this book")
                    color: theme.ink1
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: details.book.description || ""
                    color: theme.ink2
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                    lineHeight: 1.45
                    textFormat: Text.PlainText
                }

                // Actions wrap onto a second line in a narrow panel.
                Flow {
                    Layout.topMargin: 24
                    Layout.fillWidth: true
                    spacing: 8
                    TextButton {
                        visible: !details.book.finished
                        text: details.book.wantToRead ? qsTr("On Want to read") : qsTr("Want to read")
                        glyph: Icons.bookmark
                        onClicked: library.setWantToRead(details.bookId, !details.book.wantToRead)
                    }
                    TextButton {
                        text: details.book.finished ? qsTr("Mark unread") : qsTr("Mark finished")
                        glyph: Icons.check
                        onClicked: library.setFinished(details.bookId, !details.book.finished)
                    }
                    TextButton {
                        text: qsTr("Show file")
                        glyph: Icons.folder
                        onClicked: library.showInFolder(details.bookId)
                    }
                    IconButton {
                        glyph: Icons.trash
                        tip: qsTr("Remove from the library (the file stays)")
                        tone: theme.ink3
                        onClicked: {
                            const id = details.bookId
                            library.setHidden(id, true)
                            details.close()
                            toast.show(qsTr("Removed “%1” from the library").arg(details.book.title || ""), qsTr("Undo"),
                                       function() { library.setHidden(id, false) })
                        }
                    }
                }
                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: 10
                    text: details.book.path || ""
                    textFormat: Text.PlainText
                    color: theme.ink4
                    font.pixelSize: 11
                    wrapMode: Text.WrapAnywhere
                }
            }
        }
    }
}
