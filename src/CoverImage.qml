import QtQuick
import QtQuick.Effects
import QtQuick.Window

// A book's cover at its own shape, standing on the bottom edge of its box
// as books stand on a shelf. Without a cover image, a generated one.
Item {
    id: cover

    property string source
    property string title
    property string author
    property int radius: Math.min(4, theme.radius)

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
    }

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
    }
}
