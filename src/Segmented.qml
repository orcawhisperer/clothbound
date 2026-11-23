import QtQuick
import QtQuick.Controls

import "Ui.js" as Ui

// A segmented control: a few exclusive choices in one quiet track, the
// current one raised. `model` is a list of {value, label} (or plain
// strings); `current` is the chosen value; `activated(value)` on a choice.
// Arrow keys move the choice when it has focus.
FocusScope {
    id: control

    property var model: []
    property var current
    property bool compact: false
    signal activated(var value)

    readonly property int count: model ? model.length : 0
    function valueAt(i) { const m = model[i]; return typeof m === "object" ? m.value : m }
    function labelAt(i) { const m = model[i]; return typeof m === "object" ? m.label : String(m) }
    function indexOfCurrent() { for (let i = 0; i < count; ++i) if (valueAt(i) === current) return i; return -1 }
    function choose(i) { if (i >= 0 && i < count && valueAt(i) !== current) activated(valueAt(i)) }

    implicitHeight: compact ? 26 : 30
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

    Keys.onLeftPressed: choose(indexOfCurrent() + (control.mirrored ? 1 : -1))
    Keys.onRightPressed: choose(indexOfCurrent() + (control.mirrored ? -1 : 1))
    readonly property bool mirrored: LayoutMirroring.enabled

    Rectangle {
        anchors.fill: parent
        radius: Math.min(8, theme.radius)
        color: Ui.tint(control.palette.windowText, 0.06)
        border.width: control.activeFocus ? 2 : 0
        border.color: control.palette.highlight
    }

    // The raised thumb glides under the current choice.
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
        radius: Math.max(0, Math.min(6, theme.radius))
        color: Ui.mix(control.palette.window, control.palette.light, 0.85)
        border.width: 1
        border.color: Ui.tint(control.palette.windowText, 0.08)
        Behavior on x { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
        Behavior on width { NumberAnimation { duration: 160; easing.type: Easing.OutCubic } }
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
                width: control.width > control.naturalWidth + 4
                       ? (control.width - 4) / control.count : implicitWidth
                implicitWidth: label.implicitWidth + (control.compact ? 18 : 24)
                height: row.height
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                Accessible.role: Accessible.PageTab
                Accessible.name: label.text
                Accessible.checked: chosen
                onClicked: control.choose(index)
                contentItem: Label {
                    id: label
                    text: control.labelAt(segment.index)
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    elide: Text.ElideRight
                    font.pixelSize: control.compact ? 12 : 13
                    font.weight: segment.chosen ? Font.DemiBold : Font.Medium
                    color: segment.chosen ? control.palette.windowText
                         : segment.hovered ? Ui.mix(control.palette.mid, control.palette.windowText, 0.5) : control.palette.mid
                }
                background: null
            }
        }
    }
}
