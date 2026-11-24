import QtQuick
import QtQuick.Controls

// Snappy, smooth vertical wheel and trackpad scrolling attachable directly
// inside any Flickable, GridView, or ListView.
//
// - Automatically resolves the enclosing Flickable/GridView/ListView (even when
//   Flickable's default `flickableData` initially parents children into
//   `contentItem`) and reparents onto the Flickable viewport itself so wheel
//   interception covers 100% of the visible viewport.
// - Trackpad / hi-res Wayland (pixelDelta.y !== 0): cancels any active mouse
//   glide and moves contentY immediately by -pixelDelta.y * pixelMultiplier so
//   two-finger gestures stay 1:1 locked to fingers.
// - Mouse wheel (angleDelta.y !== 0, pixelDelta.y === 0): steps by `stepSize`
//   per 120-unit notch, accumulates onto active targetY during rapid spinning
//   in the same direction (or pivots immediately on direction reversal), clamps
//   to [originY - topMargin, originY + contentHeight + bottomMargin - height],
//   and glides over `glideDuration` ms with OutCubic easing (or jumps
//   instantaneously when theme.reducedMotion is enabled).
Item {
    id: root

    property Flickable target: null
    property real stepSize: 135
    property real pixelMultiplier: 1.25
    property int glideDuration: 135

    readonly property bool gliding: _gliding
    readonly property real activeTargetY: _targetY
    readonly property bool reducedMotion: typeof theme !== "undefined" && theme !== null ? !!theme.reducedMotion : false

    property bool _gliding: false
    property real _targetY: 0
    property real _animFromY: 0
    property real _lastExpectedY: 0

    // Cover the exact viewport of `target` without using anchors (avoids anchor
    // warnings during the brief moment before reparenting off `contentItem`).
    x: 0
    y: 0
    width: target ? target.width : 0
    height: target ? target.height : 0

    function _resolveFlickable(item) {
        if (!item)
            return null
        const direct = item as Flickable
        if (direct)
            return direct
        return item.parent ? (item.parent as Flickable) : null
    }

    function _attachToTarget() {
        if (target && parent !== target)
            parent = target
    }

    Component.onCompleted: {
        if (!target) {
            const resolved = _resolveFlickable(parent)
            if (resolved)
                target = resolved
        }
        _attachToTarget()
    }

    onTargetChanged: {
        cancel()
        _attachToTarget()
    }

    onEnabledChanged: {
        if (!enabled)
            cancel()
    }

    // Pure helper for deterministic unit testing and wheel target math.
    function computeWheelTarget(currentY, activeTarget, isGliding, minY, maxY, angleDeltaY, pixelDeltaY) {
        const lo = Number(minY) || 0
        const hi = Math.max(lo, Number(maxY) || 0)
        const cur = Math.max(lo, Math.min(hi, Number(currentY) || 0))
        if (hi <= lo)
            return cur

        const py = Number(pixelDeltaY) || 0
        if (py !== 0) {
            const raw = cur - py * pixelMultiplier
            return Math.max(lo, Math.min(hi, raw))
        }

        const ay = Number(angleDeltaY) || 0
        if (ay === 0)
            return cur

        const delta = -(ay / 120.0) * stepSize
        let base = cur
        if (isGliding) {
            const tgt = Math.max(lo, Math.min(hi, Number(activeTarget) || 0))
            const sameDir = (delta > 0 && tgt >= cur) || (delta < 0 && tgt <= cur)
            if (sameDir)
                base = tgt
        }
        return Math.max(lo, Math.min(hi, base + delta))
    }

    function minContentY(flick) {
        return flick ? (flick.originY - flick.topMargin) : 0
    }

    function maxContentY(flick) {
        if (!flick)
            return 0
        const lo = minContentY(flick)
        return Math.max(lo, flick.originY + flick.contentHeight + flick.bottomMargin - flick.height)
    }

    function cancel() {
        if (glideAnim.running)
            glideAnim.stop()
        _gliding = false
        if (target)
            _targetY = target.contentY
    }

    function _verticalScrollBar() {
        return target ? target.ScrollBar.vertical : null
    }

    function _pulseScrollBar() {
        const vbar = _verticalScrollBar()
        if (vbar && vbar.size < 1.0) {
            vbar.active = true
            barFadeTimer.restart()
        }
    }

    function _scheduleBarFade() {
        const vbar = _verticalScrollBar()
        if (vbar && vbar.active)
            barFadeTimer.restart()
    }

    // Applies a vertical wheel delta to `target`. Returns true when vertical
    // scroll room exists and the event was consumed.
    function handleWheelDelta(angleDeltaY, pixelDeltaY) {
        const flick = target
        if (!flick || !flick.visible || !flick.interactive)
            return false

        const minY = minContentY(flick)
        const maxY = maxContentY(flick)
        if (maxY <= minY + 0.5)
            return false

        const py = Number(pixelDeltaY) || 0
        const ay = Number(angleDeltaY) || 0
        if (py === 0 && ay === 0)
            return false

        const currentY = flick.contentY
        const nextY = computeWheelTarget(currentY, _targetY, _gliding, minY, maxY, ay, py)

        // Trackpad / hi-res pixelDelta: immediate 1:1 tracking.
        if (py !== 0) {
            if (glideAnim.running)
                glideAnim.stop()
            _gliding = false
            _targetY = nextY
            if (Math.abs(nextY - currentY) >= 0.01)
                flick.contentY = nextY
            _pulseScrollBar()
            return true
        }

        // Mouse wheel: smooth glide with accumulated targetY (instant if reducedMotion).
        _targetY = nextY
        if (Math.abs(nextY - currentY) < 0.5 || reducedMotion || glideDuration <= 0) {
            if (glideAnim.running)
                glideAnim.stop()
            _gliding = false
            flick.contentY = nextY
            _pulseScrollBar()
            return true
        }

        glideAnim.stop()
        _animFromY = currentY
        _lastExpectedY = currentY
        _gliding = true
        glideAnim.from = currentY
        glideAnim.to = nextY
        glideAnim.start()
        _pulseScrollBar()
        return true
    }

    WheelHandler {
        id: wheelHandler
        enabled: root.enabled && root.target !== null
        target: null
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        acceptedModifiers: Qt.NoModifier
        orientation: Qt.Vertical
        blocking: true

        onWheel: (event) => {
            const py = event.pixelDelta ? event.pixelDelta.y : 0
            const px = event.pixelDelta ? event.pixelDelta.x : 0
            const ay = event.angleDelta ? event.angleDelta.y : 0
            const ax = event.angleDelta ? event.angleDelta.x : 0

            // Leave horizontal trackpad swipes and tilt-wheel events untouched.
            if (py !== 0 && Math.abs(px) > Math.abs(py)) {
                event.accepted = false
                return
            }
            if (py === 0 && ay !== 0 && Math.abs(ax) > Math.abs(ay)) {
                event.accepted = false
                return
            }
            if (!root.handleWheelDelta(ay, py))
                event.accepted = false
        }
    }

    NumberAnimation {
        id: glideAnim
        target: root.target
        property: "contentY"
        duration: root.reducedMotion ? 0 : root.glideDuration
        easing.type: Easing.OutCubic
        onFinished: {
            root._gliding = false
            root._scheduleBarFade()
        }
    }

    Timer {
        id: barFadeTimer
        interval: 650
        onTriggered: {
            const flick = root.target
            const vbar = root._verticalScrollBar()
            if (!vbar)
                return
            if (root._gliding || vbar.hovered || vbar.pressed || (flick && flick.moving)) {
                restart()
                return
            }
            vbar.active = false
        }
    }

    Connections {
        target: root.target
        ignoreUnknownSignals: true

        function onDraggingChanged() {
            if (root.target && root.target.dragging)
                root.cancel()
        }
        function onFlickingChanged() {
            if (root.target && root.target.flicking)
                root.cancel()
        }
        function onVisibleChanged() {
            if (root.target && !root.target.visible)
                root.cancel()
        }
        function onContentYChanged() {
            if (!root._gliding || !root.target)
                return
            const y = root.target.contentY
            const lo = Math.min(root._animFromY, root._targetY) - 2
            const hi = Math.max(root._animFromY, root._targetY) + 2
            const goingDown = root._targetY >= root._animFromY
            const reversed = goingDown ? (y < root._lastExpectedY - 2) : (y > root._lastExpectedY + 2)
            if (y < lo || y > hi || reversed)
                root.cancel()
            else
                root._lastExpectedY = y
        }
        function onContentHeightChanged() {
            if (!root._gliding || !root.target)
                return
            const minY = root.minContentY(root.target)
            const maxY = root.maxContentY(root.target)
            if (maxY <= minY + 0.5) {
                root.cancel()
            } else if (root._targetY < minY || root._targetY > maxY) {
                const clamped = Math.max(minY, Math.min(maxY, root._targetY))
                glideAnim.stop()
                root._targetY = clamped
                root._animFromY = root.target.contentY
                root._lastExpectedY = root.target.contentY
                if (Math.abs(clamped - root.target.contentY) < 0.5 || root.reducedMotion) {
                    root._gliding = false
                    root.target.contentY = clamped
                } else {
                    glideAnim.from = root.target.contentY
                    glideAnim.to = clamped
                    glideAnim.start()
                }
            }
        }
        function onOriginYChanged() {
            if (!root._gliding || !root.target)
                return
            const minY = root.minContentY(root.target)
            const maxY = root.maxContentY(root.target)
            if (root._targetY < minY || root._targetY > maxY)
                root.cancel()
        }
    }

    Connections {
        target: root.target ? root.target.ScrollBar.vertical : null
        ignoreUnknownSignals: true

        function onPressedChanged() {
            const vbar = root._verticalScrollBar()
            if (vbar && vbar.pressed)
                root.cancel()
        }
    }
}
