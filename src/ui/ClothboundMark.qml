import QtQuick
import QtQuick.Shapes

// Clothbound's hardcover-and-ribbon geometric mark: compact (20–22 px) for the
// sidebar brand header, or `wireframe: true` (72 px) with subtle spine and
// title-plate rules for the empty-library hero.
Item {
    id: mark

    property bool wireframe: false
    property real size: 22
    property color color: theme.accent
    property color strokeColor: wireframe ? theme.ink4 : theme.accentInk

    implicitWidth: size
    implicitHeight: size

    Shape {
        anchors.centerIn: parent
        width: 24
        height: 24
        scale: Math.min(mark.width, mark.height) / 24
        preferredRendererType: Shape.CurveRenderer

        // Outer hardcover book outline with rounded spine corners.
        ShapePath {
            strokeColor: mark.strokeColor
            strokeWidth: mark.wireframe ? 0.9 : 1.6
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: "M5 3.5 C5 2.7 5.7 2 6.5 2 H18.5 V19.5 H6.5 C5.7 19.5 5 20.2 5 21 Z" }
        }

        // Bottom page-block curve closing the hardcover fore-edge.
        ShapePath {
            strokeColor: mark.strokeColor
            strokeWidth: mark.wireframe ? 0.9 : 1.6
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: "M5 21 C5 21.8 5.7 22.2 6.5 22.2 H18.5 V19.5" }
        }

        // Bound spine hinge groove.
        ShapePath {
            strokeColor: mark.strokeColor
            strokeWidth: mark.wireframe ? 0.85 : 1.35
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathSvg { path: "M8.5 2.5 V19" }
        }

        // Accent silk bookmark ribbon descending from the top of the cover.
        ShapePath {
            strokeColor: mark.wireframe ? mark.color : mark.strokeColor
            strokeWidth: mark.wireframe ? 0.9 : 1.4
            fillColor: mark.wireframe ? theme.accentSoft : mark.color
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: "M12.5 2 V11 L14.5 9.2 L16.5 11 V2 Z" }
        }

        // Subtle cover title-plate lines in wireframe hero mode.
        ShapePath {
            strokeColor: mark.wireframe ? mark.strokeColor : "transparent"
            strokeWidth: mark.wireframe ? 0.85 : 0
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathSvg { path: "M10.5 14 H16.5 M10.5 16.5 H15" }
        }
    }
}
