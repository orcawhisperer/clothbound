import QtQuick
import QtQuick.Templates as T

// A small, quick label: the overlay colour with a hairline, never a slab.
T.ToolTip {
    id: control

    x: parent ? (parent.width - implicitWidth) / 2 : 0
    y: -implicitHeight - 6
    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)
    margins: 6
    padding: 6
    horizontalPadding: 8
    font.pixelSize: 12
    closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent | T.Popup.CloseOnReleaseOutsideParent

    enter: Transition { NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 90 } }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 90 } }

    contentItem: Text {
        text: control.text
        font: control.font
        wrapMode: Text.Wrap
        color: control.palette.toolTipText
    }

    background: Rectangle {
        radius: Math.min(6, theme.radius)
        color: control.palette.toolTipBase
        border.width: 1
        border.color: control.palette.dark
    }
}
