import QtQuick
import QtQuick.Effects
import QtQuick.Window

import "../components/Ui.js" as Ui

// A book's cover at its own shape, standing on the bottom edge of its box
// as books stand on a shelf. Asymmetric book corners (3 px spine, 5 px edge;
// strictly 0 in sharp mode) and a subtle 3 px spine crease.
Item {
    id: cover

    property string source
    property string title
    property string author
    property int radius: Ui.radius(5, theme.radius)
    readonly property int spineRadius: radius > 0 ? Math.min(3, radius) : 0

    readonly property bool loaded: image.status === Image.Ready
    // Width over height of what's shown; generated covers are 2:3.
    readonly property real aspect: loaded && image.implicitHeight > 0
                                   ? Math.max(0.45, Math.min(1.2, image.implicitWidth / image.implicitHeight)) : 2 / 3
    readonly property rect box: {
        const w = Math.min(width, height * aspect)
        const h = w / aspect
        return Qt.rect((width - w) / 2, height - h, w, h)
    }

    Item {
        id: frame
        x: cover.box.x
        y: cover.box.y
        width: cover.box.width
        height: cover.box.height
        layer.enabled: cover.radius > 0
        layer.effect: MultiEffect {
            maskEnabled: true
            maskSource: mask
            maskThresholdMin: 0.5
            maskSpreadAtMin: 1.0
        }

        GeneratedCover {
            anchors.fill: parent
            visible: !cover.loaded
            title: cover.title
            author: cover.author
        }

        Image {
            id: image
            anchors.fill: parent
            source: cover.source
            asynchronous: true
            fillMode: Image.Stretch
            smooth: true
            mipmap: true
            // Bucket decode height to 128px steps so continuous window resizes
            // don't re-decode every visible cover JPEG on every frame.
            sourceSize.height: Math.max(128, Math.min(960, Math.ceil((cover.height * Screen.devicePixelRatio) / 128) * 128))
            visible: cover.loaded
            opacity: cover.loaded ? 1 : 0
            Behavior on opacity { NumberAnimation { duration: 160 } }
        }

        // Subtle left-edge spine crease on every cover (inset 3px 0 0 rgba(0,0,0,0.22)).
        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Math.max(2, Math.min(3, parent.width * 0.04))
            color: Qt.rgba(0, 0, 0, 0.22)
        }
    }

    // Offscreen mask giving 3 px left (spine) and 5 px right (fore-edge) corners.
    Item {
        id: mask
        width: frame.width
        height: frame.height
        visible: false
        layer.enabled: true
        Rectangle {
            anchors.fill: parent
            radius: cover.radius
        }
        Rectangle {
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Math.min(parent.width / 2, Math.max(6, cover.radius + 2))
            radius: cover.spineRadius
        }
        Rectangle {
            x: cover.spineRadius
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Math.min(parent.width / 2, Math.max(0, cover.radius))
        }
    }
}
