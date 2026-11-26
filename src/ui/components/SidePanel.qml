import QtCore
import QtQml
import QtQuick
import QtQuick.Controls

import "Icons.js" as Icons
import "Ui.js" as Ui

// The shell of a side panel: slides in from its edge over the page on a
// critically damped curve (~260 ms, fade under reduced motion), with a title,
// close button (Esc), hairline edge, soft shadow, and tactile inner-edge drag
// resize handle (double-click resets to defaultWidth). Content goes inside
// below the header; `headerExtra` sits beside the title.
FocusScope {
    id: panel

    property bool open: false
    property string title
    property string glyph
    property bool leftEdge: false
    property var look: null
    property bool resizable: true
    property real defaultWidth: 384
    property real minWidth: 296
    property real maxWidth: 680
    property string persistKey: ""
    property real preferredWidth: defaultWidth
    readonly property real effectiveMaxWidth: Math.min(maxWidth, parent && parent.width > 0 ? Math.max(minWidth, Math.floor(parent.width * 0.65)) : maxWidth)
    readonly property bool resizing: resizeHandle.active
    property Settings persistedSettings: null
    property alias headerExtra: extra.data
    default property alias content: body.data
    readonly property real headerHeight: 52

    readonly property color surfaceColor: look ? (look.panel || look.popover) : palette.light
    readonly property color edgeColor: look ? look.line : palette.dark
    readonly property color dividerColor: look ? look.line : palette.midlight
    readonly property color accentTone: look ? look.link : palette.link

    signal closed()

    function loadPersistedWidth() {
        if (persistKey === "" || !persistedSettings)
            return
        const raw = persistedSettings.value(persistKey, undefined)
        if (raw === undefined || raw === null || raw === "")
            return
        const saved = Number(raw)
        if (isFinite(saved) && saved > 0)
            preferredWidth = Math.max(minWidth, Math.min(maxWidth, Math.round(saved)))
    }
    function persistWidth(w) {
        if (persistKey === "" || !persistedSettings)
            return
        persistedSettings.setValue(persistKey, Math.round(w))
        persistedSettings.sync()
    }

    function show() { open = true; forceActiveFocus() }
    function close() {
        if (!open)
            return
        resizeHandle.active = false
        resizeHandle.moved = false
        open = false
        closed()
    }

    onPersistKeyChanged: loadPersistedWidth()
    onOpenChanged: {
        if (!open) {
            resizeHandle.active = false
            resizeHandle.moved = false
            if (resetWidthAnim.running) {
                resetWidthAnim.stop()
                preferredWidth = resetWidthAnim.to
            }
        }
    }

    Instantiator {
        active: panel.persistKey !== "" && typeof settingsUrl !== "undefined" && String(settingsUrl) !== ""
        delegate: Settings {
            id: panelSettings
            location: settingsUrl
            category: "ui"
            Component.onCompleted: {
                panel.persistedSettings = panelSettings
                panel.loadPersistedWidth()
            }
            Component.onDestruction: {
                if (panel.persistedSettings === panelSettings)
                    panel.persistedSettings = null
            }
        }
    }

    NumberAnimation {
        id: resetWidthAnim
        target: panel
        property: "preferredWidth"
        duration: theme.reducedMotion ? 0 : 180
        easing.type: Easing.OutCubic
    }

    width: Math.max(minWidth, Math.min(effectiveMaxWidth, Math.round(preferredWidth)))
    property real slideProgress: open ? 1 : 0
    Behavior on slideProgress {
        NumberAnimation {
            duration: theme.reducedMotion ? 110 : (panel.open ? 260 : 210)
            easing.type: panel.open ? Easing.OutQuint : Easing.OutCubic
        }
    }
    readonly property real travel: width + 24
    x: theme.reducedMotion
       ? (leftEdge ? 0 : (parent ? parent.width - width : 0))
       : (leftEdge ? -travel * (1 - slideProgress)
                   : (parent ? parent.width - width + travel * (1 - slideProgress) : 0))
    opacity: theme.reducedMotion ? slideProgress : Math.min(1, slideProgress * 2.5)
    visible: slideProgress > 0.005
    Keys.onEscapePressed: close()

    // A soft shadow toward the page, fading smoothly with drawer travel.
    Rectangle {
        x: panel.leftEdge ? parent.width : -width
        width: 18
        height: parent.height
        opacity: panel.slideProgress
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: panel.leftEdge ? 0 : 1; color: Ui.tint("#000000", theme.light ? 0.08 : 0.28) }
            GradientStop { position: panel.leftEdge ? 1 : 0; color: "transparent" }
        }
    }
    Rectangle {
        anchors.fill: parent
        color: panel.surfaceColor
        Rectangle {
            x: panel.leftEdge ? parent.width - 1 : 0
            width: 1
            height: parent.height
            color: panel.edgeColor
        }
    }
    // The panel keeps its clicks and the wheel: nothing falls through to the page.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }

    // Subtle 8 px interactive resize sash along the inner edge; invisible at rest,
    // softly illuminating on hover/drag, double-click resets to defaultWidth.
    MouseArea {
        id: resizeHandle
        property bool active: false
        property bool moved: false
        property real startParentX: 0
        property real startWidth: panel.defaultWidth

        x: panel.leftEdge ? parent.width - 4 : -4
        y: 0
        width: 8
        height: parent.height
        z: 20
        visible: panel.resizable
        enabled: panel.resizable && panel.open
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        cursorShape: Qt.SizeHorCursor

        onPressed: (mouse) => {
            if (!panel.parent)
                return
            resetWidthAnim.stop()
            const pt = mapToItem(panel.parent, mouse.x, mouse.y)
            startParentX = pt.x
            startWidth = panel.width
            moved = false
            active = true
        }
        onPositionChanged: (mouse) => {
            if (!active || !pressed || !panel.parent)
                return
            const pt = mapToItem(panel.parent, mouse.x, mouse.y)
            const delta = panel.leftEdge ? (pt.x - startParentX) : (startParentX - pt.x)
            if (!moved && Math.abs(delta) < 1)
                return
            moved = true
            const clamped = Math.max(panel.minWidth, Math.min(panel.effectiveMaxWidth, Math.round(startWidth + delta)))
            panel.preferredWidth = clamped
        }
        onReleased: {
            if (!active)
                return
            const didMove = moved
            active = false
            moved = false
            if (didMove)
                panel.persistWidth(panel.width)
        }
        onCanceled: {
            if (!active)
                return
            const didMove = moved
            active = false
            moved = false
            if (didMove)
                panel.persistWidth(panel.width)
        }
        onDoubleClicked: {
            active = false
            moved = false
            const target = Math.max(panel.minWidth, Math.min(panel.effectiveMaxWidth, panel.defaultWidth))
            panel.persistWidth(target)
            if (theme.reducedMotion) {
                panel.preferredWidth = target
            } else {
                resetWidthAnim.stop()
                resetWidthAnim.from = panel.width
                resetWidthAnim.to = target
                resetWidthAnim.start()
            }
        }

        Rectangle {
            x: 3
            y: 0
            width: 2
            height: parent.height
            color: panel.accentTone
            opacity: resizeHandle.active ? 0.8 : (resizeHandle.containsMouse ? 0.45 : 0)
            Behavior on opacity {
                enabled: !theme.reducedMotion
                NumberAnimation { duration: 120; easing.type: Easing.OutCubic }
            }
        }
    }

    Item {
        id: header
        width: parent.width
        height: panel.headerHeight
        Row {
            id: titleRow
            anchors.left: parent.left
            anchors.leftMargin: 16
            anchors.right: extra.left
            anchors.rightMargin: 8
            anchors.verticalCenter: parent.verticalCenter
            spacing: 8
            Glyph {
                id: titleGlyph
                visible: panel.glyph !== ""
                anchors.verticalCenter: parent.verticalCenter
                width: 16
                height: 16
                path: panel.glyph
                color: panel.accentTone
            }
            Label {
                anchors.verticalCenter: parent.verticalCenter
                width: Math.min(implicitWidth, Math.max(0, titleRow.width - (titleGlyph.visible ? titleGlyph.width + titleRow.spacing : 0)))
                text: panel.title
                color: panel.look ? panel.look.fg : panel.palette.windowText
                font.pixelSize: 14
                font.weight: Font.DemiBold
                elide: Text.ElideRight
            }
        }
        Row {
            id: extra
            anchors.right: closeButton.left
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            spacing: 2
        }
        IconButton {
            id: closeButton
            anchors.right: parent.right
            anchors.rightMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            width: 28
            height: 28
            look: panel.look
            glyph: Icons.close
            tip: qsTr("Close (Esc)")
            onClicked: panel.close()
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: panel.dividerColor
        }
    }

    Item {
        id: body
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
    }
}
