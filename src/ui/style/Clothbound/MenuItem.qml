import QtQuick
import QtQuick.Shapes
import QtQuick.Templates as T

// A menu row: a check when checked, the text, a submenu chevron.
T.MenuItem {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)
    padding: 4
    leftPadding: 28
    rightPadding: arrow ? 26 : 10
    hoverEnabled: true
    font.pixelSize: 13
    opacity: enabled ? 1 : 0.45

    // "Contents\tT": the part after a tab is the item's key, set apart.
    readonly property string label: text.split("\t")[0]
    readonly property string key: text.indexOf("\t") >= 0 ? text.split("\t")[1] : ""

    contentItem: Item {
        implicitWidth: labelText.implicitWidth + (keyText.visible ? keyText.implicitWidth + 24 : 0)
        implicitHeight: labelText.implicitHeight
        Text {
            id: labelText
            anchors.left: parent.left
            anchors.right: keyText.visible ? keyText.left : parent.right
            anchors.rightMargin: keyText.visible ? 16 : 0
            anchors.verticalCenter: parent.verticalCenter
            text: control.label
            font: control.font
            color: control.highlighted ? control.palette.highlightedText : control.palette.windowText
            elide: Text.ElideRight
        }
        Text {
            id: keyText
            visible: control.key !== ""
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            text: control.key
            font.pixelSize: 12
            font.family: control.font.family
            color: control.highlighted ? control.palette.highlightedText : control.palette.mid
        }
    }

    indicator: Shape {
        visible: control.checked
        x: 9
        y: (control.height - 12) / 2
        width: 12
        height: 12
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: 1.8
            strokeColor: control.highlighted ? control.palette.highlightedText : control.palette.link
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            startX: 1; startY: 6.5
            PathLine { x: 4.5; y: 10 }
            PathLine { x: 11; y: 2.5 }
        }
    }

    arrow: Text {
        x: control.mirrored ? control.leftPadding / 3 : control.width - width - 10
        y: (control.height - height) / 2
        visible: control.subMenu
        text: control.mirrored ? "‹" : "›"
        font.pixelSize: 15
        color: control.highlighted ? control.palette.highlightedText : control.palette.mid
    }

    background: Rectangle {
        implicitWidth: 180
        implicitHeight: 28
        radius: Math.min(6, theme.radius)
        color: control.highlighted ? control.palette.highlight : "transparent"
    }
}
