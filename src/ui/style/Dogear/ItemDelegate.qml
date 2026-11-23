import QtQuick
import QtQuick.Templates as T

T.ItemDelegate {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    padding: 6
    horizontalPadding: 10
    hoverEnabled: true
    font.pixelSize: 13

    contentItem: Text {
        text: control.text
        font: control.font
        color: control.highlighted ? control.palette.highlightedText : control.palette.windowText
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        implicitHeight: 28
        radius: Math.min(6, theme.radius)
        color: control.highlighted ? control.palette.highlight
             : control.down ? control.palette.button
             : control.hovered ? control.palette.alternateBase : "transparent"
    }
}
