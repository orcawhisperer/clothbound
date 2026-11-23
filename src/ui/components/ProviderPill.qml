import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui

// A compact 26 px rounded provenance/routing pill: "[12 px icon] Label" (or a
// 6 px status dot + trailing up/down chevron when `interactive` / `showDot`),
// tinted in positiveSoft/positiveInk (local), cloudSoft/cloudInk (cloud), or
// neutral muted (unavailable/none).
AbstractButton {
    id: pill

    // "local", "cloud", or "none" / "unavailable".
    property string where: "local"
    property bool interactive: false
    property bool showDot: false
    property bool showIcon: !showDot && (where === "local" || where === "cloud")

    readonly property bool isLocal: where === "local"
    readonly property bool isCloud: where === "cloud"
    readonly property color bgTone: isLocal ? theme.positiveSoft
                                  : isCloud ? theme.cloudSoft
                                  : theme.fill
    readonly property color fgTone: isLocal ? theme.positiveInk
                                  : isCloud ? theme.cloudInk
                                  : theme.ink3
    readonly property color dotTone: isLocal ? theme.positive
                                   : isCloud ? theme.cloud
                                   : theme.ink4

    text: isLocal ? qsTr("This computer")
        : isCloud ? qsTr("Gemini")
        : qsTr("Unavailable")
    hoverEnabled: interactive
    focusPolicy: interactive ? Qt.TabFocus : Qt.NoFocus
    opacity: enabled ? 1 : 0.45
    implicitHeight: 26
    implicitWidth: row.implicitWidth + (interactive ? 18 : 16)
    Accessible.name: text

    background: Rectangle {
        radius: Ui.radius(6, theme.radius)
        color: pill.interactive && pill.pressed ? Ui.mix(pill.bgTone, theme.ink1, 0.10)
             : pill.interactive && pill.hovered ? Ui.mix(pill.bgTone, theme.ink1, 0.05)
             : pill.bgTone
        Behavior on color { ColorAnimation { duration: 100 } }

        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            visible: pill.visualFocus
            radius: Ui.radius(9, theme.radius)
            color: "transparent"
            border.width: 2
            border.color: pill.palette.highlight
        }
    }

    contentItem: Item {
        Row {
            id: row
            anchors.centerIn: parent
            spacing: 6

            Rectangle {
                visible: pill.showDot
                anchors.verticalCenter: parent.verticalCenter
                width: 6
                height: 6
                radius: 3
                color: pill.dotTone
            }
            Glyph {
                visible: pill.showIcon
                anchors.verticalCenter: parent.verticalCenter
                width: 12
                height: 12
                stroke: 1.9
                path: pill.isLocal ? Icons.computer : Icons.cloud
                color: pill.fgTone
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                text: pill.text
                color: pill.fgTone
                font.pixelSize: 12
                font.weight: Font.Medium
            }
            Glyph {
                visible: pill.interactive
                anchors.verticalCenter: parent.verticalCenter
                width: 9
                height: 9
                stroke: 2.4
                path: Icons.chevronsUpDown
                color: theme.ink3
            }
        }
    }

    HoverHandler {
        enabled: pill.interactive
        cursorShape: Qt.PointingHandCursor
    }
}
