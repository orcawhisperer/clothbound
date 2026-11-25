import QtQuick
import QtQuick.Shapes
import QtQuick.Templates as T

T.CheckBox {
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
        radius: Math.min(4, theme.radius)
        color: control.checkState !== Qt.Unchecked ? control.palette.highlight : control.palette.base
        border.width: control.checkState !== Qt.Unchecked ? 0 : 1
        border.color: control.visualFocus ? control.palette.highlight : control.palette.dark
        Shape {
            anchors.fill: parent
            visible: control.checkState === Qt.Checked
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeWidth: 1.8
                strokeColor: control.palette.highlightedText
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                joinStyle: ShapePath.RoundJoin
                startX: 4; startY: 8.5
                PathLine { x: 7; y: 11.5 }
                PathLine { x: 12.5; y: 5 }
            }
        }
        Rectangle {
            visible: control.checkState === Qt.PartiallyChecked
            anchors.centerIn: parent
            width: 8; height: 2; radius: 1
            color: control.palette.highlightedText
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
