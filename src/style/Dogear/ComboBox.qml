import QtQuick
import QtQuick.Shapes
import QtQuick.Templates as T

// A select: a quiet button showing the choice, opening a menu-like list.
T.ComboBox {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding,
                             implicitIndicatorHeight + topPadding + bottomPadding)
    leftPadding: 10
    rightPadding: 30
    hoverEnabled: true
    font.pixelSize: 13
    opacity: enabled ? 1 : 0.45

    delegate: ItemDelegate {
        required property var model
        required property int index
        width: ListView.view.width
        text: model[control.textRole]
        highlighted: control.highlightedIndex === index
        font.weight: control.currentIndex === index ? Font.DemiBold : Font.Normal
    }

    indicator: Shape {
        x: control.mirrored ? 10 : control.width - width - 10
        y: (control.height - height) / 2
        width: 10
        height: 6
        preferredRendererType: Shape.CurveRenderer
        ShapePath {
            strokeWidth: 1.6
            strokeColor: control.palette.mid
            fillColor: "transparent"
            capStyle: ShapePath.RoundCap
            joinStyle: ShapePath.RoundJoin
            startX: 1; startY: 1
            PathLine { x: 5; y: 5 }
            PathLine { x: 9; y: 1 }
        }
    }

    contentItem: T.TextField {
        leftPadding: 0
        rightPadding: 0
        text: control.editable ? control.editText : control.displayText
        enabled: control.editable
        autoScroll: control.editable
        readOnly: control.down
        inputMethodHints: control.inputMethodHints
        validator: control.validator
        selectByMouse: control.selectTextByMouse
        font: control.font
        color: control.palette.buttonText
        selectionColor: control.palette.highlight
        selectedTextColor: control.palette.highlightedText
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        implicitWidth: 140
        implicitHeight: 32
        radius: Math.min(7, theme.radius)
        color: control.down || control.hovered ? control.palette.alternateBase : control.palette.button
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? control.palette.highlight : control.palette.midlight
    }

    popup: T.Popup {
        y: control.height + 4
        width: Math.max(control.width, 160)
        height: Math.min(contentItem.implicitHeight + topPadding + bottomPadding, control.Window.height - topMargin - bottomMargin)
        topMargin: 6
        bottomMargin: 6
        padding: 5
        palette: control.palette

        enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 110 } }
        exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 80 } }

        contentItem: ListView {
            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            highlightMoveDuration: 0
            T.ScrollIndicator.vertical: ScrollIndicator {}
        }

        background: Rectangle {
            radius: Math.min(10, theme.radius)
            color: control.palette.light
            border.width: 1
            border.color: control.palette.dark
        }
    }
}
