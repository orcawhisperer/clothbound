import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// The whole book as a line: where you are, where the chapters begin, and a
// drag to anywhere, with the chapter named while dragging.
Item {
    id: scrubber

    property var look

    signal go(var target)

    implicitHeight: 54

    readonly property real shown: dragging ? dragValue : reader.progress
    property bool dragging: false
    property real dragValue: 0

    Label {
        id: preview
        visible: scrubber.dragging
        anchors.bottom: track.top
        anchors.bottomMargin: 10
        x: Math.max(0, Math.min(scrubber.width - width, track.x + track.width * scrubber.dragValue - width / 2))
        text: {
            const target = reader.targetForFraction(scrubber.dragValue)
            const percent = Math.round(scrubber.dragValue * 100).toLocaleString(Qt.locale(translator.language), 'f', 0)
            const title = reader.titleOfChapter(target.chapter)
            return title !== "" ? qsTr("%1 · %2%").arg(title).arg(percent) : qsTr("%1%").arg(percent)
        }
        color: scrubber.look.fg
        font.pixelSize: 13
        padding: 8
        background: Rectangle {
            radius: Math.min(6, theme.radius)
            color: scrubber.look.popover
            border.color: scrubber.look.border
        }
    }

    Item {
        id: track
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: 24

        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 4
            radius: 2
            color: Ui.tint(scrubber.look.fg, 0.14)
        }
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width * scrubber.shown
            height: 4
            radius: 2
            color: scrubber.look.link
        }
        Repeater {
            model: reader.chapterMarks
            delegate: Rectangle {
                required property real modelData
                x: track.width * modelData - 1
                anchors.verticalCenter: parent.verticalCenter
                width: 2
                height: 10
                radius: 1
                color: Ui.tint(scrubber.look.fg, 0.32)
            }
        }
        Rectangle {
            x: track.width * scrubber.shown - width / 2
            anchors.verticalCenter: parent.verticalCenter
            width: scrubber.dragging || hover.hovered ? 16 : 12
            height: width
            radius: width / 2
            color: scrubber.look.link
            Behavior on width { NumberAnimation { duration: 100 } }
        }

        HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
        DragHandler {
            target: null
            xAxis.enabled: true
            yAxis.enabled: false
            onActiveChanged: {
                if (active) {
                    scrubber.dragging = true
                } else {
                    scrubber.dragging = false
                    scrubber.go(reader.targetForFraction(scrubber.dragValue))
                }
            }
            onCentroidChanged: if (active) scrubber.dragValue = Math.max(0, Math.min(1, centroid.position.x / track.width))
        }
        TapHandler {
            onTapped: (point) => scrubber.go(reader.targetForFraction(Math.max(0, Math.min(1, point.position.x / track.width))))
        }
    }
}
