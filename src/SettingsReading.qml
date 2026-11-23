import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Themes.js" as Themes
import "Ui.js" as Ui

// Settings › Reading: how every book looks unless it has its own look
// (Appearance, A, while reading changes the same values for the open book).
ColumnLayout {
    id: page

    spacing: 16

    readonly property var fonts: {
        const installed = Qt.fontFamilies()
        const wanted = ["Literata", "Noto Serif", "Charis SIL", "EB Garamond", "Crimson Pro", "Source Serif 4",
                        "C059", "Liberation Serif", "Merriweather", "Atkinson Hyperlegible", "iA Writer Quattro S",
                        "Adwaita Sans", "Noto Sans"]
        return wanted.filter(name => name === "Literata" || installed.indexOf(name) >= 0)
    }

    SettingsCard {
        title: qsTr("Theme")
        description: qsTr("The page's colours. Match desktop wears your Omarchy theme.")
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
                    width: 96
                    height: 72
                    hoverEnabled: true
                    focusPolicy: Qt.NoFocus
                    Accessible.name: Themes.name(modelData)
                    Accessible.checked: chosen
                    onClicked: readingSettings.theme = modelData
                    background: Rectangle {
                        radius: Math.min(10, theme.radius)
                        color: swatch.look.bg
                        border.width: swatch.chosen ? 2 : 1
                        border.color: swatch.chosen ? theme.accent : Ui.tint(swatch.look.fg, 0.16)
                    }
                    contentItem: Column {
                        topPadding: 12
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
                            color: Ui.tint(swatch.look.fg, 0.75)
                            font.pixelSize: 11
                        }
                    }
                }
            }
        }
    }

    SettingsCard {
        title: qsTr("Type")
        SettingsRow {
            title: qsTr("Typeface")
            ComboBox {
                width: 220
                model: [qsTr("The book's own")].concat(page.fonts)
                currentIndex: readingSettings.publisher ? 0 : Math.max(1, page.fonts.indexOf(readingSettings.font) + 1)
                onActivated: (index) => {
                    if (index === 0) {
                        readingSettings.publisher = true
                    } else {
                        readingSettings.publisher = false
                        readingSettings.font = page.fonts[index - 1]
                    }
                }
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: theme.hairline }
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
        Rectangle { Layout.fillWidth: true; height: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Line spacing")
            description: Number(readingSettings.lineHeight).toLocaleString(Qt.locale(), 'f', 2)
            Slider {
                width: 220
                from: 1.2; to: 2.0; stepSize: 0.05
                value: readingSettings.lineHeight
                onMoved: readingSettings.lineHeight = Math.round(value * 100) / 100
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Justify")
            description: qsTr("Straight right edges, with hyphenation to keep spacing even.")
            Switch { checked: readingSettings.justify; onToggled: readingSettings.justify = checked }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Hyphenate")
            Switch { checked: readingSettings.hyphenate; onToggled: readingSettings.hyphenate = checked }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Drop caps")
            description: qsTr("A large initial opens each chapter.")
            Switch { checked: readingSettings.dropCap; onToggled: readingSettings.dropCap = checked }
        }
    }

    SettingsCard {
        title: qsTr("Pages")
        SettingsRow {
            title: qsTr("Layout")
            Segmented {
                model: [{ value: "auto", label: qsTr("Auto") }, { value: "single", label: qsTr("One page") },
                        { value: "double", label: qsTr("Two pages") }, { value: "scroll", label: qsTr("Scroll") }]
                current: readingSettings.layout
                onActivated: (value) => readingSettings.layout = value
            }
        }
        Rectangle { Layout.fillWidth: true; height: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Animate page turns")
            Switch { checked: readingSettings.animate; onToggled: readingSettings.animate = checked }
        }
    }
}
