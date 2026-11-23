import QtQuick
import QtQuick.Templates as T

T.MenuSeparator {
    id: control
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    padding: 4
    horizontalPadding: 8
    contentItem: Rectangle {
        implicitWidth: 180
        implicitHeight: 1
        color: control.palette.dark
    }
}
