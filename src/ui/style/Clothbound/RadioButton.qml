import QtQuick
import QtQuick.Templates as T

T.RadioButton {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)
    padding: 4
    spacing: 8
    hoverEnabled: true
    font.pixelSize: 13
    opacity: enabled ? 1 : 0.45

    indicator: Rectangle {
        implicitWidth: 16
        implicitHeight: 16
        x: control.text ? (control.mirrored ? control.width - width - control.rightPadding : control.leftPadding)
                        : control.leftPadding + (control.availableWidth - width) / 2
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: 8
        color: control.checked ? control.palette.highlight : control.palette.base
        border.width: control.checked ? 0 : 1
        border.color: control.visualFocus ? control.palette.highlight : control.palette.dark
        Rectangle {
            anchors.centerIn: parent
            width: 6; height: 6; radius: 3
            color: control.palette.highlightedText
            visible: control.checked
        }
    }

    contentItem: Text {
        leftPadding: control.indicator && !control.mirrored ? control.indicator.width + control.spacing : 0
        rightPadding: control.indicator && control.mirrored ? control.indicator.width + control.spacing : 0
        text: control.text
        font: control.font
        color: control.palette.windowText
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }
}
