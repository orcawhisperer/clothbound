import QtQuick
import QtQuick.Window

import "../components/Ui.js" as Ui

// A book's cover at its own shape, standing on the bottom edge of its box
// as books stand on a shelf. Asymmetric book corners (3 px spine, 5 px edge;
// strictly 0 in sharp mode) and a subtle 3 px spine crease are baked once off-
// thread by CoverProvider (image://cover/…) — zero GPU FBO layers or shaders.
Item {
    id: cover

    property string source
    property string title
    property string author
    property int radius: Ui.radius(5, theme.radius)
    readonly property int spineRadius: radius > 0 ? Math.min(3, radius) : 0

    readonly property bool loaded: image.status === Image.Ready
    // Bucket decode size in 128px steps (capped at 640px) so multi-MB covers never
    // decode at full resolution for small library tiles and resizes avoid thrashing.
    readonly property int decodeLimit: Math.max(128, Math.min(640, Math.ceil((Math.max(cover.width, cover.height) * (Screen.devicePixelRatio || 1)) / 128) * 128))
    // Bucket logical box width in 16px steps from the cover's layout size (never
    // from cover.box, which depends on loaded/aspect and would form a binding loop).
    readonly property real nominalWidth: cover.height > 0 ? Math.min(cover.width, cover.height * 0.75) : cover.width
    readonly property int widthBucket: Math.max(32, Math.round(Math.max(1, nominalWidth) / 16) * 16)
    readonly property bool localCover: cover.source !== ""
                                       && (cover.source.startsWith("file://") || cover.source.startsWith("/"))
    readonly property bool useCoverProvider: typeof hasCoverProvider !== "undefined" && hasCoverProvider && localCover
    readonly property string effectiveSource: {
        if (!cover.visible || cover.width <= 0 || cover.height <= 0 || cover.source === "")
            return ""
        if (useCoverProvider)
            return "image://cover/" + encodeURIComponent(cover.source)
                   + "?r=" + cover.radius + "&sr=" + cover.spineRadius + "&bw=" + cover.widthBucket
        return cover.source
    }
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

        GeneratedCover {
            anchors.fill: parent
            visible: !cover.loaded
            radius: cover.radius
            title: cover.title
            author: cover.author
        }

        Image {
            id: image
            anchors.fill: parent
            source: cover.effectiveSource
            asynchronous: true
            cache: true
            fillMode: Image.Stretch
            smooth: true
            mipmap: true
            sourceSize.width: cover.decodeLimit
            sourceSize.height: cover.decodeLimit
            visible: cover.loaded
            opacity: cover.loaded ? 1 : 0
            Behavior on opacity {
                enabled: !theme.reducedMotion
                NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
            }
        }

        // Spine crease overlay only when not already baked by CoverProvider
        // (generated covers or remote fallback).
        Rectangle {
            visible: !cover.loaded || !cover.useCoverProvider
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            width: Math.max(2, Math.min(3, parent.width * 0.04))
            radius: cover.spineRadius
            color: Qt.rgba(0, 0, 0, 0.22)
        }
    }
}
