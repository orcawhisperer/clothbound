import QtQuick
import QtQuick.Shapes
import QtQuick.Templates as T

// A thin spinning arc, in the accent.
T.BusyIndicator {
    id: control

    implicitWidth: contentItem.implicitWidth + leftPadding + rightPadding
    implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
    padding: 2

    contentItem: Item {
        implicitWidth: 20
        implicitHeight: 20
        opacity: control.running ? 1 : 0
        visible: opacity > 0
        Behavior on opacity { NumberAnimation { duration: 150 } }

        Shape {
            id: arc
            readonly property real size: Math.min(parent.width, parent.height)
            anchors.centerIn: parent
            width: size
            height: size
            preferredRendererType: Shape.CurveRenderer
            ShapePath {
                strokeWidth: Math.max(1.6, arc.size / 11)
                strokeColor: control.palette.highlight
                fillColor: "transparent"
                capStyle: ShapePath.RoundCap
                PathAngleArc {
                    centerX: arc.size / 2
                    centerY: arc.size / 2
                    radiusX: arc.size / 2 - 2
                    radiusY: arc.size / 2 - 2
                    startAngle: 0
                    sweepAngle: 270
                }
            }
            RotationAnimator on rotation {
                running: control.running && control.visible
                from: 0
                to: 360
                duration: 900
                loops: Animation.Infinite
            }
        }
    }
}
