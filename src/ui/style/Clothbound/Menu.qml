import QtQuick
import QtQuick.Templates as T

T.Menu {
    id: control

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset,
                            contentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset,
                             contentHeight + topPadding + bottomPadding)
    margins: 0
    padding: 5
    overlap: 1
    font.pixelSize: 13

    delegate: MenuItem {}

    enter: Transition {
        NumberAnimation { property: "opacity"; from: 0; to: 1; duration: 110; easing.type: Easing.OutCubic }
        NumberAnimation { property: "scale"; from: 0.97; to: 1; duration: 130; easing.type: Easing.OutCubic }
    }
    exit: Transition { NumberAnimation { property: "opacity"; from: 1; to: 0; duration: 80 } }

    contentItem: ListView {
        implicitHeight: contentHeight
        model: control.contentModel
        interactive: Window.window ? contentHeight + control.topPadding + control.bottomPadding > control.height : false
        clip: true
        currentIndex: control.currentIndex
        T.ScrollIndicator.vertical: ScrollIndicator {}
    }

    background: Rectangle {
        implicitWidth: 200
        radius: Math.min(10, theme.radius)
        color: control.palette.light
        border.width: 1
        border.color: control.palette.dark
        Rectangle {
            z: -1
            anchors.fill: parent
            anchors.topMargin: 6
            anchors.margins: -2
            radius: parent.radius + 2
            color: control.palette.shadow
            opacity: 0.35
        }
    }

    T.Overlay.modal: Rectangle { color: theme.scrim }
    T.Overlay.modeless: Rectangle { color: "transparent" }
}
