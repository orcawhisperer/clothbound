import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// The whole book as a line: where you are, where the chapters begin, and a
// drag to anywhere, with the chapter named while dragging (02_reading.html).
Item {
    id: scrubber

    property var look

    signal go(var target)

    implicitHeight: 20

    readonly property real shown: dragging ? dragValue : reader.progress
    property bool dragging: false
    property real dragValue: 0
    readonly property color accentTone: look ? (look.accentInk || look.link) : palette.link
    readonly property color trackTone: look ? (look.track || Ui.tint(look.fg, 0.12)) : palette.midlight
    readonly property color tickTone: look ? (look.tick || Ui.tint(look.fg, 0.25)) : palette.dark

    Label {
        id: preview
        visible: scrubber.dragging
        anchors.bottom: track.top
        anchors.bottomMargin: 8
        x: Math.max(0, Math.min(scrubber.width - width, track.x + track.width * scrubber.dragValue - width / 2))
        text: {
            const target = reader.targetForFraction(scrubber.dragValue)
            const percent = Math.round(scrubber.dragValue * 100).toLocaleString(Qt.locale(), 'f', 0)
            const title = reader.titleOfChapter(target.chapter)
            return title !== "" ? qsTr("%1 · %2%").arg(title).arg(percent) : qsTr("%1%").arg(percent)
        }
        color: scrubber.look.fg
        font.pixelSize: 12
        font.weight: Font.Medium
        font.features: ({ "tnum": 1 })
        padding: 8
        leftPadding: 10
        rightPadding: 10
        background: Rectangle {
            radius: Ui.radius(7, theme.radius)
            color: scrubber.look.popover
            border.width: 1
            border.color: scrubber.look.line
            Rectangle {
                z: -1
                anchors.fill: parent
                anchors.topMargin: 4
                anchors.bottomMargin: -4
                radius: parent.radius
                color: "black"
                opacity: scrubber.look.dark ? 0.35 : 0.12
            }
        }
    }

    Item {
        id: track
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        height: 20

        // 3 px rounded track.
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width
            height: 3
            radius: 1.5
            color: scrubber.trackTone
        }
        // Progress fill.
        Rectangle {
            anchors.verticalCenter: parent.verticalCenter
            width: parent.width * Math.max(0, Math.min(1, scrubber.shown))
            height: 3
            radius: 1.5
            color: scrubber.accentTone
        }
        // 1 × 7 px chapter ticks protruding 2 px above and below the track.
        Repeater {
            model: reader.chapterMarks
            delegate: Rectangle {
                required property real modelData
                x: Math.round(track.width * modelData)
                anchors.verticalCenter: parent.verticalCenter
                width: 1
                height: 7
                color: scrubber.tickTone
            }
        }
        // 11 px circular thumb (13 px hovered/dragging) with a 3 px paper-cutout halo.
        Rectangle {
            readonly property real dotSize: scrubber.dragging || hover.hovered ? 13 : 11
            x: track.width * Math.max(0, Math.min(1, scrubber.shown)) - width / 2
            anchors.verticalCenter: parent.verticalCenter
            width: dotSize + 6
            height: width
            radius: width / 2
            color: scrubber.look.bg
            Behavior on dotSize {
                enabled: !theme.reducedMotion
                NumberAnimation { duration: 100 }
            }
            Rectangle {
                anchors.centerIn: parent
                width: parent.dotSize
                height: width
                radius: width / 2
                color: scrubber.accentTone
            }
        }

        HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
        DragHandler {
            target: null
            xAxis.enabled: true
            yAxis.enabled: false
            onActiveChanged: {
                if (active) {
                    scrubber.dragValue = Math.max(0, Math.min(1, centroid.position.x / Math.max(1, track.width)))
                    scrubber.dragging = true
                } else {
                    scrubber.dragging = false
                    scrubber.go(reader.targetForFraction(scrubber.dragValue))
                }
            }
            onCentroidChanged: if (active) scrubber.dragValue = Math.max(0, Math.min(1, centroid.position.x / Math.max(1, track.width)))
        }
        TapHandler {
            onTapped: (point) => scrubber.go(reader.targetForFraction(Math.max(0, Math.min(1, point.position.x / Math.max(1, track.width)))))
        }
    }
}
