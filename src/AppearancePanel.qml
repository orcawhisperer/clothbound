import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

import "Icons.js" as Icons
import "Themes.js" as Themes
import "Ui.js" as Ui

// How the page looks: theme, typeface, size, spacing, margins, and layout.
// Every change shows at once, on the page beside it.
FocusScope {
    id: panel

    property bool open: false
    property var look
    property var settings
    property bool pdf: false
    property var pdfSurface: null
    property bool bookOverride: false

    signal settingChanged(string key, var value)
    signal bookOverrideToggled(bool enabled)
    signal closed()

    function update(key, value) {
        settingChanged(key, value)
    }

    function show() {
        open = true
        forceActiveFocus()
    }
    function close() {
        if (!open)
            return
        open = false
        closed()
    }

    // Reading faces that are installed, plus the bundled Literata and the
    // book's own.
    readonly property var fonts: {
        const installed = Qt.fontFamilies()
        const wanted = ["Literata", "Noto Serif", "Charis SIL", "EB Garamond", "Crimson Pro", "Source Serif 4",
                        "C059", "Liberation Serif", "Merriweather", "Atkinson Hyperlegible", "iA Writer Quattro S",
                        "Adwaita Sans", "Noto Sans"]
        return wanted.filter(name => name === "Literata" || installed.indexOf(name) >= 0)
    }

    Keys.onEscapePressed: close()

    width: 380
    x: open ? parent.width - width : parent.width + 24
    Behavior on x { NumberAnimation { duration: 220; easing.type: Easing.OutCubic } }
    visible: x < parent.width + 20

    Material.theme: look.dark ? Material.Dark : Material.Light
    Material.accent: look.link

    Rectangle {
        anchors.fill: parent
        color: panel.look.popover
        Rectangle { width: 1; height: parent.height; color: panel.look.border }
    }
    // The panel keeps its clicks and the wheel: nothing falls through to the page.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        onWheel: (wheel) => wheel.accepted = true
    }
    Rectangle {
        anchors.right: parent.left
        width: 18
        height: parent.height
        gradient: Gradient {
            orientation: Gradient.Horizontal
            GradientStop { position: 0; color: "transparent" }
            GradientStop { position: 1; color: Ui.tint("black", panel.look.dark ? 0.35 : 0.12) }
        }
    }

    component Heading: Label {
        color: panel.look.muted
        font.pixelSize: 12
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 0.8
        Layout.topMargin: 14
    }

    component Choice: AbstractButton {
        id: choice
        property bool selected: false
        Layout.fillWidth: true
        implicitHeight: 34
        hoverEnabled: true
        focusPolicy: Qt.NoFocus
        background: Rectangle {
            radius: Math.min(6, theme.radius)
            color: choice.selected ? Ui.tint(panel.look.link, 0.2) : choice.hovered ? Ui.tint(panel.look.fg, 0.07) : Ui.tint(panel.look.fg, 0.035)
            border.width: choice.selected ? 1.5 : 0
            border.color: panel.look.link
        }
        contentItem: Label {
            text: choice.text
            color: panel.look.fg
            font.pixelSize: 13
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
    }

    component Toggle: RowLayout {
        id: toggle
        property alias text: toggleLabel.text
        property alias checked: toggleSwitch.checked
        signal toggled(bool on)
        Layout.fillWidth: true
        Label {
            id: toggleLabel
            Layout.fillWidth: true
            color: panel.look.fg
            font.pixelSize: 14
            wrapMode: Text.Wrap
        }
        Switch {
            id: toggleSwitch
            focusPolicy: Qt.NoFocus
            onToggled: toggle.toggled(checked)
        }
    }

    // The title and close stay put while the rest scrolls.
    RowLayout {
        id: header
        x: 24
        y: 22
        width: panel.width - 48
        Label {
            Layout.fillWidth: true
            text: qsTr("Appearance")
            color: panel.look.fg
            font.pixelSize: 18
            font.weight: Font.DemiBold
        }
        IconButton {
            glyph: Icons.close
            look: panel.look
            tip: qsTr("Close (Esc)")
            onClicked: panel.close()
        }
    }

    Flickable {
        anchors.fill: parent
        anchors.topMargin: header.y + header.height + 6
        contentHeight: content.implicitHeight + 30
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: content
            x: 24
            y: 4
            width: panel.width - 48
            spacing: 8


            Heading { text: qsTr("Theme") }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Repeater {
                    model: Themes.order
                    delegate: AbstractButton {
                        id: swatch
                        required property string modelData
                        readonly property var colors: Themes.palette(modelData, theme)
                        readonly property bool selected: panel.settings.theme === modelData
                        Layout.fillWidth: true
                        implicitHeight: 66
                        hoverEnabled: true
                        focusPolicy: Qt.NoFocus
                        onClicked: panel.update("theme", modelData)
                        background: Rectangle {
                            radius: Math.min(8, theme.radius)
                            color: swatch.colors.bg
                            border.width: swatch.selected ? 2 : 1
                            border.color: swatch.selected ? panel.look.link : panel.look.border
                        }
                        contentItem: Column {
                            spacing: 2
                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "Aa"
                                color: swatch.colors.fg
                                font.family: "Literata"
                                font.pixelSize: 20
                            }
                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: Themes.name(swatch.modelData)
                                color: swatch.colors.muted
                                font.pixelSize: 10
                            }
                        }
                    }
                }
            }

            Heading {
                visible: !panel.pdf
                opacity: panel.settings.publisher ? 0.45 : 1
                text: qsTr("Typeface")
            }
            GridLayout {
                visible: !panel.pdf
                enabled: !panel.settings.publisher
                opacity: panel.settings.publisher ? 0.45 : 1
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 8
                rowSpacing: 8
                Choice {
                    text: qsTr("Book's own")
                    selected: panel.settings.font === "publisher"
                    onClicked: panel.update("font", "publisher")
                }
                Repeater {
                    model: panel.fonts
                    delegate: Choice {
                        required property string modelData
                        text: modelData
                        font.family: modelData
                        selected: panel.settings.font === modelData
                        onClicked: panel.update("font", modelData)
                        contentItem: Label {
                            text: modelData
                            font.family: modelData
                            font.pixelSize: 14
                            color: panel.look.fg
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Heading { visible: panel.pdf; text: qsTr("Zoom") }
            RowLayout {
                visible: panel.pdf && panel.pdfSurface !== null
                Layout.fillWidth: true
                spacing: 10
                IconButton {
                    look: panel.look
                    glyph: Icons.zoomOut
                    tip: qsTr("Smaller (−)")
                    onClicked: if (panel.pdfSurface) panel.pdfSurface.zoom = Math.max(1, Math.round(panel.pdfSurface.zoom / 1.15 * 100) / 100)
                }
                Slider {
                    Layout.fillWidth: true
                    from: 1.0
                    to: 4.0
                    stepSize: 0.1
                    value: panel.pdfSurface ? panel.pdfSurface.zoom : 1.0
                    focusPolicy: Qt.NoFocus
                    onMoved: if (panel.pdfSurface) panel.pdfSurface.zoom = Math.round(value * 100) / 100
                }
                IconButton {
                    look: panel.look
                    glyph: Icons.zoomIn
                    tip: qsTr("Larger (+)")
                    onClicked: if (panel.pdfSurface) panel.pdfSurface.zoom = Math.min(4, Math.round(panel.pdfSurface.zoom * 1.15 * 100) / 100)
                }
                Label {
                    Layout.preferredWidth: 44
                    horizontalAlignment: Text.AlignRight
                    text: Math.round((panel.pdfSurface ? panel.pdfSurface.zoom : 1.0) * 100).toLocaleString(Qt.locale(), 'f', 0) + "%"
                    color: panel.look.muted
                    font.pixelSize: 12
                }
            }

            Heading { visible: !panel.pdf; text: qsTr("Size") }
            RowLayout {
                visible: !panel.pdf
                Layout.fillWidth: true
                spacing: 10
                IconButton {
                    look: panel.look
                    tip: qsTr("Smaller (−)")
                    contentItem: Label { text: "A"; font.family: "Literata"; font.pixelSize: 13; color: panel.look.fg; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: panel.update("fontScale", Math.max(0.7, Math.round((panel.settings.fontScale - 0.05) * 100) / 100))
                }
                Slider {
                    Layout.fillWidth: true
                    from: 0.7
                    to: 2.0
                    stepSize: 0.05
                    value: panel.settings.fontScale
                    focusPolicy: Qt.NoFocus
                    onMoved: panel.update("fontScale", Math.round(value * 100) / 100)
                }
                IconButton {
                    look: panel.look
                    tip: qsTr("Larger (+)")
                    contentItem: Label { text: "A"; font.family: "Literata"; font.pixelSize: 21; color: panel.look.fg; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: panel.update("fontScale", Math.min(2.0, Math.round((panel.settings.fontScale + 0.05) * 100) / 100))
                }
                Label {
                    Layout.preferredWidth: 44
                    horizontalAlignment: Text.AlignRight
                    text: Math.round(panel.settings.fontScale * 100).toLocaleString(Qt.locale(), 'f', 0) + "%"
                    color: panel.look.muted
                    font.pixelSize: 12
                }
            }

            Heading {
                visible: !panel.pdf
                opacity: panel.settings.publisher ? 0.45 : 1
                text: qsTr("Font weight")
            }
            RowLayout {
                visible: !panel.pdf
                enabled: !panel.settings.publisher
                opacity: panel.settings.publisher ? 0.45 : 1
                Layout.fillWidth: true
                spacing: 10
                Slider {
                    Layout.fillWidth: true
                    from: 300
                    to: 700
                    stepSize: 25
                    value: panel.settings.fontWeight
                    focusPolicy: Qt.NoFocus
                    onMoved: panel.update("fontWeight", Math.round(value))
                }
                Label {
                    Layout.preferredWidth: 44
                    horizontalAlignment: Text.AlignRight
                    text: Number(panel.settings.fontWeight).toLocaleString(Qt.locale(), 'f', 0)
                    color: panel.look.muted
                    font.pixelSize: 12
                }
            }

            Heading {
                visible: !panel.pdf
                opacity: panel.settings.publisher ? 0.45 : 1
                text: qsTr("Line spacing")
            }
            RowLayout {
                visible: !panel.pdf
                enabled: !panel.settings.publisher
                opacity: panel.settings.publisher ? 0.45 : 1
                Layout.fillWidth: true
                spacing: 10
                Slider {
                    Layout.fillWidth: true
                    from: 1.2
                    to: 2.0
                    stepSize: 0.05
                    value: panel.settings.lineHeight
                    focusPolicy: Qt.NoFocus
                    onMoved: panel.update("lineHeight", Math.round(value * 100) / 100)
                }
                Label {
                    Layout.preferredWidth: 44
                    horizontalAlignment: Text.AlignRight
                    text: Number(panel.settings.lineHeight).toLocaleString(Qt.locale(), 'f', 2)
                    color: panel.look.muted
                    font.pixelSize: 12
                }
            }

            Heading {
                visible: !panel.pdf
                opacity: panel.settings.publisher ? 0.45 : 1
                text: qsTr("Paragraph spacing")
            }
            RowLayout {
                visible: !panel.pdf
                enabled: !panel.settings.publisher
                opacity: panel.settings.publisher ? 0.45 : 1
                Layout.fillWidth: true
                spacing: 10
                Slider {
                    Layout.fillWidth: true
                    from: 0.0
                    to: 1.2
                    stepSize: 0.1
                    value: panel.settings.paragraphSpacing
                    focusPolicy: Qt.NoFocus
                    onMoved: panel.update("paragraphSpacing", Math.round(value * 100) / 100)
                }
                Label {
                    Layout.preferredWidth: 44
                    horizontalAlignment: Text.AlignRight
                    text: Number(panel.settings.paragraphSpacing).toLocaleString(Qt.locale(), 'f', 1) + " em"
                    color: panel.look.muted
                    font.pixelSize: 12
                }
            }

            Heading { visible: !panel.pdf; text: qsTr("Margins") }
            RowLayout {
                visible: !panel.pdf
                Layout.fillWidth: true
                spacing: 8
                Repeater {
                    model: [{ id: "narrow", label: qsTr("Narrow") }, { id: "normal", label: qsTr("Normal") }, { id: "wide", label: qsTr("Wide") }]
                    delegate: Choice {
                        required property var modelData
                        text: modelData.label
                        selected: panel.settings.margins === modelData.id
                        onClicked: panel.update("margins", modelData.id)
                    }
                }
            }

            Heading { text: qsTr("Layout") }
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 8
                rowSpacing: 8
                Repeater {
                    model: [{ id: "auto", label: qsTr("Automatic") }, { id: "single", label: qsTr("One page") },
                            { id: "double", label: qsTr("Two pages") }, { id: "scroll", label: qsTr("Scroll") }]
                    delegate: Choice {
                        required property var modelData
                        text: modelData.label
                        selected: panel.settings.layout === modelData.id
                        onClicked: panel.update("layout", modelData.id)
                    }
                }
            }

            Item { Layout.preferredHeight: 6 }
            Toggle {
                visible: panel.pdf
                text: qsTr("Trim margins")
                checked: panel.settings.pdfCrop
                onToggled: (on) => panel.update("pdfCrop", on)
            }
            Toggle {
                visible: panel.pdf
                text: qsTr("Original page colors")
                checked: panel.settings.pdfOriginalColors
                onToggled: (on) => panel.update("pdfOriginalColors", on)
            }
            Label {
                visible: panel.pdf
                Layout.fillWidth: true
                text: qsTr("Shows PDF pages with their original ink and paper instead of recoloring them to the reading theme.")
                color: panel.look.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
            Toggle {
                visible: !panel.pdf
                enabled: !panel.settings.publisher
                opacity: enabled ? 1 : 0.45
                text: qsTr("Justify text")
                checked: panel.settings.justify
                onToggled: (on) => panel.update("justify", on)
            }
            Toggle {
                visible: !panel.pdf
                enabled: !panel.settings.publisher && panel.settings.justify
                opacity: enabled ? 1 : 0.45
                text: qsTr("Hyphenate")
                checked: panel.settings.hyphenate
                onToggled: (on) => panel.update("hyphenate", on)
            }
            Toggle {
                visible: !panel.pdf
                text: qsTr("Animate page turns")
                checked: panel.settings.animate
                onToggled: (on) => panel.update("animate", on)
            }
            Toggle {
                visible: !panel.pdf
                text: qsTr("Use the book's own styles")
                checked: panel.settings.publisher
                onToggled: (on) => panel.update("publisher", on)
            }
            Label {
                visible: !panel.pdf
                Layout.fillWidth: true
                text: qsTr("Keeps the publisher's fonts, spacing, and layout; only the colors follow the theme.")
                color: panel.look.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
            Rectangle {
                Layout.fillWidth: true
                Layout.topMargin: 6
                Layout.bottomMargin: 2
                Layout.preferredHeight: 1
                color: panel.look.border
            }
            Toggle {
                text: qsTr("Custom settings for this book")
                checked: panel.bookOverride
                onToggled: (on) => panel.bookOverrideToggled(on)
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("Keeps separate appearance settings for this book without changing your defaults.")
                color: panel.look.muted
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }
        }
    }
}
