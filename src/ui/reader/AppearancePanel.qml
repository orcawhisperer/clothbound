import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

import "../components/Icons.js" as Icons
import "../components/Themes.js" as Themes
import "../components/Ui.js" as Ui

// How the page looks: theme, typeface, size, weight, spacing, margins, layout,
// and per-book overrides. Every change shows at once on the page beside it.
SidePanel {
    id: panel

    title: qsTr("Appearance")

    property var settings
    property bool pdf: false
    property var pdfSurface: null
    property bool bookOverride: false

    signal settingChanged(string key, var value)
    signal pdfFitChosen(string mode)
    signal bookOverrideToggled(bool enabled)

    function update(key, value) {
        settingChanged(key, value)
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

    component Heading: Label {
        color: panel.look ? panel.look.muted : panel.palette.mid
        font.pixelSize: 11
        font.weight: Font.DemiBold
        font.letterSpacing: 0.22
        Layout.topMargin: 12
    }

    component Choice: AbstractButton {
        id: choice
        property bool selected: false
        Layout.fillWidth: true
        implicitHeight: 30
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        scale: pressed && !theme.reducedMotion ? 0.98 : 1.0
        Behavior on scale {
            enabled: !theme.reducedMotion
            NumberAnimation { duration: 100; easing.type: Easing.OutCubic }
        }
        background: Rectangle {
            radius: Ui.radius(7, theme.radius)
            color: choice.selected ? Ui.tint(panel.look.link, 0.18)
                 : choice.hovered ? Ui.tint(panel.look.fg, 0.08) : Ui.tint(panel.look.fg, 0.04)
            Behavior on color {
                enabled: !theme.reducedMotion
                ColorAnimation { duration: 130; easing.type: Easing.OutCubic }
            }
            border.width: choice.selected ? 1.5 : 0
            border.color: panel.look.link
            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                visible: choice.visualFocus
                radius: Ui.radius(10, theme.radius)
                color: "transparent"
                border.width: 2
                border.color: panel.look.link
            }
        }
        contentItem: Label {
            text: choice.text
            color: panel.look.fg
            font.pixelSize: 13
            font.weight: choice.selected ? Font.Medium : Font.Normal
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
        spacing: 12
        Label {
            id: toggleLabel
            Layout.fillWidth: true
            color: panel.look.fg
            font.pixelSize: 13
            wrapMode: Text.Wrap
        }
        Switch {
            id: toggleSwitch
            focusPolicy: Qt.TabFocus
            onToggled: toggle.toggled(checked)
        }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: content.implicitHeight + 36
        boundsBehavior: Flickable.StopAtBounds
        clip: true
        ScrollBar.vertical: ScrollBar {}

        ColumnLayout {
            id: content
            x: 18
            y: 6
            width: panel.width - 36
            spacing: 8

            Heading { Layout.topMargin: 6; text: qsTr("Theme") }
            // 2 × 2 reading theme swatch grid (09_design_language:562-567).
            GridLayout {
                Layout.fillWidth: true
                columns: 2
                columnSpacing: 10
                rowSpacing: 10
                Repeater {
                    model: ["paper", "sepia", "dusk", "night"]
                    delegate: AbstractButton {
                        id: swatch
                        required property string modelData
                        readonly property var colors: Themes.palette(modelData, theme)
                        readonly property bool selected: panel.settings.theme === modelData
                        Layout.fillWidth: true
                        implicitHeight: 92
                        hoverEnabled: true
                        focusPolicy: Qt.TabFocus
                        Accessible.name: Themes.name(modelData)
                        Accessible.checked: selected
                        onClicked: panel.update("theme", modelData)
                        scale: pressed && !theme.reducedMotion ? 0.975 : 1.0
                        Behavior on scale {
                            enabled: !theme.reducedMotion
                            NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
                        }
                        background: Rectangle {
                            radius: Ui.radius(10, theme.radius)
                            color: swatch.colors.bg
                            border.width: swatch.selected ? 2 : 1
                            border.color: swatch.selected ? panel.look.link
                                        : swatch.hovered ? (swatch.colors.dark ? Qt.rgba(1, 1, 1, 0.24) : Qt.rgba(0, 0, 0, 0.24))
                                        : (swatch.colors.dark ? Qt.rgba(1, 1, 1, 0.10) : Qt.rgba(0, 0, 0, 0.10))
                            Behavior on border.color {
                                enabled: !theme.reducedMotion
                                ColorAnimation { duration: 140; easing.type: Easing.OutCubic }
                            }
                            Rectangle {
                                anchors.fill: parent
                                anchors.margins: -3
                                visible: swatch.visualFocus
                                radius: Ui.radius(13, theme.radius)
                                color: "transparent"
                                border.width: 2
                                border.color: panel.look.link
                            }
                        }
                        contentItem: Item {
                            Label {
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.leftMargin: 12
                                anchors.topMargin: 8
                                text: "Aa"
                                color: swatch.colors.fg
                                font.family: "Literata"
                                font.pixelSize: 28
                            }
                            Label {
                                anchors.left: parent.left
                                anchors.bottom: parent.bottom
                                anchors.leftMargin: 12
                                anchors.bottomMargin: 10
                                text: Themes.name(swatch.modelData)
                                color: swatch.colors.fg
                                font.pixelSize: 13
                                font.weight: Font.DemiBold
                            }
                        }
                    }
                }
            }

            // 5th reading theme: follow the live Omarchy desktop theme.
            AbstractButton {
                id: omarchySwatch
                readonly property var colors: Themes.palette("omarchy", theme)
                readonly property bool selected: panel.settings.theme === "omarchy"
                Layout.fillWidth: true
                implicitHeight: 38
                hoverEnabled: true
                focusPolicy: Qt.TabFocus
                Accessible.name: qsTr("Match desktop (Omarchy)")
                Accessible.checked: selected
                onClicked: panel.update("theme", "omarchy")
                scale: pressed && !theme.reducedMotion ? 0.985 : 1.0
                Behavior on scale {
                    enabled: !theme.reducedMotion
                    NumberAnimation { duration: 110; easing.type: Easing.OutCubic }
                }
                background: Rectangle {
                    radius: Ui.radius(8, theme.radius)
                    color: omarchySwatch.colors.bg
                    border.width: omarchySwatch.selected ? 2 : 1
                    border.color: omarchySwatch.selected ? panel.look.link
                                : omarchySwatch.hovered ? (omarchySwatch.colors.dark ? Qt.rgba(1, 1, 1, 0.24) : Qt.rgba(0, 0, 0, 0.24))
                                : (omarchySwatch.colors.dark ? Qt.rgba(1, 1, 1, 0.10) : Qt.rgba(0, 0, 0, 0.10))
                    Behavior on border.color {
                        enabled: !theme.reducedMotion
                        ColorAnimation { duration: 140; easing.type: Easing.OutCubic }
                    }
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -3
                        visible: omarchySwatch.visualFocus
                        radius: Ui.radius(11, theme.radius)
                        color: "transparent"
                        border.width: 2
                        border.color: panel.look.link
                    }
                }
                contentItem: RowLayout {
                    spacing: 10
                    Label {
                        Layout.leftMargin: 12
                        text: "Aa"
                        color: omarchySwatch.colors.fg
                        font.family: "Literata"
                        font.pixelSize: 16
                    }
                    Label {
                        Layout.fillWidth: true
                        text: qsTr("Match desktop (Omarchy)")
                        color: omarchySwatch.colors.fg
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Rectangle {
                        Layout.rightMargin: 12
                        Layout.preferredWidth: 8
                        Layout.preferredHeight: 8
                        radius: 4
                        color: theme.accent
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
                rowSpacing: 6
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
                            font.pixelSize: 13
                            color: panel.look.fg
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                    }
                }
            }

            Heading { visible: panel.pdf; text: qsTr("Fit") }
            Segmented {
                visible: panel.pdf
                Layout.fillWidth: true
                compact: true
                model: [{ value: "page", label: qsTr("Fit page") },
                        { value: "width", label: qsTr("Fit width") },
                        { value: "actual", label: qsTr("Actual") }]
                current: panel.pdfSurface ? panel.pdfSurface.fitMode : (panel.settings.pdfFit || "page")
                onActivated: (value) => panel.pdfFitChosen(value)
            }

            Heading { visible: panel.pdf; text: qsTr("Zoom") }
            RowLayout {
                visible: panel.pdf && panel.pdfSurface !== null
                Layout.fillWidth: true
                spacing: 8
                IconButton {
                    look: panel.look
                    glyph: Icons.zoomOut
                    tip: qsTr("Smaller (−)")
                    onClicked: if (panel.pdfSurface) panel.pdfSurface.nudgeZoom(1 / 1.15)
                }
                Slider {
                    Layout.fillWidth: true
                    from: 0.5
                    to: 4.0
                    stepSize: 0.05
                    value: panel.pdfSurface ? panel.pdfSurface.effectiveZoom : 1.0
                    focusPolicy: Qt.TabFocus
                    onMoved: if (panel.pdfSurface) panel.pdfSurface.setCustomZoom(value)
                }
                IconButton {
                    look: panel.look
                    glyph: Icons.zoomIn
                    tip: qsTr("Larger (+)")
                    onClicked: if (panel.pdfSurface) panel.pdfSurface.nudgeZoom(1.15)
                }
                Label {
                    Layout.preferredWidth: 42
                    horizontalAlignment: Text.AlignRight
                    text: Math.round((panel.pdfSurface ? panel.pdfSurface.effectiveZoom : 1.0) * 100).toLocaleString(Qt.locale(), 'f', 0) + "%"
                    color: panel.look.muted
                    font.pixelSize: 12
                    font.features: ({ "tnum": 1 })
                }
            }

            Heading { visible: !panel.pdf; text: qsTr("Size") }
            RowLayout {
                visible: !panel.pdf
                Layout.fillWidth: true
                spacing: 8
                IconButton {
                    look: panel.look
                    tip: qsTr("Smaller (−)")
                    contentItem: Label { text: "A"; font.family: "Literata"; font.pixelSize: 12; color: panel.look.fg; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: panel.update("fontScale", Math.max(0.7, Math.round((panel.settings.fontScale - 0.05) * 100) / 100))
                }
                Slider {
                    Layout.fillWidth: true
                    from: 0.7
                    to: 2.0
                    stepSize: 0.05
                    value: panel.settings.fontScale
                    focusPolicy: Qt.TabFocus
                    onMoved: panel.update("fontScale", Math.round(value * 100) / 100)
                }
                IconButton {
                    look: panel.look
                    tip: qsTr("Larger (+)")
                    contentItem: Label { text: "A"; font.family: "Literata"; font.pixelSize: 19; color: panel.look.fg; horizontalAlignment: Text.AlignHCenter; verticalAlignment: Text.AlignVCenter }
                    onClicked: panel.update("fontScale", Math.min(2.0, Math.round((panel.settings.fontScale + 0.05) * 100) / 100))
                }
                Label {
                    Layout.preferredWidth: 42
                    horizontalAlignment: Text.AlignRight
                    text: Math.round(panel.settings.fontScale * 100).toLocaleString(Qt.locale(), 'f', 0) + "%"
                    color: panel.look.muted
                    font.pixelSize: 12
                    font.features: ({ "tnum": 1 })
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
                    focusPolicy: Qt.TabFocus
                    onMoved: panel.update("fontWeight", Math.round(value))
                }
                Label {
                    Layout.preferredWidth: 42
                    horizontalAlignment: Text.AlignRight
                    text: Number(panel.settings.fontWeight).toLocaleString(Qt.locale(), 'f', 0)
                    color: panel.look.muted
                    font.pixelSize: 12
                    font.features: ({ "tnum": 1 })
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
                    focusPolicy: Qt.TabFocus
                    onMoved: panel.update("lineHeight", Math.round(value * 100) / 100)
                }
                Label {
                    Layout.preferredWidth: 42
                    horizontalAlignment: Text.AlignRight
                    text: Number(panel.settings.lineHeight).toLocaleString(Qt.locale(), 'f', 2)
                    color: panel.look.muted
                    font.pixelSize: 12
                    font.features: ({ "tnum": 1 })
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
                    focusPolicy: Qt.TabFocus
                    onMoved: panel.update("paragraphSpacing", Math.round(value * 100) / 100)
                }
                Label {
                    Layout.preferredWidth: 42
                    horizontalAlignment: Text.AlignRight
                    text: Number(panel.settings.paragraphSpacing).toLocaleString(Qt.locale(), 'f', 1) + " em"
                    color: panel.look.muted
                    font.pixelSize: 12
                    font.features: ({ "tnum": 1 })
                }
            }

            Heading { visible: !panel.pdf; text: qsTr("Margins") }
            Segmented {
                visible: !panel.pdf
                Layout.fillWidth: true
                compact: true
                model: [{ value: "narrow", label: qsTr("Narrow") },
                        { value: "normal", label: qsTr("Normal") },
                        { value: "wide", label: qsTr("Wide") }]
                current: panel.settings.margins
                onActivated: (value) => panel.update("margins", value)
            }

            Heading { text: qsTr("Layout") }
            Segmented {
                Layout.fillWidth: true
                compact: true
                model: [{ value: "auto", label: qsTr("Auto") },
                        { value: "single", label: qsTr("One page") },
                        { value: "double", label: qsTr("Two pages") },
                        { value: "scroll", label: qsTr("Scroll") }]
                current: panel.settings.layout
                onActivated: (value) => panel.update("layout", value)
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
                enabled: !panel.settings.publisher
                opacity: enabled ? 1 : 0.45
                text: qsTr("Drop caps")
                checked: panel.settings.dropCap
                onToggled: (on) => panel.update("dropCap", on)
            }
            Toggle {
                visible: !panel.pdf
                text: qsTr("Animate page turns")
                checked: panel.settings.animate
                onToggled: (on) => panel.update("animate", on)
            }
            Toggle {
                id: pageTurnSoundToggle
                text: qsTr("Page-turn sound")
                checked: readingSettings.pageTurnSound
                onToggled: (on) => {
                    readingSettings.pageTurnSound = on
                    if (on)
                        reader.playPageTurn(1)
                }
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
                color: panel.look.line
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
