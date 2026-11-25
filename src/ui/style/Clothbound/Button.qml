import QtQuick
import QtQuick.Templates as T

// A push button: the accent fill when `highlighted` (the thing to do),
// a quiet fill otherwise, nothing at all when `flat`.
T.Button {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             implicitContentHeight + topPadding + bottomPadding)
    padding: 6
    horizontalPadding: 14
    spacing: 6
    hoverEnabled: true
    font.pixelSize: 13
    font.weight: highlighted ? Font.DemiBold : Font.Medium
    opacity: enabled ? 1 : 0.45

    icon.width: 16
    icon.height: 16
    icon.color: contentColor
    readonly property color contentColor: highlighted ? palette.highlightedText : palette.buttonText

    contentItem: Text {
        text: control.text
        textFormat: Text.PlainText
        font: control.font
        color: control.contentColor
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        implicitWidth: 64
        implicitHeight: 32
        radius: Math.min(7, theme.radius)
        color: control.highlighted
               ? (control.down ? Qt.darker(control.palette.highlight, 1.12)
                  : control.hovered ? Qt.lighter(control.palette.highlight, 1.06) : control.palette.highlight)
               : control.flat
                 ? (control.down ? control.palette.button : control.hovered ? control.palette.alternateBase : "transparent")
                 : (control.down ? Qt.darker(control.palette.alternateBase, 1.0) : control.hovered ? control.palette.alternateBase : control.palette.button)
        border.width: control.visualFocus ? 2 : 0
        border.color: control.palette.highlight
        Behavior on color { ColorAnimation { duration: 100 } }
    }
}
