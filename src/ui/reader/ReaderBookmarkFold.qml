import QtQuick
import QtQuick.Controls
import QtQuick.Shapes

import "../components/Ui.js" as Ui

// GPU-accelerated 44×44 tactile paper bookmark dog-ear anchored in the
// top-right corner of the reading stage (zero software Canvas repaints).
Item {
    id: pageFold

    property var look
    property bool bookmarked: false

    signal toggled()

    width: 44
    height: 44

    property real fold: bookmarked ? 1 : (earHover.hovered ? 0.55 : 0)
    Behavior on fold {
        enabled: !theme.reducedMotion
        NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
    }

    readonly property real s: width * fold
    readonly property real x0: width - s
    readonly property color pageColor: look ? look.bg : "#FAF6EE"
    readonly property color inkColor: look ? look.fg : "#26221D"
    readonly property bool darkPage: !!(look && look.dark)
    readonly property color flapColor: bookmarked
        ? ((look && look.fold) ? look.fold : Ui.mix(pageColor, inkColor, 0.12))
        : Ui.mix(pageColor, inkColor, 0.08)

    Accessible.role: Accessible.Button
    Accessible.name: bookmarked ? qsTr("Remove bookmark") : qsTr("Bookmark this page")

    HoverHandler { id: earHover; cursorShape: Qt.PointingHandCursor }
    TapHandler { onTapped: pageFold.toggled() }

    ToolTip.visible: earHover.hovered
    ToolTip.text: bookmarked ? qsTr("Remove bookmark (B)") : qsTr("Bookmark this page (B)")
    ToolTip.delay: 500

    Shape {
        anchors.fill: parent
        visible: pageFold.fold > 0.01
        preferredRendererType: Shape.CurveRenderer

        // Soft feathered outer drop shadow under the folded paper triangle.
        ShapePath {
            strokeWidth: 0
            strokeColor: "transparent"
            fillColor: Qt.rgba(0, 0, 0, pageFold.darkPage ? 0.14 : 0.045)
            PathMove { x: pageFold.x0 - 3; y: 3.5 }
            PathLine { x: pageFold.x0 - 3; y: pageFold.s + 3.5 }
            PathLine { x: pageFold.width - 3; y: pageFold.s + 3.5 }
            PathLine { x: pageFold.x0 - 3; y: 3.5 }
        }

        // Crisp core drop shadow hugging the fold edge.
        ShapePath {
            strokeWidth: 0
            strokeColor: "transparent"
            fillColor: Qt.rgba(0, 0, 0, pageFold.darkPage ? 0.22 : 0.085)
            PathMove { x: pageFold.x0 - 1.5; y: 1.5 }
            PathLine { x: pageFold.x0 - 1.5; y: pageFold.s + 1.5 }
            PathLine { x: pageFold.width - 1.5; y: pageFold.s + 1.5 }
            PathLine { x: pageFold.x0 - 1.5; y: 1.5 }
        }

        // Cut-away top-right corner matching the reading paper background.
        ShapePath {
            strokeWidth: 0
            strokeColor: "transparent"
            fillColor: pageFold.pageColor
            PathMove { x: pageFold.x0; y: 0 }
            PathLine { x: pageFold.width; y: 0 }
            PathLine { x: pageFold.width; y: pageFold.s }
            PathLine { x: pageFold.x0; y: 0 }
        }

        // Folded paper flap with subtle directional sheen and hairline crease.
        ShapePath {
            strokeWidth: 0.75
            strokeColor: Qt.rgba(0, 0, 0, pageFold.darkPage ? 0.24 : 0.08)
            joinStyle: ShapePath.MiterJoin
            fillGradient: LinearGradient {
                x1: pageFold.x0
                y1: pageFold.s
                x2: pageFold.x0 + pageFold.s * 0.5
                y2: pageFold.s * 0.5
                GradientStop { position: 0.0; color: pageFold.flapColor }
                GradientStop { position: 1.0; color: Ui.mix(pageFold.flapColor, pageFold.pageColor, 0.22) }
            }
            PathMove { x: pageFold.x0; y: 0 }
            PathLine { x: pageFold.x0; y: pageFold.s }
            PathLine { x: pageFold.width; y: pageFold.s }
            PathLine { x: pageFold.x0; y: 0 }
        }
    }
}
