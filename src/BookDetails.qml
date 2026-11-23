import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Format.js" as Format
import "Icons.js" as Icons
import "Ui.js" as Ui

// Everything about one book, beside the grid: its cover, description, how
// long it takes to read, and, for a work with several editions, the
// editions to choose between.
FocusScope {
    id: details

    property int bookId: 0
    property string workKey
    property bool open: false

    readonly property var book: bookId > 0 && revision >= 0 ? library.book(bookId) : ({})
    readonly property var editions: workKey !== "" && revision >= 0 ? library.editions(workKey) : []
    property int revision: 0

    signal read(int bookId)
    signal closed()

    function show(id, key) {
        bookId = id
        workKey = key
        open = true
        forceActiveFocus()
    }
    function close() {
        if (!open)
            return
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

    Connections {
        target: library
        function onBooksChanged() { details.revision++ }
    }

    visible: open || panel.x < width
    Keys.onEscapePressed: close()
    Keys.onReturnPressed: read(bookId)
    Keys.onEnterPressed: read(bookId)
    Keys.onUpPressed: step(-1)
    Keys.onDownPressed: step(1)

    // A scrim over the grid; clicking it closes the panel.
    Rectangle {
        anchors.fill: parent
        color: "black"
        opacity: details.open ? (theme.light ? 0.18 : 0.35) : 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        TapHandler { onTapped: details.close() }
    }

    Rectangle {
        id: panel
        width: Math.min(480, details.width - 40)
        height: details.height
        x: details.open ? details.width - width : details.width
        Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
        color: theme.background
        border.color: Ui.tint(theme.text, 0.08)

        // Clicks inside the panel stay inside it.
        TapHandler {}

        IconButton {
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 12
            z: 2
            glyph: Icons.close
            tip: qsTr("Close (Esc)")
            onClicked: details.close()
        }

        Flickable {
            id: flick
            anchors.fill: parent
            contentHeight: content.implicitHeight + 64
            boundsBehavior: Flickable.StopAtBounds
            clip: true
            ScrollBar.vertical: ScrollBar {}

            ColumnLayout {
                id: content
                x: 32
                y: 40
                width: panel.width - 64
                spacing: 6

                Item {
                    Layout.fillWidth: true
                    Layout.preferredHeight: 300
                    Rectangle {
                        x: cover.x + cover.box.x
                        y: cover.y + cover.box.y + 6
                        width: cover.box.width
                        height: cover.box.height
                        radius: cover.radius
                        color: "black"
                        opacity: theme.light ? 0.16 : 0.38
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
                    color: theme.text
                    font.family: "Literata"
                    font.pixelSize: 24
                    font.weight: Font.DemiBold
                    wrapMode: Text.Wrap
                    lineHeight: 1.1
                }
                Label {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: details.book.author || ""
                    textFormat: Text.PlainText
                    color: theme.textSecondary
                    font.pixelSize: 16
                    wrapMode: Text.Wrap
                }
                Label {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: details.book.editionLine || ""
                    textFormat: Text.PlainText
                    color: theme.textTertiary
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }

                Label {
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                    visible: (details.book.problem || "") !== ""
                    text: details.book.problem || ""
                    textFormat: Text.PlainText
                    color: theme.colors.red || "#ff6b6b"
                    font.pixelSize: 13
                    wrapMode: Text.Wrap
                }

                TextButton {
                    Layout.topMargin: 16
                    Layout.fillWidth: true
                    primary: true
                    enabled: (details.book.problem || "") === ""
                    glyph: Icons.book
                    hint: "↵"
                    text: details.book.finished ? qsTr("Read again")
                        : (details.book.progress || 0) > 0.005
                          ? qsTr("Continue reading · %1%").arg(Math.round(details.book.progress * 100).toLocaleString(Qt.locale(translator.language), 'f', 0))
                          : qsTr("Read")
                    onClicked: details.read(details.bookId)
                }

                // Facts, in a quiet two-column table.
                GridLayout {
                    Layout.topMargin: 18
                    Layout.fillWidth: true
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 6

                    component Fact: Label {
                        color: theme.textTertiary
                        font.pixelSize: 13
                    }
                    component Value: Label {
                        Layout.fillWidth: true
                        textFormat: Text.PlainText
                        color: theme.text
                        font.pixelSize: 13
                        wrapMode: Text.Wrap
                    }

                    Fact { text: qsTr("Format") }
                    Value {
                        text: (details.book.format || "").toUpperCase() + " · "
                              + Format.size(details.book.size || 0, Qt.locale(translator.language))
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

                    Fact { text: qsTr("Language"); visible: languageValue.text !== "" }
                    Value {
                        id: languageValue
                        visible: text !== ""
                        text: details.book.language ? Qt.locale(details.book.language).nativeLanguageName || details.book.language : ""
                    }

                    Fact { text: qsTr("ISBN"); visible: (details.book.isbn || "") !== "" }
                    Value { visible: text !== ""; text: details.book.isbn || "" }

                    Fact { text: qsTr("Series"); visible: (details.book.series || "") !== "" }
                    Value { visible: text !== ""; text: details.book.series || "" }
                }

                // Editions of the same work, to choose between.
                Label {
                    Layout.topMargin: 22
                    visible: details.editions.length > 1
                    text: qsTr("%n editions", "", details.editions.length)
                    color: theme.text
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    visible: details.editions.length > 1
                    text: qsTr("↑ ↓ to choose, ↵ to read")
                    color: theme.textTertiary
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
                        radius: Math.min(8, theme.radius)
                        color: chosen ? Ui.tint(theme.accent, 0.16) : editionHover.hovered ? Ui.tint(theme.text, 0.06) : "transparent"
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
                                color: theme.text
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
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
                                color: theme.textSecondary
                                font.pixelSize: 12
                                elide: Text.ElideRight
                            }
                        }
                        HoverHandler { id: editionHover; cursorShape: Qt.PointingHandCursor }
                        TapHandler {
                            onTapped: details.bookId = edition.modelData.id
                            onDoubleTapped: details.read(edition.modelData.id)
                        }
                    }
                }

                Label {
                    Layout.topMargin: 22
                    visible: (details.book.description || "") !== ""
                    text: qsTr("About this book")
                    color: theme.text
                    font.pixelSize: 15
                    font.weight: Font.DemiBold
                }
                Label {
                    Layout.fillWidth: true
                    visible: text !== ""
                    text: details.book.description || ""
                    color: theme.textSecondary
                    font.pixelSize: 14
                    wrapMode: Text.Wrap
                    lineHeight: 1.35
                    textFormat: Text.PlainText
                }

                RowLayout {
                    Layout.topMargin: 24
                    Layout.fillWidth: true
                    spacing: 8
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
                    Item { Layout.fillWidth: true }
                    IconButton {
                        glyph: Icons.trash
                        tip: qsTr("Remove from the library (the file stays)")
                        tone: theme.textSecondary
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
                    color: theme.textTertiary
                    font.pixelSize: 11
                    wrapMode: Text.WrapAnywhere
                }
            }
        }
    }
}
