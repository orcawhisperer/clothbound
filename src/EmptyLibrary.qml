import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons

// The first thing a new library shows: what to do, and the promise that
// books are read where they are and never changed.
Column {
    id: empty

    signal addFolders()
    signal addBooks()

    spacing: 14
    width: 460

    Image {
        anchors.horizontalCenter: parent.horizontalCenter
        source: "qrc:/dogear.svg"
        sourceSize.width: 168
        sourceSize.height: 168
        width: 168
        height: 168
    }

    Label {
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("Your library is empty")
        color: theme.text
        font.family: "Literata"
        font.pixelSize: 26
        font.weight: Font.DemiBold
    }
    Label {
        width: parent.width
        horizontalAlignment: Text.AlignHCenter
        text: qsTr("Add a folder of EPUB and PDF books. Dogear reads them where they are and never changes them.")
        color: theme.textSecondary
        font.pixelSize: 15
        wrapMode: Text.Wrap
        lineHeight: 1.3
    }

    Item { width: 1; height: 8 }

    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 10
        TextButton {
            primary: true
            glyph: Icons.folder
            text: qsTr("Add a folder…")
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
        text: qsTr("You can also drop books or folders on this window.")
        color: theme.textTertiary
        font.pixelSize: 13
        wrapMode: Text.Wrap
    }
}
