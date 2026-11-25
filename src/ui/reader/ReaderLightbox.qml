import QtQuick
import QtQuick.Controls
import "../components"

import "../components/Icons.js" as Icons
import "../components/Ui.js" as Ui

// Inline EPUB image/SVG lightbox with async loading indicator, zoom, wheel zoom,
// drag-to-pan, keyboard shortcuts (+/-/0/Esc), and optional caption pill.
FocusScope {
    id: lightboxInner

    property bool open: false
    property string source: ""
    property string caption: ""
    property real zoom: 1.0
    Behavior on zoom {
        enabled: !theme.reducedMotion && lightboxInner.open
        NumberAnimation { duration: 150; easing.type: Easing.OutCubic }
    }
    property real panX: 0
    property real panY: 0
    property real reveal: open ? 1 : 0
    Behavior on reveal {
        NumberAnimation {
            duration: theme.reducedMotion ? 90 : (lightboxInner.open ? 220 : 160)
            easing.type: lightboxInner.open ? Easing.OutCubic : Easing.OutQuad
        }
    }

    signal closed()

    visible: reveal > 0.01
    opacity: reveal
    Accessible.role: Accessible.Dialog
    Accessible.name: caption !== "" ? caption : qsTr("Image viewer")

    function openImage(src, cap) {
        if (!src || src === "")
            return
        source = src
        caption = cap || ""
        zoom = 1.0
        panX = 0
        panY = 0
        open = true
        forceActiveFocus()
    }
    function close() {
        if (!open)
            return
        open = false
        zoom = 1.0
        panX = 0
        panY = 0
        closed()
    }
    function setZoom(next) {
        zoom = Math.max(1.0, Math.min(5.0, Math.round(next * 100) / 100))
        if (zoom <= 1.01) {
            panX = 0
            panY = 0
        } else {
            clampPan()
        }
    }
    function clampPan() {
        const maxX = Math.max(0, (lightboxImage.paintedWidth * zoom - width + 96) / 2)
        const maxY = Math.max(0, (lightboxImage.paintedHeight * zoom - height + 140) / 2)
        panX = Math.max(-maxX, Math.min(maxX, panX))
        panY = Math.max(-maxY, Math.min(maxY, panY))
    }

    Keys.onEscapePressed: close()
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Plus || event.key === Qt.Key_Equal) { setZoom(zoom * 1.25); event.accepted = true }
        else if (event.key === Qt.Key_Minus) { setZoom(zoom / 1.25); event.accepted = true }
        else if (event.key === Qt.Key_0) { setZoom(1.0); event.accepted = true }
    }

    Rectangle {
        anchors.fill: parent
        color: Ui.tint("black", 0.86)
    }

    MouseArea {
        id: lightboxPan
        anchors.fill: parent
        acceptedButtons: Qt.LeftButton
        cursorShape: lightboxInner.zoom > 1.01 ? (pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor) : Qt.ArrowCursor
        property bool dragged: false
        property real startX: 0
        property real startY: 0
        property real startPanX: 0
        property real startPanY: 0
        onPressed: (event) => {
            dragged = false
            startX = event.x
            startY = event.y
            startPanX = lightboxInner.panX
            startPanY = lightboxInner.panY
        }
        onPositionChanged: (event) => {
            if (!pressed || lightboxInner.zoom <= 1.01)
                return
            const dx = event.x - startX, dy = event.y - startY
            if (!dragged && Math.abs(dx) + Math.abs(dy) < 4)
                return
            dragged = true
            lightboxInner.panX = startPanX + dx
            lightboxInner.panY = startPanY + dy
            lightboxInner.clampPan()
        }
        onDoubleClicked: lightboxInner.setZoom(lightboxInner.zoom > 1.2 ? 1.0 : 2.0)
        onClicked: (event) => {
            if (dragged)
                return
            const hitW = lightboxImage.paintedWidth * lightboxInner.zoom
            const hitH = lightboxImage.paintedHeight * lightboxInner.zoom
            const cx = lightboxInner.width / 2 + lightboxInner.panX
            const cy = lightboxInner.height / 2 + lightboxInner.panY
            const onImage = event.x >= cx - hitW / 2 && event.x <= cx + hitW / 2
                            && event.y >= cy - hitH / 2 && event.y <= cy + hitH / 2
            if (!onImage)
                lightboxInner.close()
        }
        onWheel: (wheel) => {
            wheel.accepted = true
            const factor = wheel.angleDelta.y > 0 ? 1.15 : (1 / 1.15)
            lightboxInner.setZoom(lightboxInner.zoom * factor)
        }
    }

    BusyIndicator {
        anchors.centerIn: parent
        visible: lightboxInner.open && lightboxImage.status === Image.Loading
        running: visible
        implicitWidth: 30
        implicitHeight: 30
    }

    Image {
        id: lightboxImage
        anchors.centerIn: parent
        anchors.horizontalCenterOffset: lightboxInner.panX
        anchors.verticalCenterOffset: lightboxInner.panY
        width: Math.max(120, lightboxInner.width - 96)
        height: Math.max(120, lightboxInner.height - (lightboxInner.caption !== "" ? 156 : 112))
        source: lightboxInner.visible ? lightboxInner.source : ""
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        cache: false
        smooth: true
        mipmap: true
        scale: lightboxInner.zoom * (theme.reducedMotion ? 1.0 : (0.94 + 0.06 * lightboxInner.reveal))
    }

    // Top-right floating zoom & close controls.
    Rectangle {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.topMargin: 20
        anchors.rightMargin: 20
        width: lightboxControls.implicitWidth + 14
        height: 40
        radius: Ui.radius(20, theme.radius)
        color: Ui.tint("#1c1c1e", 0.9)
        border.color: Ui.tint("white", 0.16)

        Row {
            id: lightboxControls
            anchors.centerIn: parent
            spacing: 4
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                glyph: Icons.zoomOut
                tone: "white"
                enabled: lightboxInner.zoom > 1.01
                tip: qsTr("Zoom out (−)")
                onClicked: lightboxInner.setZoom(lightboxInner.zoom / 1.25)
            }
            AbstractButton {
                id: resetZoomBtn
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: 48
                implicitHeight: 30
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.name: qsTr("Reset zoom")
                onClicked: lightboxInner.setZoom(1.0)
                background: Rectangle {
                    radius: Ui.radius(6, theme.radius)
                    color: resetZoomBtn.hovered ? Ui.tint("white", 0.12) : "transparent"
                }
                contentItem: Label {
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: Math.round(lightboxInner.zoom * 100).toLocaleString(Qt.locale(), 'f', 0) + "%"
                    color: "white"
                    font.pixelSize: 12
                    font.features: ({ "tnum": 1 })
                }
                ToolTip.visible: hovered
                ToolTip.delay: 600
                ToolTip.text: qsTr("Reset zoom (0)")
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                glyph: Icons.zoomIn
                tone: "white"
                enabled: lightboxInner.zoom < 4.99
                tip: qsTr("Zoom in (+)")
                onClicked: lightboxInner.setZoom(lightboxInner.zoom * 1.25)
            }
            Rectangle {
                anchors.verticalCenter: parent.verticalCenter
                width: 1
                height: 18
                color: Ui.tint("white", 0.18)
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                glyph: Icons.close
                tone: "white"
                tip: qsTr("Close (Esc)")
                onClicked: lightboxInner.close()
            }
        }
    }

    // Caption pill shown when the image has alt text or a figcaption.
    Rectangle {
        visible: lightboxInner.caption !== ""
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 24
        anchors.horizontalCenter: parent.horizontalCenter
        width: Math.min(parent.width - 64, captionText.implicitWidth + 32)
        height: captionText.implicitHeight + 18
        radius: Ui.radius(10, theme.radius)
        color: Ui.tint("#1c1c1e", 0.88)
        border.color: Ui.tint("white", 0.14)
        Label {
            id: captionText
            anchors.centerIn: parent
            width: Math.min(implicitWidth, lightboxInner.width - 96)
            text: lightboxInner.caption
            color: "#f2f0eb"
            font.pixelSize: 13
            wrapMode: Text.Wrap
            horizontalAlignment: Text.AlignHCenter
            maximumLineCount: 4
            elide: Text.ElideRight
        }
    }
}
