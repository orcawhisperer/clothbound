import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui

// The first thing a new library shows. When books turn up where people
// usually keep them, one click adds that folder; otherwise a folder or books
// can be chosen. Either way, books are read where they are and never changed.
Column {
    id: empty

    signal addFolders()
    signal addBooks()

    readonly property var offer: libraryFinder.found.length > 0 ? libraryFinder.found[0] : null

    spacing: 14
    width: 480

    onVisibleChanged: if (visible) libraryFinder.find(library.folders)
    Component.onCompleted: if (visible) libraryFinder.find(library.folders)

    Image {
        anchors.horizontalCenter: parent.horizontalCenter
        source: "qrc:/dogear.svg"
        sourceSize.width: 136
        sourceSize.height: 136
        width: 136
        height: 136
    }

    Label {
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        text: empty.offer ? qsTr("Your books are close by") : qsTr("Your library is empty")
        color: theme.ink1
        font.family: "Literata"
        font.pixelSize: 26
        font.weight: Font.DemiBold
    }
    Label {
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("Dogear reads EPUB and PDF books where they are, and never changes them.")
        color: theme.ink2
        font.pixelSize: 14
        wrapMode: Text.Wrap
        lineHeight: 1.3
    }

    Item { width: 1; height: 6 }

    // The folder found, as one card to accept.
    AbstractButton {
        id: offerCard
        visible: empty.offer !== null
        width: parent.width
        height: 72
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: empty.offer ? qsTr("Add %1").arg(empty.offer.label) : ""
        onClicked: if (empty.offer) library.addFolder(empty.offer.path)
        Keys.onReturnPressed: clicked()

        background: Rectangle {
            radius: Math.min(12, theme.radius)
            color: offerCard.pressed ? theme.fillPressed : offerCard.hovered ? theme.fillHover : theme.fill
            border.width: offerCard.visualFocus ? 2 : 1
            border.color: offerCard.visualFocus ? theme.accent : theme.hairline
        }
        contentItem: Item {
            Rectangle {
                id: folderTile
                anchors.left: parent.left
                anchors.leftMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                width: 40
                height: 40
                radius: Math.min(10, theme.radius)
                color: theme.accentSoft
                Glyph {
                    anchors.centerIn: parent
                    width: 20
                    height: 20
                    path: Icons.folder
                    color: theme.accentInk
                }
            }
            Column {
                anchors.left: folderTile.right
                anchors.leftMargin: 14
                anchors.right: addLabel.left
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3
                Label {
                    width: parent.width
                    text: empty.offer ? qsTr("Found %n books in %1", "", empty.offer.count).arg(empty.offer.label) : ""
                    color: theme.ink1
                    font.pixelSize: 14
                    font.weight: Font.DemiBold
                    elide: Text.ElideMiddle
                }
                Label {
                    width: parent.width
                    text: libraryFinder.found.length > 1
                          ? qsTr("And %n more places with books", "", libraryFinder.found.length - 1)
                          : qsTr("New books there are added each time Dogear starts")
                    color: theme.ink3
                    font.pixelSize: 12
                    elide: Text.ElideRight
                }
            }
            Rectangle {
                id: addLabel
                anchors.right: parent.right
                anchors.rightMargin: 16
                anchors.verticalCenter: parent.verticalCenter
                width: addText.implicitWidth + 28
                height: 32
                radius: Math.min(7, theme.radius)
                color: offerCard.pressed ? Qt.darker(theme.accent, 1.12) : theme.accent
                Label {
                    id: addText
                    anchors.centerIn: parent
                    text: qsTr("Add")
                    color: theme.accentForeground
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }
            }
        }
    }

    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 10
        TextButton {
            primary: empty.offer === null
            glyph: Icons.folder
            text: empty.offer ? qsTr("Another folder…") : qsTr("Add a folder…")
            hint: "A"
            onClicked: empty.addFolders()
        }
        TextButton {
            glyph: Icons.book
            text: qsTr("Add books…")
            onClicked: empty.addBooks()
        }
    }
    Label {
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        text: libraryFinder.searching ? qsTr("Looking for books…") : qsTr("You can also drop books or folders on this window.")
        color: theme.ink3
        font.pixelSize: 12
        wrapMode: Text.Wrap
    }
}
