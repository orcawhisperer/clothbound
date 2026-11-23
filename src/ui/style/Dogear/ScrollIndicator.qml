import QtQuick
import QtQuick.Templates as T

T.ScrollIndicator {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    padding: 2

    contentItem: Rectangle {
        implicitWidth: 4
        implicitHeight: 4
        radius: 2
        color: control.palette.mid
        visible: control.size < 1.0
        opacity: control.active ? 0.45 : 0
        Behavior on opacity { NumberAnimation { duration: 180 } }
    }
}
