import QtQuick
import QtQuick.Templates as T

// A thin overlay scrollbar that shows while scrolling or hovered.
T.ScrollBar {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    padding: 2
    visible: policy !== T.ScrollBar.AlwaysOff
    minimumSize: orientation === Qt.Horizontal ? height / width : width / height
    hoverEnabled: true

    contentItem: Rectangle {
        implicitWidth: control.interactive ? (control.hovered || control.pressed ? 8 : 6) : 4
        implicitHeight: control.interactive ? (control.hovered || control.pressed ? 8 : 6) : 4
        radius: width / 2
        color: control.palette.mid
        opacity: control.policy === T.ScrollBar.AlwaysOn || (control.active && control.size < 1.0)
                 ? (control.pressed ? 0.75 : control.hovered ? 0.6 : 0.42) : 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Behavior on implicitWidth { NumberAnimation { duration: 100 } }
    }
}
