import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Icons.js" as Icons
import "Themes.js" as Themes
import "Ui.js" as Ui

// Settings › Reading: how every book looks unless it has its own look
// (Appearance, A, while reading changes the same values for the open book).
ColumnLayout {
    id: page

    spacing: 16

    property bool fallbackPageTurnSound: true
    property bool confirmingReset: false

    Timer {
        running: page.confirmingReset
        interval: 4000
        onTriggered: page.confirmingReset = false
    }

    readonly property bool hasPageTurnSoundProp: typeof readingSettings.pageTurnSound !== "undefined"
    readonly property bool pageTurnSoundOn: hasPageTurnSoundProp
                                          ? Boolean(readingSettings.pageTurnSound)
                                          : fallbackPageTurnSound
    readonly property var previewLook: Themes.palette(readingSettings.theme, theme)
    readonly property var fonts: {
        const installed = Qt.fontFamilies()
        const wanted = ["Literata", "Noto Serif", "Charis SIL", "EB Garamond", "Crimson Pro", "Source Serif 4",
                        "C059", "Liberation Serif", "Merriweather", "Atkinson Hyperlegible", "iA Writer Quattro S",
                        "Adwaita Sans", "Noto Sans"]
        return wanted.filter(name => name === "Literata" || installed.indexOf(name) >= 0)
    }

    function weightLabel(w) {
        if (w <= 325) return qsTr("Light (%1)").arg(w)
        if (w <= 425) return qsTr("Regular (%1)").arg(w)
        if (w <= 525) return qsTr("Medium (%1)").arg(w)
        if (w <= 625) return qsTr("Semibold (%1)").arg(w)
        return qsTr("Bold (%1)").arg(w)
    }

    function resetDefaults() {
        readingSettings.theme = "paper"
        readingSettings.font = "Literata"
        readingSettings.fontScale = 1.15
        readingSettings.lineHeight = 1.62
        readingSettings.margins = "normal"
        readingSettings.justify = true
        readingSettings.hyphenate = true
        readingSettings.dropCap = true
        readingSettings.layout = "auto"
        readingSettings.animate = true
        readingSettings.publisher = false
        readingSettings.pdfCrop = true
        readingSettings.pdfOriginalColors = false
        if (hasPageTurnSoundProp)
            readingSettings.pageTurnSound = true
        else
            fallbackPageTurnSound = true
        reader.fontWeight = 400
        reader.paragraphSpacing = 0.0
        toast.show(qsTr("Reading settings reset to defaults"))
    }

    // Notice when the currently open book overrides default reading styles.
    SettingsCard {
        visible: reader.open && reader.bookAppearanceOverride
        RowLayout {
            Layout.fillWidth: true
            spacing: 12
            Glyph {
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                path: Icons.infoCircle
                color: theme.accentInk
            }
            Label {
                Layout.fillWidth: true
                text: qsTr("“%1” currently uses its own per-book look. Defaults below apply to all other books.").arg(reader.title)
                textFormat: Text.PlainText
                color: theme.ink2
                font.pixelSize: 12
                wrapMode: Text.Wrap
                lineHeight: 1.35
            }
            TextButton {
                size: "compact"
                text: qsTr("Use defaults")
                onClicked: {
                    reader.clearBookAppearance()
                    toast.show(qsTr("Using default reading style for this book"))
                }
            }
        }
    }

    SettingsCard {
        title: qsTr("Theme & preview")
        glyph: Icons.sun
        description: qsTr("The page's colours. Match desktop wears your Omarchy theme.")

        // Live miniature reading page preview.
        Rectangle {
            id: previewBox
            Layout.fillWidth: true
            implicitHeight: previewCol.implicitHeight + 28
            radius: Ui.radius(10, theme.radius)
            color: page.previewLook.bg
            border.width: 1
            border.color: page.previewLook.dark ? Qt.rgba(1, 1, 1, 0.10) : Qt.rgba(0, 0, 0, 0.10)
            clip: true

            readonly property int sideInset: readingSettings.margins === "narrow" ? 18
                                           : readingSettings.margins === "wide" ? 56 : 36
            readonly property string previewFamily: readingSettings.publisher || readingSettings.font === "publisher"
                                                  ? "Literata" : readingSettings.font
            readonly property int previewSize: Math.max(11, Math.min(20, Math.round(13 * readingSettings.fontScale)))
            readonly property real previewLineHeight: readingSettings.publisher ? 1.55 : readingSettings.lineHeight
            readonly property int previewWeight: readingSettings.publisher ? 400 : reader.fontWeight
            readonly property int previewAlign: !readingSettings.publisher && readingSettings.justify
                                              ? Text.AlignJustify : Text.AlignLeft
            readonly property int dropCapWeight: Math.min(700, Math.max(500, previewWeight + 100))

            FontMetrics {
                id: previewBodyMetrics
                font.family: previewBox.previewFamily
                font.pixelSize: previewBox.previewSize
                font.weight: previewBox.previewWeight
            }

            readonly property real bodyCapHeight: {
                void previewBodyMetrics.ascent
                const h = previewBodyMetrics.tightBoundingRect("T").height
                return h > 0 ? h : previewSize * 0.72
            }
            readonly property real bodyLineStep: Math.max(previewSize, previewBodyMetrics.lineSpacing) * previewLineHeight
            readonly property int dropCapPixelSize: {
                // Span cleanly from line 1's cap-top to line 2's baseline.
                const targetCap = bodyCapHeight + bodyLineStep
                return Math.max(24, Math.round(targetCap * (previewSize / bodyCapHeight)))
            }

            FontMetrics {
                id: previewCapMetrics
                font.family: previewBox.previewFamily
                font.pixelSize: previewBox.dropCapPixelSize
                font.weight: previewBox.dropCapWeight
            }

            readonly property real dropCapGlyphHeight: {
                void previewCapMetrics.ascent
                const h = previewCapMetrics.tightBoundingRect("T").height
                return h > 0 ? h : dropCapPixelSize * 0.72
            }

            ColumnLayout {
                id: previewCol
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.topMargin: 14
                anchors.leftMargin: previewBox.sideInset
                anchors.rightMargin: previewBox.sideInset
                spacing: readingSettings.publisher ? 6 : Math.round(6 + reader.paragraphSpacing * 10)

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Label {
                        visible: !readingSettings.publisher && readingSettings.dropCap
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: Math.round((previewBodyMetrics.ascent - previewBox.bodyCapHeight)
                                                     - (previewCapMetrics.ascent - previewBox.dropCapGlyphHeight))
                        Layout.preferredHeight: Math.ceil(previewCapMetrics.ascent + 2)
                        topPadding: 0
                        bottomPadding: 0
                        text: "T"
                        color: page.previewLook.accentInk || page.previewLook.link
                        font.family: previewBox.previewFamily
                        font.pixelSize: previewBox.dropCapPixelSize
                        font.weight: previewBox.dropCapWeight
                        verticalAlignment: Text.AlignTop
                    }
                    Label {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignTop
                        text: (!readingSettings.publisher && readingSettings.dropCap ? "he" : "The")
                              + qsTr(" book opens calm, clear, and set to your hand. Every line keeps a quiet, unhurried rhythm across the page.")
                        color: page.previewLook.fg
                        font.family: previewBox.previewFamily
                        font.pixelSize: previewBox.previewSize
                        font.weight: previewBox.previewWeight
                        lineHeight: previewBox.previewLineHeight
                        horizontalAlignment: previewBox.previewAlign
                        wrapMode: Text.Wrap
                    }
                }
                Label {
                    Layout.fillWidth: true
                    text: qsTr("Adjust the theme, typeface, spacing, or margins below to see the page respond right away.")
                    color: page.previewLook.muted
                    font.family: previewBox.previewFamily
                    font.pixelSize: Math.max(11, previewBox.previewSize - 1)
                    font.weight: previewBox.previewWeight
                    lineHeight: previewBox.previewLineHeight
                    horizontalAlignment: previewBox.previewAlign
                    wrapMode: Text.Wrap
                }
            }
        }

        Flow {
            Layout.fillWidth: true
            spacing: 10
            Repeater {
                model: Themes.order
                delegate: AbstractButton {
                    id: swatch
                    required property string modelData
                    readonly property var look: Themes.palette(modelData, theme)
                    readonly property bool chosen: readingSettings.theme === modelData
                    width: 112
                    height: 74
                    hoverEnabled: true
                    focusPolicy: Qt.TabFocus
                    Accessible.role: Accessible.RadioButton
                    Accessible.name: modelData === "omarchy" ? qsTr("Match desktop") : Themes.name(modelData)
                    Accessible.checked: chosen
                    HoverHandler { cursorShape: Qt.PointingHandCursor }
                    onClicked: readingSettings.theme = modelData
                    background: Rectangle {
                        radius: Ui.radius(10, theme.radius)
                        color: swatch.look.bg
                        border.width: swatch.chosen ? 2 : 1
                        border.color: swatch.chosen ? theme.accent
                                    : swatch.hovered ? Ui.tint(swatch.look.fg, 0.32)
                                    : Ui.tint(swatch.look.fg, 0.16)
                        Rectangle {
                            anchors.fill: parent
                            anchors.margins: -3
                            visible: swatch.visualFocus
                            radius: Ui.radius(13, theme.radius)
                            color: "transparent"
                            border.width: 2
                            border.color: theme.accent
                        }
                    }
                    contentItem: Item {
                        Rectangle {
                            visible: swatch.chosen
                            anchors.top: parent.top
                            anchors.right: parent.right
                            anchors.topMargin: 7
                            anchors.rightMargin: 7
                            width: 16
                            height: 16
                            radius: 8
                            color: theme.accent
                            Glyph {
                                anchors.centerIn: parent
                                width: 10
                                height: 10
                                stroke: 2.5
                                path: Icons.check
                                color: theme.accentOn
                            }
                        }
                        Column {
                            anchors.centerIn: parent
                            spacing: 4
                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: "Aa"
                                color: swatch.look.fg
                                font.family: "Literata"
                                font.pixelSize: 20
                            }
                            Label {
                                anchors.horizontalCenter: parent.horizontalCenter
                                text: modelData === "omarchy" ? qsTr("Match desktop") : Themes.name(modelData)
                                color: Ui.tint(swatch.look.fg, 0.78)
                                font.pixelSize: 11
                                font.weight: swatch.chosen ? Font.DemiBold : Font.Normal
                            }
                        }
                    }
                }
            }
        }
    }

    SettingsCard {
        title: qsTr("Typography")
        glyph: Icons.bookOpen

        SettingsRow {
            title: qsTr("Use the book's own styles")
            description: qsTr("Follow the publisher's CSS for typeface, spacing, justification, and drop caps.")
            Switch {
                checked: readingSettings.publisher
                onToggled: readingSettings.publisher = checked
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            enabled: !readingSettings.publisher
            title: qsTr("Typeface")
            ComboBox {
                width: 220
                model: [qsTr("The book's own")].concat(page.fonts)
                currentIndex: readingSettings.font === "publisher"
                              ? 0 : Math.max(1, page.fonts.indexOf(readingSettings.font) + 1)
                onActivated: (index) => {
                    readingSettings.font = index === 0 ? "publisher" : page.fonts[index - 1]
                }
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            enabled: !readingSettings.publisher
            title: qsTr("Weight")
            description: page.weightLabel(reader.fontWeight)
            Slider {
                width: 220
                from: 300; to: 700; stepSize: 25
                value: reader.fontWeight
                onMoved: reader.fontWeight = Math.round(value / 25) * 25
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Size")
            description: qsTr("%1%").arg(Math.round(readingSettings.fontScale * 100).toLocaleString(Qt.locale(), 'f', 0))
            Slider {
                width: 220
                from: 0.7; to: 2.0; stepSize: 0.05
                value: readingSettings.fontScale
                onMoved: readingSettings.fontScale = Math.round(value * 100) / 100
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            enabled: !readingSettings.publisher
            title: qsTr("Line spacing")
            description: Number(readingSettings.lineHeight).toLocaleString(Qt.locale(), 'f', 2)
            Slider {
                width: 220
                from: 1.2; to: 2.0; stepSize: 0.05
                value: readingSettings.lineHeight
                onMoved: readingSettings.lineHeight = Math.round(value * 100) / 100
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            enabled: !readingSettings.publisher
            title: qsTr("Paragraph spacing")
            description: qsTr("%1 em").arg(Number(reader.paragraphSpacing).toLocaleString(Qt.locale(), 'f', 1))
            Slider {
                width: 220
                from: 0.0; to: 1.2; stepSize: 0.1
                value: reader.paragraphSpacing
                onMoved: reader.paragraphSpacing = Math.round(value * 10) / 10
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Margins")
            Segmented {
                model: [{ value: "narrow", label: qsTr("Narrow") },
                        { value: "normal", label: qsTr("Normal") },
                        { value: "wide", label: qsTr("Wide") }]
                current: readingSettings.margins
                onActivated: (value) => readingSettings.margins = value
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            enabled: !readingSettings.publisher
            title: qsTr("Justify")
            description: qsTr("Straight right edges, with hyphenation to keep spacing even.")
            Switch { checked: readingSettings.justify; onToggled: readingSettings.justify = checked }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            enabled: !readingSettings.publisher && readingSettings.justify
            title: qsTr("Hyphenate")
            description: readingSettings.justify
                         ? qsTr("Break long words across lines to avoid wide gaps.")
                         : qsTr("Applies when Justify is turned on.")
            Switch { checked: readingSettings.hyphenate; onToggled: readingSettings.hyphenate = checked }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            enabled: !readingSettings.publisher
            title: qsTr("Drop caps")
            description: qsTr("A large initial opens each chapter.")
            Switch { checked: readingSettings.dropCap; onToggled: readingSettings.dropCap = checked }
        }
    }

    SettingsCard {
        title: qsTr("Pages")
        glyph: Icons.page

        SettingsRow {
            title: qsTr("Layout")
            Segmented {
                model: [{ value: "auto", label: qsTr("Auto") }, { value: "single", label: qsTr("One page") },
                        { value: "double", label: qsTr("Two pages") }, { value: "scroll", label: qsTr("Scroll") }]
                current: readingSettings.layout
                onActivated: (value) => readingSettings.layout = value
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Animate page turns")
            description: theme.reducedMotion
                         ? qsTr("Paused while Reduce motion is turned on in General.")
                         : qsTr("Smooth page-turn glide in paginated view.")
            Switch { checked: readingSettings.animate; onToggled: readingSettings.animate = checked }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Page-turn sound")
            description: qsTr("Subtle paper-flip sound when turning pages; pauses automatically while listening.")
            Switch {
                checked: page.pageTurnSoundOn
                onToggled: {
                    if (page.hasPageTurnSoundProp)
                        readingSettings.pageTurnSound = checked
                    else
                        page.fallbackPageTurnSound = checked
                    if (checked && typeof reader.playPageTurn === "function")
                        reader.playPageTurn(1)
                }
            }
        }
    }

    SettingsCard {
        title: qsTr("PDFs")
        glyph: Icons.crop

        SettingsRow {
            title: qsTr("Crop white margins")
            description: qsTr("Trim empty outer borders on PDF pages so text fills more of the window.")
            Switch { checked: readingSettings.pdfCrop; onToggled: readingSettings.pdfCrop = checked }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Keep original colours")
            description: qsTr("Show PDF pages in their original colours even in dark reading themes.")
            Switch { checked: readingSettings.pdfOriginalColors; onToggled: readingSettings.pdfOriginalColors = checked }
        }
    }

    SettingsCard {
        SettingsRow {
            title: qsTr("Reset reading preferences")
            description: qsTr("Restore Paper theme, Literata typeface, normal margins, and default layout.")
            TextButton {
                size: "compact"
                text: page.confirmingReset ? qsTr("Reset defaults?") : qsTr("Reset defaults")
                onClicked: {
                    if (!page.confirmingReset) {
                        page.confirmingReset = true
                        return
                    }
                    page.confirmingReset = false
                    page.resetDefaults()
                }
            }
        }
    }
}
