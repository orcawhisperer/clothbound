import QtQuick

import "Ui.js" as Ui

// A placeholder shape while something loads, with a slow shimmer.
Rectangle {
    id: skeleton

    property bool shimmer: true

    radius: Math.min(6, theme.radius)
    color: Ui.tint(palette.windowText, 0.06)
    clip: true

    Rectangle {
        visible: skeleton.shimmer
        width: skeleton.width * 0.6
        height: skeleton.height
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: "transparent" }
            GradientStop { position: 0.5; color: Ui.tint(skeleton.palette.windowText, 0.06) }
            GradientStop { position: 1; color: "transparent" }
        }
        XAnimator on x {
            running: skeleton.shimmer && skeleton.visible
            from: -skeleton.width * 0.6
            to: skeleton.width
            duration: 1400
            loops: Animation.Infinite
        }
    }
}
