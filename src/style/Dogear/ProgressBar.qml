import QtQuick
import QtQuick.Templates as T

// A slim track filling with the accent; a gliding segment when indeterminate.
T.ProgressBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)

    contentItem: Item {
        implicitWidth: 160
        implicitHeight: 4
        clip: true
        Rectangle {
            visible: !control.indeterminate
            width: control.visualPosition * parent.width
            height: parent.height
            radius: 2
            color: control.palette.highlight
            Behavior on width { NumberAnimation { duration: 160 } }
        }
        Rectangle {
            id: glide
            visible: control.indeterminate
            width: parent.width * 0.3
            height: parent.height
            radius: 2
            color: control.palette.highlight
            XAnimator on x {
                running: control.indeterminate && control.visible
                from: -glide.width
                to: glide.parent.width
                duration: 1100
                loops: Animation.Infinite
                easing.type: Easing.InOutQuad
            }
        }
    }

    background: Rectangle {
        implicitWidth: 160
        implicitHeight: 4
        y: (control.height - height) / 2
        height: 4
        radius: 2
        color: control.palette.dark
    }
}
