import QtQuick
import QtQuick.Templates as T

// A 38×22 pill switch with an 18×18 white knob, filled with the accent when on.
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
        implicitWidth: 38
        implicitHeight: 22
        x: control.text ? (control.mirrored ? control.width - width - control.rightPadding : control.leftPadding)
                        : control.leftPadding + (control.availableWidth - width) / 2
        y: control.topPadding + (control.availableHeight - height) / 2
        radius: height / 2
        color: control.checked
               ? control.palette.highlight
               : Qt.rgba(control.palette.windowText.r, control.palette.windowText.g, control.palette.windowText.b, 0.16)
        Behavior on color {
            enabled: !theme.reducedMotion
            ColorAnimation { duration: theme.reducedMotion ? 0 : 140 }
        }

        // 2 px offset focus ring when reached via keyboard Tab.
        Rectangle {
            anchors.fill: parent
            anchors.margins: -3
            visible: control.visualFocus
            radius: height / 2
            color: "transparent"
            border.width: 2
            border.color: control.palette.highlight
        }

        Item {
            x: Math.max(2, Math.min(parent.width - width - 2,
                                    control.visualPosition * parent.width - width / 2))
            y: 2
            width: 18
            height: 18
            Behavior on x {
                enabled: !control.down && !theme.reducedMotion
                NumberAnimation { duration: 180; easing.type: Easing.OutCubic }
            }

            Rectangle {
                y: 1
                width: parent.width
                height: parent.height
                radius: height / 2
                color: Qt.rgba(0, 0, 0, 0.28)
            }
            Rectangle {
                anchors.fill: parent
                radius: height / 2
                color: "#ffffff"
            }
        }
    }

    contentItem: Text {
        leftPadding: control.indicator && !control.mirrored ? control.indicator.width + control.spacing : 0
        rightPadding: control.indicator && control.mirrored ? control.indicator.width + control.spacing : 0
        text: control.text
        textFormat: Text.PlainText
        font: control.font
        color: control.palette.windowText
        elide: Text.ElideRight
        verticalAlignment: Text.AlignVCenter
    }
}
