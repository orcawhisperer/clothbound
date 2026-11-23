import QtQuick
import QtQuick.Shapes

// One of Dogear's line icons (Icons.js), drawn in any color and size.
Item {
    id: glyph

    property string path
    property color color: palette.windowText
    property real stroke: 1.8

    implicitWidth: 20
    implicitHeight: 20

    Shape {
        anchors.centerIn: parent
        width: 24
        height: 24
        scale: Math.min(glyph.width, glyph.height) / 24
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeColor: glyph.color
            strokeWidth: glyph.stroke
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            PathSvg { path: glyph.path }
        }
    }
}
