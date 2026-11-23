import QtQuick
import QtQuick.Templates as T

// A small pill switch, the accent when on.
T.Switch {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)
    padding: 4
    spacing: 10
    hoverEnabled: true
    font.pixelSize: 13
    opacity: enabled ? 1 : 0.45

    indicator: Rectangle {
        implicitWidth: 32
        implicitHeight: 18
        x: control.text ? (control.mirrored ? control.width - width - control.rightPadding : control.leftPadding)
                        : control.leftPadding + (control.availableWidth - width) / 2
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: height / 2
        color: control.checked ? control.palette.highlight : control.palette.dark
        border.width: control.visualFocus ? 2 : 0
        border.color: control.palette.link
        Behavior on color { ColorAnimation { duration: 140 } }

        Rectangle {
            x: Math.max(2, Math.min(parent.width - width - 2,
                                    control.visualPosition * parent.width - width / 2))
            y: 2
            width: 14
            height: 14
            radius: 7
            color: "#ffffff"
            Behavior on x {
                enabled: !control.down
                NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
            }
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
