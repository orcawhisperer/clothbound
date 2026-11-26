import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// A segmented control: exclusive choices in one quiet track, the current one
// raised. `model` items may be plain strings or objects
// `{ value, label, subtitle, glyph, enabled }`; with `glyph` the label becomes
// its tooltip; with `subtitle` the track grows to 44 px for two-line tabs.
FocusScope {
    id: control

    property var model: []
    property var current
    property bool compact: false
    signal activated(var value)

    readonly property int count: model ? model.length : 0
    readonly property bool hasSubtitles: {
        for (let i = 0; i < count; ++i)
            if (subtitleAt(i) !== "")
                return true
        return false
    }
    function valueAt(i) { const m = model[i]; return typeof m === "object" && m !== null ? m.value : m }
    function labelAt(i) { const m = model[i]; return typeof m === "object" && m !== null ? (m.label || "") : String(m) }
    function subtitleAt(i) { const m = model[i]; return typeof m === "object" && m !== null && m.subtitle ? String(m.subtitle) : "" }
    function glyphAt(i) { const m = model[i]; return typeof m === "object" && m !== null && m.glyph ? m.glyph : "" }
    function enabledAt(i) { const m = model[i]; return !(typeof m === "object" && m !== null && m.enabled === false) }
    function indexOfCurrent() { for (let i = 0; i < count; ++i) if (valueAt(i) === current) return i; return -1 }
    function choose(i) {
        if (i >= 0 && i < count && enabledAt(i) && valueAt(i) !== current)
            activated(valueAt(i))
    }
    function step(delta) {
        let i = indexOfCurrent() + delta
        while (i >= 0 && i < count) {
            if (enabledAt(i)) {
                choose(i)
                return
            }
            i += delta
        }
    }

    implicitHeight: hasSubtitles ? 44 : (compact ? 28 : 30)
    // The segments' own widths, summed apart from their laid-out widths so a
    // stretched control can share its width out evenly.
    readonly property real naturalWidth: {
        let sum = 0
        for (let i = 0; repeater.built >= 0 && i < repeater.count; ++i) {
            const item = repeater.itemAt(i)
            if (item) sum += item.implicitWidth
        }
        return sum
    }
    implicitWidth: naturalWidth + 4
    activeFocusOnTab: true
    Accessible.role: Accessible.PageTabList

    Keys.onLeftPressed: step(control.mirrored ? 1 : -1)
    Keys.onRightPressed: step(control.mirrored ? -1 : 1)
    readonly property bool mirrored: LayoutMirroring.enabled

    // 2 px offset focus ring when reached via keyboard Tab.
    Rectangle {
        anchors.fill: parent
        anchors.margins: -3
        visible: control.activeFocus
        radius: Ui.radius(10, theme.radius)
        color: "transparent"
        border.width: 2
        border.color: control.palette.highlight
    }

    Rectangle {
        anchors.fill: parent
        radius: Ui.radius(7, theme.radius)
        color: Ui.tint(control.palette.windowText, 0.06)
    }

    // The active pill glides under the current choice.
    Rectangle {
        id: thumb
        // itemAt() isn't a notifying binding, so it is read again whenever
        // segments are created or the choice changes.
        readonly property Item target: repeater.built >= 0 && control.indexOfCurrent() >= 0
                                       ? repeater.itemAt(control.indexOfCurrent()) : null
        visible: target !== null
        x: target ? row.x + target.x : 0
        y: 2
        width: target ? target.width : 0
        height: control.height - 4
        radius: Ui.radius(5, theme.radius)
        color: Ui.tint(control.palette.windowText, 0.13)
        Behavior on x {
            enabled: !theme.reducedMotion
            NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
        }
        Behavior on width {
            enabled: !theme.reducedMotion
            NumberAnimation { duration: 160; easing.type: Easing.OutCubic }
        }
    }

    Row {
        id: row
        x: 2
        height: parent.height
        Repeater {
            id: repeater
            property int built: 0
            onItemAdded: built++
            onItemRemoved: built++
            model: control.count
            delegate: AbstractButton {
                id: segment
                required property int index
                readonly property bool chosen: control.valueAt(index) === control.current
                readonly property string glyph: control.glyphAt(index)
                readonly property string subtitle: control.subtitleAt(index)
                readonly property bool itemEnabled: control.enabledAt(index)

                enabled: itemEnabled
                width: control.count <= 0 ? 0
                     : control.width > control.naturalWidth + 4
                       ? (control.width - 4) / control.count
                       : (control.naturalWidth > 0 && control.width > 4
                          ? implicitWidth * ((control.width - 4) / control.naturalWidth)
                          : implicitWidth)
                implicitWidth: glyph !== "" ? (control.compact ? 30 : 34)
                             : subtitle !== "" ? Math.max(twoLineTitle.implicitWidth, twoLineSub.implicitWidth) + 22
                             : label.implicitWidth + (control.compact ? 18 : 24)
                height: row.height
                hoverEnabled: itemEnabled
                focusPolicy: Qt.NoFocus
                opacity: itemEnabled ? 1 : 0.42
                Accessible.role: Accessible.PageTab
                Accessible.name: subtitle !== "" ? (control.labelAt(index) + ", " + subtitle) : control.labelAt(index)
                Accessible.checked: chosen
                onClicked: control.choose(index)
                ToolTip.visible: glyph !== "" && hovered
                ToolTip.text: control.labelAt(index)
                ToolTip.delay: 600

                readonly property color fgColor: segment.chosen ? control.palette.windowText
                                               : segment.hovered ? Ui.mix(control.palette.mid, control.palette.windowText, 0.5)
                                               : control.palette.mid

                contentItem: Item {
                    Glyph {
                        anchors.centerIn: parent
                        visible: segment.glyph !== ""
                        width: 14
                        height: 14
                        path: segment.glyph
                        color: segment.fgColor
                    }
                    Label {
                        id: label
                        anchors.fill: parent
                        visible: segment.glyph === "" && segment.subtitle === ""
                        text: control.labelAt(segment.index)
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        font.pixelSize: control.compact ? 12 : 13
                        font.weight: segment.chosen ? Font.Medium : Font.Normal
                        color: segment.fgColor
                    }
                    Column {
                        anchors.centerIn: parent
                        width: parent.width - 8
                        visible: segment.glyph === "" && segment.subtitle !== ""
                        spacing: 1
                        Label {
                            id: twoLineTitle
                            width: parent.width
                            text: control.labelAt(segment.index)
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                            color: segment.fgColor
                        }
                        Label {
                            id: twoLineSub
                            width: parent.width
                            text: segment.subtitle
                            horizontalAlignment: Text.AlignHCenter
                            elide: Text.ElideRight
                            font.pixelSize: 11
                            color: control.palette.mid
                        }
                    }
                }
                background: null
            }
        }
    }
}
