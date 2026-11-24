import QtQuick
import QtQuick.Controls

// Snappy, effortless vertical wheel and trackpad scrolling attachable directly
// inside any Flickable, GridView, or ListView.
//
// - Automatically resolves the enclosing Flickable/GridView/ListView (even when
//   Flickable's default `flickableData` initially parents children into
//   `contentItem`) and reparents onto the Flickable viewport itself so wheel
//   interception covers 100% of the visible viewport.
// - Trackpad / hi-res Wayland (pixelDelta.y !== 0 or TouchPad device):
//   * Tracks slow, careful movement crisply 1:1 under your fingers.
//   * Applies smooth velocity-adaptive gain during brisk strokes so moving top
//     to bottom doesn't take repeated swipes.
//   * Launches kinetic momentum coasting on finger lift (Qt.ScrollEnd,
//     WheelHandler deactivation, or a brief idle fallback when the compositor
//     doesn't emit ScrollEnd), with flick compounding on consecutive strokes
//     and instant stop when fingers touch back down.
//   * Detects native Qt.ScrollMomentum events and passes them through directly
//     without double-coasting.
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
    property real maxTrackpadGain: 2.8
    property real coastDeceleration: 3200
    property real maxCoastVelocity: 9000
    property real minCoastVelocity: 240

    readonly property bool gliding: _gliding
    readonly property bool coasting: _coasting
    readonly property real activeTargetY: _targetY
    readonly property bool reducedMotion: typeof theme !== "undefined" && theme !== null ? !!theme.reducedMotion : false

    property bool _gliding: false
    property bool _coasting: false
    property real _targetY: 0
    property real _animFromY: 0
    property real _lastExpectedY: 0

    // Trackpad gesture velocity & kinetic coasting state.
    property var _trackSamples: []
    property double _trackLastMs: 0
    property int _trackDir: 0
    property real _trackVelocity: 0
    property bool _trackSawMomentum: false
    property double _coastStartMs: 0
    property int _coastDuration: 0
    property real _coastLaunchVel: 0
    property real _carryVelocity: 0
    property double _carryMs: 0

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

    // Smooth velocity-adaptive gain for trackpad scrolling: stays 1.0x for
    // slow precision scrolling and ramps smoothly up to maxTrackpadGain for
    // brisk swipes.
    function trackpadGainForSpeed(rawSpeedPxPerSec) {
        const speed = Math.abs(Number(rawSpeedPxPerSec) || 0)
        if (speed <= 300 || maxTrackpadGain <= 1.0)
            return 1.0
        const t = Math.min(1.0, (speed - 300.0) / 1800.0)
        return 1.0 + (maxTrackpadGain - 1.0) * (t * (2.0 - t))
    }

    // Pure helper for kinetic coast distance & duration (velocity-matched to
    // OutCubic's initial slope 3*D/T for seamless zero-pop handoff).
    function computeCoastPlan(currentY, velocityPxPerSec, minY, maxY) {
        const lo = Number(minY) || 0
        const hi = Math.max(lo, Number(maxY) || 0)
        const cur = Math.max(lo, Math.min(hi, Number(currentY) || 0))
        const vRaw = Number(velocityPxPerSec) || 0
        const speed = Math.min(maxCoastVelocity, Math.abs(vRaw))
        if (hi <= lo || speed < minCoastVelocity || coastDeceleration <= 0)
            return { targetY: cur, duration: 0, distance: 0 }

        const sign = vRaw >= 0 ? 1 : -1
        const unclampedDist = (speed * speed) / (2.0 * coastDeceleration)
        const rawTarget = cur + sign * unclampedDist
        const targetY = Math.max(lo, Math.min(hi, rawTarget))
        const clampedDist = Math.abs(targetY - cur)
        if (clampedDist < 2.0)
            return { targetY: cur, duration: 0, distance: 0 }

        let duration = Math.min(1150, Math.max(220, Math.round((3000.0 * unclampedDist) / speed)))
        if (clampedDist < unclampedDist) {
            const ratio = clampedDist / unclampedDist
            const hitFrac = 1.0 - Math.pow(Math.max(0.0, 1.0 - ratio), 1.0 / 3.0)
            duration = Math.max(110, Math.round(duration * Math.max(hitFrac, ratio * 0.65)))
        }
        return { targetY: targetY, duration: duration, distance: clampedDist }
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

    function _resetTrackpadState() {
        coastFallbackTimer.stop()
        _trackSamples = []
        _trackLastMs = 0
        _trackDir = 0
        _trackVelocity = 0
        _trackSawMomentum = false
    }

    function _captureCoastCarry(now) {
        if (!_coasting || _coastDuration <= 0 || _coastLaunchVel === 0)
            return
        const elapsed = Math.max(0, now - _coastStartMs)
        const u = Math.min(1.0, elapsed / _coastDuration)
        // Derivative of OutCubic (1 - (1 - u)^3) is proportional to (1 - u)^2.
        const rem = _coastLaunchVel * (1.0 - u) * (1.0 - u)
        if (Math.abs(rem) >= minCoastVelocity) {
            _carryVelocity = rem
            _carryMs = now
        } else {
            _carryVelocity = 0
            _carryMs = 0
        }
    }

    function cancel() {
        coastFallbackTimer.stop()
        if (glideAnim.running)
            glideAnim.stop()
        _gliding = false
        _coasting = false
        _coastLaunchVel = 0
        _carryVelocity = 0
        _carryMs = 0
        _resetTrackpadState()
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

    function _finishTrackpadGesture() {
        coastFallbackTimer.stop()
        const flick = target
        const now = Date.now()
        const vel = _trackVelocity
        const lastMs = _trackLastMs
        const sampleCount = _trackSamples.length
        const sawMomentum = _trackSawMomentum
        _resetTrackpadState()

        if (!flick || !flick.visible || !flick.interactive || reducedMotion || sawMomentum)
            return false
        if (sampleCount < 2 || lastMs <= 0 || (now - lastMs) > 95)
            return false

        // Compound momentum if the user quickly flicked again in the same direction.
        let launchVel = vel
        if (_carryVelocity !== 0 && (now - _carryMs) <= 420
                && Math.sign(_carryVelocity) === Math.sign(launchVel)) {
            launchVel += _carryVelocity * 0.6
        }
        _carryVelocity = 0
        _carryMs = 0

        if (Math.abs(launchVel) > maxCoastVelocity)
            launchVel = Math.sign(launchVel) * maxCoastVelocity
        if (Math.abs(launchVel) < minCoastVelocity)
            return false

        const minY = minContentY(flick)
        const maxY = maxContentY(flick)
        const currentY = flick.contentY
        const plan = computeCoastPlan(currentY, launchVel, minY, maxY)
        if (plan.duration <= 0 || plan.distance < 2.0)
            return false

        glideAnim.stop()
        _targetY = plan.targetY
        _animFromY = currentY
        _lastExpectedY = currentY
        _gliding = true
        _coasting = true
        _coastStartMs = now
        _coastDuration = plan.duration
        _coastLaunchVel = launchVel
        glideAnim.duration = plan.duration
        glideAnim.from = currentY
        glideAnim.to = plan.targetY
        glideAnim.start()
        _pulseScrollBar()
        return true
    }

    // Backwards-compatible entry point (mouse or direct trackpad delta).
    function handleWheelDelta(angleDeltaY, pixelDeltaY) {
        return handleWheelEvent(angleDeltaY, pixelDeltaY, Qt.NoScrollPhase, false)
    }

    // Full wheel handler supporting ScrollPhase + device type for kinetic trackpad scrolling.
    function handleWheelEvent(angleDeltaY, pixelDeltaY, phase, isTouchPad) {
        const flick = target
        if (!flick || !flick.visible || !flick.interactive)
            return false

        const minY = minContentY(flick)
        const maxY = maxContentY(flick)
        if (maxY <= minY + 0.5)
            return false

        const py = Number(pixelDeltaY) || 0
        const ay = Number(angleDeltaY) || 0
        const p = (phase === undefined || phase === null) ? Qt.NoScrollPhase : phase

        if (p === Qt.ScrollEnd) {
            const hadGesture = _trackSamples.length > 0 || _trackSawMomentum
            _finishTrackpadGesture()
            return hadGesture
        }

        if (p === Qt.ScrollBegin && py === 0 && ay === 0) {
            const now = Date.now()
            _captureCoastCarry(now)
            if (glideAnim.running)
                glideAnim.stop()
            _gliding = false
            _coasting = false
            _resetTrackpadState()
            _targetY = flick.contentY
            return true
        }

        if (py === 0 && ay === 0)
            return false

        const currentY = flick.contentY
        const trackpad = (py !== 0) || (!!isTouchPad && ay !== 0) || (p !== Qt.NoScrollPhase && ay !== 0)

        if (trackpad) {
            const now = Date.now()
            const baseDelta = (py !== 0) ? (-py * pixelMultiplier) : (-(ay / 120.0) * stepSize)

            // Native compositor momentum (e.g. macOS or momentum-enabled compositors):
            // follow directly and never layer synthetic coasting on top.
            if (p === Qt.ScrollMomentum) {
                _trackSawMomentum = true
                coastFallbackTimer.stop()
                if (glideAnim.running)
                    glideAnim.stop()
                _gliding = false
                _coasting = false
                _carryVelocity = 0
                const nextMomY = Math.max(minY, Math.min(maxY, currentY + baseDelta))
                _targetY = nextMomY
                if (Math.abs(nextMomY - currentY) >= 0.01)
                    flick.contentY = nextMomY
                _pulseScrollBar()
                return true
            }

            // Active two-finger drag: capture any ongoing coast for flick compounding,
            // then cancel active glide so movement stays tightly locked to fingers.
            if (_coasting)
                _captureCoastCarry(now)
            if (glideAnim.running)
                glideAnim.stop()
            _gliding = false
            _coasting = false

            const dir = baseDelta >= 0 ? 1 : -1
            const gapMs = _trackLastMs > 0 ? (now - _trackLastMs) : 9999
            const isNewGesture = (_trackSamples.length === 0) || (gapMs > 110)
                                 || (p === Qt.ScrollBegin) || (_trackDir !== 0 && dir !== _trackDir)

            let gain = 1.0
            if (isNewGesture) {
                if (_carryVelocity !== 0 && Math.sign(_carryVelocity) !== dir) {
                    _carryVelocity = 0
                    _carryMs = 0
                }
                _trackSamples = [{ t: now, d: baseDelta }]
                _trackDir = dir
                _trackVelocity = 0
                _trackSawMomentum = false
            } else {
                const samples = _trackSamples.slice()
                samples.push({ t: now, d: baseDelta })
                while (samples.length > 2 && (now - samples[0].t) > 85)
                    samples.shift()
                _trackSamples = samples

                const rawSpan = now - samples[0].t
                const spanMs = rawSpan >= 4 ? rawSpan : Math.max(8, (samples.length - 1) * 8)
                let sumD = 0
                for (let i = 1; i < samples.length; ++i)
                    sumD += samples[i].d
                const rawVel = (sumD * 1000.0) / spanMs
                gain = trackpadGainForSpeed(rawVel)
                const effVel = rawVel * gain
                _trackVelocity = (_trackVelocity !== 0 && Math.sign(_trackVelocity) === dir)
                                 ? (0.65 * effVel + 0.35 * _trackVelocity)
                                 : effVel
            }
            _trackLastMs = now

            const nextY = Math.max(minY, Math.min(maxY, currentY + baseDelta * gain))
            _targetY = nextY
            if (Math.abs(nextY - currentY) >= 0.01)
                flick.contentY = nextY
            _pulseScrollBar()

            if (!reducedMotion && _trackSamples.length >= 2 && Math.abs(_trackVelocity) >= minCoastVelocity)
                coastFallbackTimer.restart()
            else
                coastFallbackTimer.stop()
            return true
        }

        // Standard mouse wheel: smooth glide with accumulated targetY (instant if reducedMotion).
        _resetTrackpadState()
        _coasting = false
        _carryVelocity = 0
        const nextY = computeWheelTarget(currentY, _targetY, _gliding, minY, maxY, ay, 0)
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
        glideAnim.duration = glideDuration
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

        onActiveChanged: {
            if (!active && root._trackSamples.length >= 2)
                root._finishTrackpadGesture()
        }

        onWheel: (event) => {
            const py = event.pixelDelta ? event.pixelDelta.y : 0
            const px = event.pixelDelta ? event.pixelDelta.x : 0
            const ay = event.angleDelta ? event.angleDelta.y : 0
            const ax = event.angleDelta ? event.angleDelta.x : 0
            const phase = event.phase !== undefined ? event.phase : Qt.NoScrollPhase
            const isTouchPad = event.device !== null && event.device !== undefined
                               && event.device.type === PointerDevice.TouchPad

            // Leave horizontal trackpad swipes and tilt-wheel events untouched.
            if (py !== 0 && Math.abs(px) > Math.abs(py)) {
                event.accepted = false
                return
            }
            if (py === 0 && ay !== 0 && Math.abs(ax) > Math.abs(ay)) {
                event.accepted = false
                return
            }
            if (!root.handleWheelEvent(ay, py, phase, isTouchPad))
                event.accepted = false
        }
    }

    // Fallback when a Wayland compositor/driver omits Qt.ScrollEnd on finger lift.
    Timer {
        id: coastFallbackTimer
        interval: 44
        onTriggered: root._finishTrackpadGesture()
    }

    NumberAnimation {
        id: glideAnim
        target: root.target
        property: "contentY"
        duration: root.reducedMotion ? 0 : root.glideDuration
        easing.type: Easing.OutCubic
        onFinished: {
            root._gliding = false
            root._coasting = false
            root._coastLaunchVel = 0
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
                    root._coasting = false
                    root.target.contentY = clamped
                } else {
                    glideAnim.duration = Math.min(glideAnim.duration > 0 ? glideAnim.duration : root.glideDuration, root.glideDuration)
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
