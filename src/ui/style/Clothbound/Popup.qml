import QtQuick
import QtQuick.Templates as T

// A floating surface: overlay colour, hairline edge, soft shadow.
T.Popup {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    padding: 8

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 120; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: 140; easing.type: Easing.OutCubic }
    }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 90 } }

    background: Rectangle {
        radius: Math.min(10, theme.radius)
        color: control.palette.light
        border.width: 1
        border.color: control.palette.dark
        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.topMargin: 6
            anchors.margins: -2
            radius: parent.radius > 0 ? parent.radius + 2 : 0
            color: control.palette.shadow
            opacity: 0.35
        }
    }

    T.Overlay.modal: Rectangle { color: theme.scrim }
    T.Overlay.modeless: Rectangle { color: "transparent" }
}
