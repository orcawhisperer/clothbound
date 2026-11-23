import QtQuick
import QtQuick.Controls
import QtQuick.Shapes

import "Icons.js" as Icons
import "Ui.js" as Ui

// Journey J1 — An empty library (10_j1_empty_library.html): 72 px wireframe
// DogearMark, Literata 28 px "No books yet", auto-detected folder card with
// EPUB/PDF counts and one-click "Add this folder", secondary folder/file
// buttons, and a 520×96 dashed dropzone box.
Column {
    id: empty

    signal addFolders()
    signal addBooks()

    readonly property var offer: libraryFinder.found.length > 0 ? libraryFinder.found[0] : null
    readonly property string offerSummary: {
        const _lang = translator.language
        if (!offer)
            return ""
        const epub = offer.epubCount || 0
        const pdf = offer.pdfCount || 0
        if (epub > 0 && pdf > 0)
            return qsTr("%1 EPUB and %2 PDF files")
                .arg(epub.toLocaleString(Qt.locale(), "f", 0))
                .arg(pdf.toLocaleString(Qt.locale(), "f", 0))
        if (epub > 0)
            return qsTr("%n EPUB files", "", epub)
        if (pdf > 0)
            return qsTr("%n PDF files", "", pdf)
        return qsTr("%n books", "", offer.count || 0)
    }

    spacing: 24
    width: Math.min(520, parent ? parent.width - 48 : 520)

    onVisibleChanged: if (visible) libraryFinder.find(library.folders)
    Component.onCompleted: if (visible) libraryFinder.find(library.folders)

    // Hero mark, heading & lead.
    Column {
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(440, parent.width)
        spacing: 14

        DogearMark {
            anchors.horizontalCenter: parent.horizontalCenter
            wireframe: true
            size: 72
        }

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("No books yet")
            color: theme.ink1
            font.family: "Literata"
            font.pixelSize: 28
            font.weight: Font.Medium
        }

        Label {
            width: parent.width
            horizontalAlignment: Text.AlignHCenter
            text: qsTr("Point Dogear at the folder your books live in. New books there join by themselves, and your files are never changed.")
            color: theme.ink3
            font.pixelSize: 14
            wrapMode: Text.Wrap
            lineHeight: 1.55
        }
    }

    // Auto-detected folder card: one click adds ~/Books.
    AbstractButton {
        id: offerCard
        visible: empty.offer !== null
        width: parent.width
        height: 74
        hoverEnabled: true
        focusPolicy: Qt.StrongFocus
        Accessible.name: empty.offer ? qsTr("Found books in %1, add this folder").arg(empty.offer.label) : ""
        onClicked: if (empty.offer) library.addFolder(empty.offer.path)
        Keys.onReturnPressed: clicked()
        Keys.onEnterPressed: clicked()

        background: Rectangle {
            radius: Ui.radius(12, theme.radius)
            color: offerCard.pressed ? theme.fillPressed
                 : offerCard.hovered ? theme.fillHover : Ui.tint(theme.ink1, 0.04)
            border.width: offerCard.visualFocus ? 2 : 1
            border.color: offerCard.visualFocus ? theme.accent : Ui.tint(theme.ink1, 0.09)
            Behavior on color { ColorAnimation { duration: 100 } }
        }
        contentItem: Item {
            Rectangle {
                id: folderTile
                anchors.left: parent.left
                anchors.leftMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                width: 40
                height: 40
                radius: Ui.radius(10, theme.radius)
                color: theme.accentSoft
                Glyph {
                    anchors.centerIn: parent
                    width: 20
                    height: 20
                    stroke: 1.7
                    path: Icons.folder
                    color: theme.accentInk
                }
            }
            Column {
                anchors.left: folderTile.right
                anchors.leftMargin: 14
                anchors.right: addPill.left
                anchors.rightMargin: 12
                anchors.verticalCenter: parent.verticalCenter
                spacing: 3
                Label {
                    width: parent.width
                    text: empty.offer ? qsTr("Found books in %1").arg(empty.offer.label) : ""
                    textFormat: Text.PlainText
                    color: theme.ink1
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    elide: Text.ElideMiddle
                }
                Label {
                    width: parent.width
                    text: empty.offerSummary
                    color: theme.ink3
                    font.pixelSize: 12
                    font.features: ({ "tnum": 1 })
                    elide: Text.ElideRight
                }
            }
            Rectangle {
                id: addPill
                anchors.right: parent.right
                anchors.rightMargin: 18
                anchors.verticalCenter: parent.verticalCenter
                width: addText.implicitWidth + 28
                height: 32
                radius: Ui.radius(7, theme.radius)
                color: offerCard.pressed ? Qt.darker(theme.accent, 1.12)
                     : offerCard.hovered ? Qt.lighter(theme.accent, 1.05) : theme.accent
                Label {
                    id: addText
                    anchors.centerIn: parent
                    text: qsTr("Add this folder")
                    color: theme.accentOn
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }
            }
        }
    }

    // Secondary folder & single-book picker buttons.
    Row {
        anchors.horizontalCenter: parent.horizontalCenter
        spacing: 8
        TextButton {
            primary: empty.offer === null
            text: qsTr("Choose a folder…")
            onClicked: empty.addFolders()
        }
        TextButton {
            text: qsTr("Add single books…")
            onClicked: empty.addBooks()
        }
    }

    // 520×96 dashed dropzone box (10_j1_empty_library.html).
    Item {
        id: dropZone
        width: parent.width
        height: 96
        readonly property real r: Ui.radius(12, theme.radius)

        Shape {
            anchors.fill: parent
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeColor: Ui.tint(theme.ink1, 0.16)
                strokeWidth: 1.5
                strokeStyle: ShapePath.DashLine
                dashPattern: [5, 4]
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                PathSvg {
                    path: {
                        const w = dropZone.width - 2
                        const h = dropZone.height - 2
                        const r = dropZone.r
                        if (r <= 0)
                            return "M1 1 H" + (w + 1) + " V" + (h + 1) + " H1 Z"
                        return "M" + (1 + r) + " 1 H" + (w + 1 - r)
                             + " Q" + (w + 1) + " 1 " + (w + 1) + " " + (1 + r)
                             + " V" + (h + 1 - r)
                             + " Q" + (w + 1) + " " + (h + 1) + " " + (w + 1 - r) + " " + (h + 1)
                             + " H" + (1 + r)
                             + " Q1 " + (h + 1) + " 1 " + (h + 1 - r)
                             + " V" + (1 + r)
                             + " Q1 1 " + (1 + r) + " 1 Z"
                    }
                }
            }
        }

        Row {
            anchors.centerIn: parent
            spacing: 10
            Glyph {
                anchors.verticalCenter: parent.verticalCenter
                width: 18
                height: 18
                stroke: 1.8
                path: Icons.download
                color: theme.ink4
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: libraryFinder.searching
                      ? qsTr("Looking for books…")
                      : qsTr("Or drop books and folders anywhere on the window")
                color: theme.ink4
                font.pixelSize: 13
            }
        }
    }
}
