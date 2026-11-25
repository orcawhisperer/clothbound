import QtCore
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "../components"

import "../components/Icons.js" as Icons

// Settings › General: appearance, language, library preferences, folders, and
// full-text / semantic search indexes.
ColumnLayout {
    id: page

    spacing: 16

    Settings {
        id: appearance
        location: settingsUrl
        category: "appearance"
        property string mode: "match"
        property string corners: "match"
        property bool desktopFont: false
        property bool reducedMotion: false
    }

    property bool initialDesktopFont: false
    property int folderVersion: 0
    readonly property bool fontChanged: appearance.desktopFont !== initialDesktopFont
    readonly property bool canSwitchLanguage: typeof translator.setLanguage === "function"
    readonly property string languageOverride: typeof translator.languageOverride !== "undefined"
                                             ? String(translator.languageOverride || "") : ""
    readonly property var folderList: folderVersion >= 0 ? library.folderStats() : []

    // Accurate native display names for all 50 shipped locales (distinguishing
    // regional variants Qt.locale().nativeLanguageName collapses).
    readonly property var locales: [
        { code: "ar",    name: "العربية" },
        { code: "bg",    name: "Български" },
        { code: "bn",    name: "বাংলা" },
        { code: "ca",    name: "Català" },
        { code: "cs",    name: "Čeština" },
        { code: "da",    name: "Dansk" },
        { code: "de",    name: "Deutsch" },
        { code: "el",    name: "Ελληνικά" },
        { code: "en",    name: "English" },
        { code: "es-MX", name: "Español (México)" },
        { code: "et",    name: "Eesti" },
        { code: "fa",    name: "فارسی" },
        { code: "fi",    name: "Suomi" },
        { code: "fil",   name: "Filipino" },
        { code: "fr",    name: "Français" },
        { code: "ga",    name: "Gaeilge" },
        { code: "he",    name: "עברית" },
        { code: "hi",    name: "हिन्दी" },
        { code: "hr",    name: "Hrvatski" },
        { code: "hu",    name: "Magyar" },
        { code: "id",    name: "Bahasa Indonesia" },
        { code: "is",    name: "Íslenska" },
        { code: "it",    name: "Italiano" },
        { code: "ja",    name: "日本語" },
        { code: "ko",    name: "한국어" },
        { code: "lt",    name: "Lietuvių" },
        { code: "lv",    name: "Latviešu" },
        { code: "ms",    name: "Bahasa Melayu" },
        { code: "nl",    name: "Nederlands" },
        { code: "no",    name: "Norsk" },
        { code: "pl",    name: "Polski" },
        { code: "pt",    name: "Português" },
        { code: "pt-BR", name: "Português (Brasil)" },
        { code: "ro",    name: "Română" },
        { code: "ru",    name: "Русский" },
        { code: "si",    name: "සිංහල" },
        { code: "sk",    name: "Slovenčina" },
        { code: "sl",    name: "Slovenščina" },
        { code: "sr",    name: "Српски" },
        { code: "sv",    name: "Svenska" },
        { code: "sw",    name: "Kiswahili" },
        { code: "ta",    name: "தமிழ்" },
        { code: "th",    name: "ไทย" },
        { code: "tr",    name: "Türkçe" },
        { code: "uk",    name: "Українська" },
        { code: "ur",    name: "اردو" },
        { code: "uz",    name: "Oʻzbekcha" },
        { code: "vi",    name: "Tiếng Việt" },
        { code: "zh-CN", name: "简体中文" },
        { code: "zh-TW", name: "繁體中文" }
    ]

    function languageLabel(code) {
        const clean = String(code || "").replace("_", "-")
        for (let i = 0; i < locales.length; ++i)
            if (locales[i].code.toLowerCase() === clean.toLowerCase())
                return locales[i].name
        const base = clean.split("-")[0]
        for (let j = 0; j < locales.length; ++j)
            if (locales[j].code.toLowerCase() === base.toLowerCase())
                return locales[j].name
        return Qt.locale(clean).nativeLanguageName || clean
    }

    readonly property var sortOptions: [
        { value: "recent",   label: qsTr("Recently read") },
        { value: "title",    label: qsTr("Title") },
        { value: "author",   label: qsTr("Author") },
        { value: "series",   label: qsTr("Series") },
        { value: "added",    label: qsTr("Date added") },
        { value: "progress", label: qsTr("Progress") }
    ]

    Component.onCompleted: {
        initialDesktopFont = appearance.desktopFont
        if (appearance.corners !== theme.corners)
            theme.corners = appearance.corners
        if (appearance.reducedMotion && !theme.reducedMotion)
            theme.reducedMotion = true
    }

    Connections {
        target: library
        function onFoldersChanged() { page.folderVersion++ }
        function onBooksChanged() { page.folderVersion++ }
    }

    SettingsCard {
        title: qsTr("Appearance")
        glyph: Icons.sun

        SettingsRow {
            title: qsTr("Light or dark")
            description: theme.omarchy || appearance.mode !== "match"
                         ? qsTr("Match desktop follows your Omarchy theme as it changes.")
                         : qsTr("Match desktop follows your Omarchy theme as it changes; without one, Clothbound is dark.")
            Segmented {
                model: [{ value: "match", label: qsTr("Match desktop") },
                        { value: "dark", label: qsTr("Dark") },
                        { value: "light", label: qsTr("Light") }]
                current: theme.appearance
                onActivated: (value) => { theme.appearance = value; appearance.mode = value }
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Corners")
            description: qsTr("Match desktop follows Hyprland's window rounding. Override to Rounded or Sharp for Clothbound.")
            Segmented {
                model: [{ value: "match", label: qsTr("Match desktop") },
                        { value: "rounded", label: qsTr("Rounded") },
                        { value: "sharp", label: qsTr("Sharp") }]
                current: theme.corners
                onActivated: (value) => { theme.corners = value; appearance.corners = value }
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Use the desktop's font")
            description: page.fontChanged ? qsTr("Takes effect the next time Clothbound starts.")
                                          : qsTr("Clothbound's interface is set in Inter. Books keep their own typeface.")
            Switch {
                checked: appearance.desktopFont
                onToggled: appearance.desktopFont = checked
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Reduce motion")
            description: qsTr("Replace sliding and fading transitions with quick, minimal motion across the app.")
            Switch {
                checked: theme.reducedMotion
                onToggled: {
                    theme.reducedMotion = checked
                    appearance.reducedMotion = checked
                }
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Language")
            description: page.canSwitchLanguage
                         ? (page.languageOverride === ""
                            ? qsTr("Following your desktop language (%1). Choose any language below to switch immediately.").arg(page.languageLabel(translator.language))
                            : qsTr("Using %1 instead of your desktop language. Switch back to Follow desktop any time.").arg(page.languageLabel(translator.language)))
                         : qsTr("Clothbound speaks the language chosen for Omarchy in Setup › Language, and follows it when it changes.")
            ComboBox {
                visible: page.canSwitchLanguage
                width: 220
                model: [qsTr("Follow desktop")].concat(page.locales.map(l => l.name))
                currentIndex: {
                    if (page.languageOverride === "")
                        return 0
                    const idx = page.locales.findIndex(l => l.code.toLowerCase() === page.languageOverride.toLowerCase())
                    return idx >= 0 ? idx + 1 : 0
                }
                onActivated: (index) => {
                    if (!page.canSwitchLanguage)
                        return
                    const nextCode = index === 0 ? "" : page.locales[index - 1].code
                    translator.setLanguage(nextCode)
                    toast.show(nextCode === ""
                               ? qsTr("Following desktop language")
                               : qsTr("Language set to %1").arg(page.languageLabel(nextCode)))
                }
            }
            Row {
                visible: !page.canSwitchLanguage
                spacing: 8
                Label {
                    anchors.verticalCenter: parent.verticalCenter
                    text: page.languageLabel(translator.language)
                    color: theme.ink1
                    font.pixelSize: 13
                    font.weight: Font.Medium
                }
                Badge {
                    anchors.verticalCenter: parent.verticalCenter
                    where: "none"
                    text: translator.language
                }
            }
        }
    }

    SettingsCard {
        title: qsTr("Library")
        glyph: Icons.books
        description: qsTr("How your library is arranged. Changes apply immediately and stay saved.")

        SettingsRow {
            title: qsTr("Default view")
            Segmented {
                model: [{ value: "grid", label: qsTr("Covers") },
                        { value: "list", label: qsTr("List") }]
                current: libraryView.viewMode
                onActivated: (value) => libraryView.viewMode = value
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Sort books by")
            ComboBox {
                width: 180
                model: page.sortOptions.map(o => o.label)
                currentIndex: Math.max(0, page.sortOptions.findIndex(o => o.value === library.works.sortOrder))
                onActivated: (index) => library.works.sortOrder = page.sortOptions[index].value
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Group editions")
            description: qsTr("Combine multiple formats or editions of the same book into one shelf entry.")
            Switch {
                checked: library.works.grouped
                onToggled: library.works.grouped = checked
            }
        }
        Rectangle { Layout.fillWidth: true; Layout.preferredHeight: 1; color: theme.hairline }
        SettingsRow {
            title: qsTr("Show library sidebar")
            description: qsTr("Keep shelves, collections, and reading statistics visible alongside your books.")
            Switch {
                checked: !libraryView.sidebarCollapsed
                onToggled: libraryView.sidebarCollapsed = !checked
            }
        }
    }

    SettingsCard {
        title: qsTr("Library folders")
        glyph: Icons.folder
        description: qsTr("Books in these folders are read where they are, never changed, and new ones are added each time Clothbound starts.")

        Repeater {
            model: page.folderList
            delegate: ColumnLayout {
                id: folderRow
                required property var modelData
                required property int index
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    visible: folderRow.index > 0
                    Layout.fillWidth: true
                    Layout.preferredHeight: 1
                    color: theme.hairline
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 10
                    Glyph {
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        path: Icons.folder
                        color: folderRow.modelData.exists ? theme.ink3 : theme.danger
                    }
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 1
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            Label {
                                Layout.maximumWidth: 260
                                text: folderRow.modelData.name
                                textFormat: Text.PlainText
                                color: folderRow.modelData.exists ? theme.ink1 : theme.danger
                                font.pixelSize: 13
                                font.weight: Font.Medium
                                elide: Text.ElideRight
                            }
                            Badge {
                                visible: !folderRow.modelData.exists
                                where: "cloud"
                                text: qsTr("Unavailable")
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: folderRow.modelData.path.replace(/^\/home\/[^/]+/, "~")
                                  + " · " + qsTr("%n books", "", folderRow.modelData.books)
                            textFormat: Text.PlainText
                            color: theme.ink3
                            font.pixelSize: 11
                            elide: Text.ElideMiddle
                        }
                    }
                    TextButton {
                        size: "compact"
                        ghost: true
                        visible: folderRow.modelData.exists
                        text: qsTr("Open")
                        onClicked: library.openFolder(folderRow.modelData.path)
                    }
                    TextButton {
                        size: "compact"
                        text: qsTr("Remove")
                        onClicked: {
                            const removedPath = folderRow.modelData.path
                            const removedName = folderRow.modelData.name
                            library.removeFolder(removedPath)
                            toast.show(qsTr("Removed %1").arg(removedName), () => library.addFolder(removedPath))
                        }
                    }
                }
            }
        }

        Label {
            visible: page.folderList.length === 0
            Layout.fillWidth: true
            text: qsTr("No folders watched yet. Add a folder of EPUB or PDF books, or add individual book files.")
            color: theme.ink3
            font.pixelSize: 13
            wrapMode: Text.Wrap
            lineHeight: 1.35
        }

        ColumnLayout {
            visible: library.scanning
            Layout.fillWidth: true
            spacing: 6
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                BusyIndicator {
                    running: library.scanning
                    implicitWidth: 14
                    implicitHeight: 14
                }
                Label {
                    Layout.fillWidth: true
                    text: library.scanTotal > 0
                          ? qsTr("Scanning library · %1 of %2").arg(library.scanDone).arg(library.scanTotal)
                          : qsTr("Looking for books…")
                    color: theme.ink2
                    font.pixelSize: 12
                }
                TextButton {
                    size: "compact"
                    text: qsTr("Stop")
                    onClicked: library.cancelScan()
                }
            }
            ProgressBar {
                Layout.fillWidth: true
                indeterminate: library.scanTotal <= 0
                value: library.scanTotal > 0 ? library.scanDone / library.scanTotal : 0
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            TextButton {
                size: "compact"
                primary: page.folderList.length === 0
                text: qsTr("Add a folder…")
                glyph: Icons.plus
                onClicked: picker.chooseFolders()
            }
            TextButton {
                size: "compact"
                text: qsTr("Add books…")
                onClicked: picker.chooseBooks()
            }
            Item { Layout.fillWidth: true }
            TextButton {
                size: "compact"
                visible: page.folderList.length > 0 || library.bookCount > 0
                text: library.scanning ? qsTr("Looking…") : qsTr("Look for new books")
                enabled: !library.scanning
                onClicked: library.rescan()
            }
        }
    }

    SettingsCard {
        title: qsTr("Search inside books")
        glyph: Icons.search
        description: libraryIndex.available
                     ? qsTr("Every book's words, kept on this computer so the palette (Ctrl+K) finds a passage in any book.")
                     : qsTr("This computer's SQLite has no full-text search (FTS5), so the palette searches titles, authors and notes only.")

        SettingsRow {
            visible: libraryIndex.available
            title: libraryIndex.indexing
                   ? qsTr("Reading books in · %1 of %2").arg(libraryIndex.done.toLocaleString(Qt.locale(), 'f', 0))
                                                         .arg(libraryIndex.total.toLocaleString(Qt.locale(), 'f', 0))
                   : qsTr("%n books searchable", "", libraryIndex.bookCount)
            description: libraryIndex.bookCount > 0
                         ? qsTr("%1 MB, in the cache; it can always be built again.")
                               .arg(Math.max(0.1, libraryIndex.megabytes).toLocaleString(Qt.locale(), 'f', 1))
                         : qsTr("The index builds automatically as soon as books are in your library.")
            TextButton {
                size: "compact"
                text: qsTr("Rebuild")
                enabled: !libraryIndex.indexing && library.bookCount > 0
                onClicked: libraryIndex.rebuild()
            }
        }
        ProgressBar {
            visible: libraryIndex.available && libraryIndex.indexing
            Layout.fillWidth: true
            indeterminate: libraryIndex.total <= 0
            value: libraryIndex.total > 0 ? libraryIndex.done / libraryIndex.total : 0
        }
        Rectangle {
            visible: libraryIndex.available
            Layout.fillWidth: true
            Layout.preferredHeight: 1
            color: theme.hairline
        }
        SettingsRow {
            title: qsTr("By meaning")
            description: !bookSearch.canSearchByMeaning
                         ? qsTr("Needs a search model, from Gemini or on this computer (Settings › Intelligence).")
                         : qsTr("A book is prepared the first time you search inside it by meaning (/ while reading); prepared books are searched together from the palette.")
            Label {
                visible: bookSearch.canSearchByMeaning
                anchors.verticalCenter: parent.verticalCenter
                text: qsTr("%n books prepared", "", bookSearch.booksPrepared)
                color: theme.ink2
                font.pixelSize: 13
            }
            TextButton {
                visible: !bookSearch.canSearchByMeaning
                size: "compact"
                text: qsTr("Set up…")
                onClicked: settingsView.show("intelligence")
            }
        }
    }

    SettingsCard {
        title: qsTr("Guide & updates")
        glyph: Icons.bookOpen
        description: qsTr("Clothbound %1. Revisit the welcome tour or see what changed in this version.").arg(Qt.application.version)
        RowLayout {
            Layout.fillWidth: true
            spacing: 8
            TextButton {
                size: "compact"
                text: qsTr("Show what's new")
                onClicked: welcome.show("whatsnew")
            }
            TextButton {
                size: "compact"
                ghost: true
                text: qsTr("Show welcome guide")
                onClicked: welcome.show("welcome")
            }
        }
    }
}
