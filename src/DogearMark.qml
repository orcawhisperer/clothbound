import QtQuick
import QtQuick.Shapes

// Dogear's folded-page geometric mark: compact (20–22 px) for the sidebar
// brand header, or `wireframe: true` (72 px) with delicate text lines for the
// empty-library hero (01_library.html, 10_j1_empty_library.html).
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

        // Outer page outline with clipped top-right fold.
        ShapePath {
            strokeColor: mark.strokeColor
            strokeWidth: mark.wireframe ? 0.9 : 1.6
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: "M5 3 H15 L19 7 V21 H5 Z" }
        }

        // Folded top-right dog-ear corner.
        ShapePath {
            strokeColor: mark.wireframe ? mark.color : mark.strokeColor
            strokeWidth: mark.wireframe ? 0.9 : 1.6
            fillColor: mark.wireframe ? theme.accentSoft : mark.color
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: "M15 3 V7 H19 Z" }
        }

        // Subtle horizontal page lines in wireframe hero mode.
        ShapePath {
            strokeColor: mark.wireframe ? mark.strokeColor : "transparent"
            strokeWidth: mark.wireframe ? 0.9 : 0
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            PathSvg { path: "M8 11 H16 M8 14 H16 M8 17 H13" }
        }
    }
}
