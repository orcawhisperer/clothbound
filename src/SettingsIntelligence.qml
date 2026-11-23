import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import "Icons.js" as Icons
import "Ui.js" as Ui

// Settings › Intelligence (06_intelligence): responsive two-column workspace.
// Left: 3-card "Where it runs" preset picker + 7-row "Feature by feature"
// table (with expandable 9-feature advanced routing). Right: compact Google
// Gemini (key + monthly budget sheet) and This computer (WhereItRuns) cards.
ColumnLayout {
    id: page

    spacing: 20

    property bool replacing: false
    property bool advancedRouting: false
    readonly property bool askingForKey: !ai.hasKey || replacing
    readonly property bool twoColumns: width >= 760
    readonly property bool budgetOpen: budgetSheet.open

    function closeBudget() { budgetSheet.close() }
    onVisibleChanged: if (!visible && budgetSheet.open) budgetSheet.close()

    // Called when the page is shown: fresh status for both sides.
    function prepare() {
        keyInput.text = ""
        replacing = false
        if (ai.hasKey && !ai.connected && !ai.checking)
            ai.check()
        if (!ai.localServers.length && !ai.findingLocal)
            ai.findLocal()
        localSetup.refresh()
        systemCheck.refresh()
    }
    function save() {
        if (keyInput.text.trim() !== "")
            ai.setKey(keyInput.text)
    }
    function money(value) { return Number(value).toLocaleCurrencyString(Qt.locale("en_US"), "$") }
    function formatCents(dollars) {
        const value = Number(dollars) || 0
        if (value < 0.01)
            return qsTr("less than 1¢")
        if (value < 1.0)
            return qsTr("%1¢").arg(Math.max(1, Math.round(value * 100)))
        return money(value)
    }
    // Resolve a grouped feature row's effective route ("local", "cloud", "mixed", or "none").
    function groupRoute(ids) {
        void(ai.overrideCount); void(ai.preset); void(ai.hasKey); void(ai.hasLocal)
        void(ai.localUrl); void(ai.localEmbeddingModel)
        if (!ids || ids.length === 0)
            return ai.preset === "local" ? "local" : (ai.hasKey ? "cloud" : "local")
        let seenLocal = false
        let seenCloud = false
        let seenNone = false
        for (let i = 0; i < ids.length; ++i) {
            const r = ai.routeFor(ids[i])
            if (r === "local") seenLocal = true
            else if (r === "cloud") seenCloud = true
            else seenNone = true
        }
        const kinds = (seenLocal ? 1 : 0) + (seenCloud ? 1 : 0) + (seenNone ? 1 : 0)
        if (kinds > 1)
            return "mixed"
        if (seenLocal)
            return ai.localIsLoopback ? "local" : "cloud"
        if (seenCloud)
            return "cloud"
        return "none"
    }
    function groupOverride(ids) {
        void(ai.overrideCount); void(ai.preset)
        if (!ids || ids.length === 0)
            return ""
        const first = ai.overrideFor(ids[0])
        for (let i = 1; i < ids.length; ++i)
            if (ai.overrideFor(ids[i]) !== first)
                return "mixed"
        return first
    }
    function setGroupOverride(ids, route) {
        for (let i = 0; i < ids.length; ++i)
            ai.setOverride(ids[i], route)
    }

    Connections {
        target: ai
        function onKeyChecked(ok, message) {
            if (ok) {
                page.replacing = false
                keyInput.text = ""
            }
        }
    }

    component PresetCard: AbstractButton {
        id: presetCard
        property string title
        property string description
        property bool recommended: false

        checkable: true
        autoExclusive: true
        hoverEnabled: true
        focusPolicy: Qt.TabFocus
        implicitHeight: presetContent.implicitHeight + 28
        Accessible.role: Accessible.RadioButton
        Accessible.name: title
        Accessible.description: description
        Accessible.checked: checked
        HoverHandler { cursorShape: Qt.PointingHandCursor }

        background: Rectangle {
            radius: Ui.radius(10, theme.radius)
            color: presetCard.checked ? Ui.tint(theme.accent, 0.10)
                 : presetCard.hovered ? Ui.tint(theme.ink1, 0.05) : Ui.tint(theme.ink1, 0.03)
            border.width: 1
            border.color: presetCard.checked ? Ui.tint(theme.accent, 0.60) : Ui.tint(theme.ink1, 0.08)
            Behavior on color { ColorAnimation { duration: 100 } }

            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                visible: presetCard.visualFocus
                radius: Ui.radius(13, theme.radius)
                color: "transparent"
                border.width: 2
                border.color: theme.accent
            }
        }

        contentItem: ColumnLayout {
            id: presetContent
            anchors.fill: parent
            anchors.margins: 14
            spacing: 6

            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Rectangle {
                    Layout.preferredWidth: 16
                    Layout.preferredHeight: 16
                    radius: 8
                    color: "transparent"
                    border.width: presetCard.checked ? 5 : 1.5
                    border.color: presetCard.checked ? theme.accent : theme.ink4
                }
                Label {
                    Layout.fillWidth: true
                    text: presetCard.title
                    color: theme.ink1
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                    elide: Text.ElideRight
                }
                Rectangle {
                    visible: presetCard.recommended
                    implicitHeight: 18
                    implicitWidth: recLabel.implicitWidth + 12
                    radius: 9
                    color: theme.accentSoft
                    Label {
                        id: recLabel
                        anchors.centerIn: parent
                        text: qsTr("Recommended")
                        color: theme.accentInk
                        font.pixelSize: 10
                        font.weight: Font.DemiBold
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                text: presetCard.description
                color: theme.ink3
                font.pixelSize: 12
                lineHeight: 1.45
                wrapMode: Text.Wrap
            }
        }
    }

    // --- Main two-column (or stacked) layout -------------------------------
    GridLayout {
        Layout.fillWidth: true
        columns: page.twoColumns ? 2 : 1
        columnSpacing: 24
        rowSpacing: 22

        // --- Left column: Where it runs + Feature by feature ---------------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignTop
            spacing: 22

            // 1. Where it runs (3 radio cards)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10

                Label {
                    text: qsTr("Where it runs")
                    color: theme.ink2
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }

                ButtonGroup { id: presetGroup }
                GridLayout {
                    Layout.fillWidth: true
                    columns: page.width >= 520 ? 3 : 1
                    columnSpacing: 10
                    rowSpacing: 10

                    PresetCard {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        ButtonGroup.group: presetGroup
                        title: qsTr("Cloud")
                        description: qsTr("Gemini does everything. Best quality; costs a little.")
                        checked: ai.preset === "cloud"
                        onClicked: ai.preset = "cloud"
                    }
                    PresetCard {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        ButtonGroup.group: presetGroup
                        title: qsTr("Hybrid")
                        description: qsTr("Quick things here, careful things with Gemini. Offline falls back here.")
                        recommended: true
                        checked: ai.preset === "hybrid"
                        onClicked: ai.preset = "hybrid"
                    }
                    PresetCard {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        ButtonGroup.group: presetGroup
                        title: qsTr("This computer")
                        description: qsTr("Nothing leaves this computer. Some features wait for a bigger one.")
                        checked: ai.preset === "local"
                        onClicked: ai.preset = "local"
                    }
                }
            }

            // 2. Feature by feature (7-row table + legend + expandable 9-feature overrides)
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 10

                Label {
                    text: qsTr("Feature by feature")
                    color: theme.ink2
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }

                Rectangle {
                    Layout.fillWidth: true
                    implicitHeight: featureTableCol.implicitHeight
                    radius: Ui.radius(10, theme.radius)
                    color: Ui.tint(theme.ink1, 0.03)
                    border.width: 1
                    border.color: Ui.tint(theme.ink1, 0.07)

                    ColumnLayout {
                        id: featureTableCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        spacing: 0

                        readonly property var rows: [
                            {
                                name: qsTr("Narration"),
                                note: qsTr("Falls back to a voice here when offline"),
                                ids: [],
                                editable: false
                            },
                            {
                                name: qsTr("Scene moods"),
                                note: qsTr("How the narrator reads each scene"),
                                ids: ["director", "cast"],
                                editable: true
                            },
                            {
                                name: qsTr("Define, translate, who's who"),
                                note: qsTr("Short answers"),
                                ids: ["define", "translate", "whoswho"],
                                editable: true
                            },
                            {
                                name: qsTr("Explain, Previously…"),
                                note: qsTr("Careful answers"),
                                ids: ["explain", "recap"],
                                editable: true
                            },
                            {
                                name: qsTr("Ask the book"),
                                note: ai.preset === "local"
                                      ? (ai.hasLocal ? qsTr("Matched passages here · 32 GB for whole book") : qsTr("Needs 32 GB here for whole book"))
                                      : qsTr("Reads the book up to your page"),
                                ids: ["ask"],
                                editable: true
                            },
                            {
                                name: qsTr("Search by meaning"),
                                note: qsTr("Finds passages, not just words"),
                                ids: ["search"],
                                editable: true
                            },
                            {
                                name: qsTr("Pictures"),
                                note: ai.routeFor("illustrate") === ""
                                      ? qsTr("Needs a large graphics card here")
                                      : qsTr("About %1 each").arg(page.formatCents(illustrator.cost || 0.03)),
                                ids: ["illustrate"],
                                editable: ai.hasKey,
                                cloudOnly: true
                            }
                        ]

                        Repeater {
                            model: featureTableCol.rows
                            delegate: ColumnLayout {
                                id: featRow
                                required property var modelData
                                required property int index
                                readonly property string route: page.groupRoute(modelData.ids)
                                readonly property string currentOverride: page.groupOverride(modelData.ids)
                                Layout.fillWidth: true
                                spacing: 0

                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.minimumHeight: 46
                                    Layout.leftMargin: 14
                                    Layout.rightMargin: 12
                                    Layout.topMargin: 6
                                    Layout.bottomMargin: 6
                                    spacing: 12

                                    Label {
                                        Layout.preferredWidth: 170
                                        text: featRow.modelData.name
                                        color: theme.ink1
                                        font.pixelSize: 13
                                        font.weight: Font.Medium
                                        wrapMode: Text.Wrap
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: featRow.modelData.note
                                        color: theme.ink4
                                        font.pixelSize: 12
                                        wrapMode: Text.Wrap
                                    }
                                    ProviderPill {
                                        id: routePill
                                        showDot: true
                                        interactive: !!featRow.modelData.editable
                                        where: featRow.route === "local" ? "local"
                                             : featRow.route === "cloud" ? "cloud"
                                             : "none"
                                        text: featRow.route === "mixed" ? qsTr("Mixed")
                                            : featRow.route === "local" ? qsTr("This computer")
                                            : featRow.route === "cloud" ? qsTr("Gemini")
                                            : qsTr("Unavailable")
                                        onClicked: if (featRow.modelData.editable) routeMenu.popup(routePill, 0, routePill.height + 4)

                                        Menu {
                                            id: routeMenu
                                            MenuItem {
                                                text: qsTr("Follow preset")
                                                checkable: true
                                                checked: featRow.currentOverride === ""
                                                onTriggered: page.setGroupOverride(featRow.modelData.ids, "")
                                            }
                                            MenuItem {
                                                text: qsTr("Gemini")
                                                checkable: true
                                                checked: featRow.currentOverride === "cloud"
                                                onTriggered: page.setGroupOverride(featRow.modelData.ids, "cloud")
                                            }
                                            MenuItem {
                                                text: featRow.modelData.cloudOnly ? qsTr("Off (local only)") : qsTr("This computer")
                                                checkable: true
                                                checked: featRow.currentOverride === "local"
                                                onTriggered: page.setGroupOverride(featRow.modelData.ids, "local")
                                            }
                                        }
                                    }
                                }

                                Rectangle {
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 1
                                    visible: featRow.index < featureTableCol.rows.length - 1
                                    color: Ui.tint(theme.ink1, 0.05)
                                }
                            }
                        }
                    }
                }

                // Legend directly below table.
                Flow {
                    Layout.fillWidth: true
                    spacing: 14
                    Row {
                        spacing: 6
                        Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 6; height: 6; radius: 3; color: theme.positive }
                        Label { text: qsTr("On this computer, nothing leaves it"); color: theme.ink4; font.pixelSize: 12 }
                    }
                    Row {
                        spacing: 6
                        Rectangle { anchors.verticalCenter: parent.verticalCenter; width: 6; height: 6; radius: 3; color: theme.cloud }
                        Label { text: qsTr("Sent to Google, with your key"); color: theme.ink4; font.pixelSize: 12 }
                    }
                }

                // Expandable granular 9-feature routing & Reset to preset.
                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    TextButton {
                        size: "compact"
                        ghost: true
                        text: page.advancedRouting ? qsTr("Hide per-feature overrides")
                            : ai.overrideCount > 0 ? qsTr("Per-feature routing (%n custom)", "", ai.overrideCount)
                            : qsTr("Advanced per-feature routing…")
                        onClicked: page.advancedRouting = !page.advancedRouting
                    }
                    Item { Layout.fillWidth: true }
                    TextButton {
                        size: "compact"
                        visible: ai.overrideCount > 0
                        text: qsTr("Reset to preset")
                        onClicked: ai.clearOverrides()
                    }
                }

                Rectangle {
                    visible: page.advancedRouting
                    Layout.fillWidth: true
                    implicitHeight: advCol.implicitHeight + 16
                    radius: Ui.radius(10, theme.radius)
                    color: Ui.tint(theme.ink1, 0.025)
                    border.width: 1
                    border.color: Ui.tint(theme.ink1, 0.07)

                    ColumnLayout {
                        id: advCol
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.margins: 12
                        spacing: 8

                        Repeater {
                            model: [
                                { id: "ask",       label: qsTr("Ask about the book") },
                                { id: "explain",   label: qsTr("Explain a passage") },
                                { id: "recap",     label: qsTr("Previously… recaps") },
                                { id: "whoswho",   label: qsTr("Who's who") },
                                { id: "define",    label: qsTr("Define a word") },
                                { id: "translate", label: qsTr("Translate") },
                                { id: "search",    label: qsTr("Search by meaning") },
                                { id: "director",  label: qsTr("Narration styles") },
                                { id: "cast",      label: qsTr("Full cast") }
                            ]
                            delegate: RowLayout {
                                id: advRow
                                required property var modelData
                                readonly property string route: {
                                    void(ai.overrideCount); void(ai.preset); void(ai.hasKey); void(ai.hasLocal)
                                    void(ai.localUrl); void(ai.localEmbeddingModel)
                                    return ai.routeFor(modelData.id)
                                }
                                readonly property string override: {
                                    void(ai.overrideCount); void(ai.preset)
                                    return ai.overrideFor(modelData.id)
                                }
                                Layout.fillWidth: true
                                spacing: 10
                                Label {
                                    Layout.fillWidth: true
                                    text: advRow.modelData.label
                                    color: theme.ink2
                                    font.pixelSize: 12
                                    elide: Text.ElideRight
                                }
                                Badge {
                                    showDot: true
                                    where: advRow.route === "local" && ai.localIsLoopback ? "local"
                                         : advRow.route === "" ? "none" : "cloud"
                                    text: advRow.route === "" ? qsTr("Unavailable")
                                        : advRow.route === "local" && ai.localIsLoopback ? qsTr("This computer")
                                        : qsTr("Gemini")
                                }
                                ComboBox {
                                    Layout.preferredWidth: 104
                                    model: [qsTr("Auto"), qsTr("Gemini"), qsTr("Local")]
                                    currentIndex: advRow.override === "cloud" ? 1 : advRow.override === "local" ? 2 : 0
                                    onActivated: (index) => ai.setOverride(advRow.modelData.id, ["", "cloud", "local"][index])
                                }
                            }
                        }
                    }
                }
            }
        }

        // --- Right column (330 px): Google Gemini + This computer ----------
        ColumnLayout {
            Layout.preferredWidth: page.twoColumns ? 330 : -1
            Layout.minimumWidth: page.twoColumns ? 330 : 0
            Layout.maximumWidth: page.twoColumns ? 330 : 10000
            Layout.fillWidth: !page.twoColumns
            Layout.alignment: Qt.AlignTop
            spacing: 14

            // Card 1: Google Gemini
            Rectangle {
                Layout.fillWidth: true
                implicitHeight: geminiCol.implicitHeight + 32
                radius: Ui.radius(10, theme.radius)
                color: Ui.tint(theme.ink1, 0.03)
                border.width: 1
                border.color: Ui.tint(theme.ink1, 0.07)

                ColumnLayout {
                    id: geminiCol
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.top: parent.top
                    anchors.margins: 16
                    spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Label {
                            Layout.fillWidth: true
                            text: qsTr("Google Gemini")
                            color: theme.ink1
                            font.pixelSize: 13
                            font.weight: Font.DemiBold
                        }
                        BusyIndicator {
                            visible: ai.checking
                            running: visible
                            implicitWidth: 14
                            implicitHeight: 14
                        }
                        Row {
                            spacing: 6
                            Rectangle {
                                anchors.verticalCenter: parent.verticalCenter
                                width: 7
                                height: 7
                                radius: 3.5
                                color: ai.checking ? theme.cloud
                                     : ai.connected ? theme.positive
                                     : ai.hasKey ? theme.cloud : theme.ink4
                            }
                            Label {
                                anchors.verticalCenter: parent.verticalCenter
                                text: ai.checking ? qsTr("Checking…")
                                    : ai.connected ? qsTr("Connected")
                                    : ai.hasKey ? qsTr("Ready")
                                    : qsTr("Not connected")
                                color: theme.ink3
                                font.pixelSize: 12
                            }
                        }
                    }

                    Label {
                        Layout.fillWidth: true
                        text: !ai.hasKey ? qsTr("Add your own key from Google AI Studio. Kept in the system keyring, never in a file.")
                            : ai.keyFromEnvironment ? qsTr("Your key is in %1.").arg(ai.keyLocation)
                            : qsTr("Your key is in the system keyring, never in a file.")
                        color: theme.ink3
                        font.pixelSize: 12
                        wrapMode: Text.Wrap
                        lineHeight: 1.4
                    }

                    // Inline key input when unconfigured or replacing.
                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: page.askingForKey
                        spacing: 8

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: 6
                            TextField {
                                id: keyInput
                                Layout.fillWidth: true
                                echoMode: TextInput.Password
                                placeholderText: qsTr("Paste your Gemini API key")
                                Keys.onReturnPressed: page.save()
                                Keys.onEnterPressed: page.save()
                            }
                            TextButton {
                                size: "compact"
                                primary: true
                                text: qsTr("Save")
                                enabled: keyInput.text.trim() !== "" && !ai.checking
                                onClicked: page.save()
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            visible: ai.status !== "" && !ai.connected && !ai.checking && ai.status !== qsTr("No key yet") && ai.status !== qsTr("Ready")
                            text: ai.status
                            color: theme.danger
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
                        Label {
                            Layout.fillWidth: true
                            textFormat: Text.StyledText
                            text: qsTr("Get a key at <a href=\"https://aistudio.google.com/apikey\">aistudio.google.com/apikey</a>.")
                            color: theme.ink4
                            linkColor: theme.accentInk
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                            onLinkActivated: (link) => Qt.openUrlExternally(link)
                            HoverHandler { cursorShape: parent.hoveredLink ? Qt.PointingHandCursor : Qt.ArrowCursor }
                        }
                    }

                    // Monthly spending & 6px bar.
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 6

                        RowLayout {
                            Layout.fillWidth: true
                            Label {
                                Layout.fillWidth: true
                                text: qsTr("This month")
                                color: theme.ink1
                                font.pixelSize: 12
                            }
                            Label {
                                text: ai.unlimitedBudget
                                      ? qsTr("%1 · no limit").arg(page.money(ai.spentThisMonth))
                                      : ai.budget <= 0
                                      ? qsTr("%1 · paused ($0)").arg(page.money(ai.spentThisMonth))
                                      : qsTr("%1 of %2").arg(page.money(ai.spentThisMonth)).arg(page.money(ai.budget))
                                color: theme.ink3
                                font.pixelSize: 12
                                font.features: { "tnum": 1 }
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 6
                            radius: 3
                            color: Ui.tint(theme.ink1, 0.08)
                            Rectangle {
                                width: parent.width * (ai.unlimitedBudget ? 0
                                       : Math.min(1, ai.budget > 0 ? ai.spentThisMonth / ai.budget : (ai.spentThisMonth > 0 ? 1 : 0)))
                                height: parent.height
                                radius: 3
                                color: ai.budget <= 0 || (!ai.unlimitedBudget && ai.spentThisMonth > ai.budget * 0.8)
                                       ? theme.cloud : theme.accent
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: ai.unlimitedBudget
                                  ? qsTr("No monthly cap is set.")
                                  : ai.budget <= 0
                                  ? qsTr("Cloud spending is paused ($0 budget).")
                                  : qsTr("Dogear stops and asks before going over.")
                            color: theme.ink4
                            font.pixelSize: 11
                            wrapMode: Text.Wrap
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        TextButton {
                            size: "compact"
                            visible: ai.hasKey && !ai.keyFromEnvironment
                            text: page.replacing ? qsTr("Cancel") : qsTr("Change key")
                            onClicked: {
                                page.replacing = !page.replacing
                                if (page.replacing)
                                    keyInput.forceActiveFocus()
                                else
                                    keyInput.text = ""
                            }
                        }
                        TextButton {
                            size: "compact"
                            text: qsTr("Budget…")
                            onClicked: budgetSheet.show()
                        }
                        TextButton {
                            size: "compact"
                            ghost: true
                            visible: page.replacing && ai.hasKey && !ai.keyFromEnvironment
                            text: qsTr("Remove")
                            onClicked: {
                                page.replacing = false
                                ai.forgetKey()
                            }
                        }
                    }
                }
            }

            // Card 2: This computer
            WhereItRuns { Layout.fillWidth: true }
        }
    }

    // --- Privacy footnote --------------------------------------------------
    Label {
        Layout.fillWidth: true
        text: (ai.preset === "local" && ai.hasLocal && ai.localIsLoopback
               ? qsTr("Running on this computer with %1: nothing is sent anywhere.").arg(ai.localName)
               : ai.privacyNote)
              + " " + qsTr("Dogear sends only what a feature needs, and never text past where you're reading.")
        color: theme.ink4
        font.pixelSize: 12
        wrapMode: Text.Wrap
        lineHeight: 1.35
    }

    // --- Budget & Usage Sheet (preserves $0 strict budget, No limit & ledger) ---
    Sheet {
        id: budgetSheet
        parent: Overlay.overlay
        z: 80
        title: qsTr("Monthly budget")
        glyph: Icons.sparkle
        cardWidth: 460

        ColumnLayout {
            width: parent.width
            spacing: 14

            RowLayout {
                Layout.fillWidth: true
                spacing: 6
                Label {
                    Layout.fillWidth: true
                    text: ai.unlimitedBudget
                          ? qsTr("About %1 this month · no limit").arg(page.money(ai.spentThisMonth))
                          : ai.budget <= 0
                          ? qsTr("About %1 this month · cloud spending paused ($0)").arg(page.money(ai.spentThisMonth))
                          : qsTr("About %1 of your %2 budget").arg(page.money(ai.spentThisMonth)).arg(page.money(ai.budget))
                    color: theme.ink1
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    wrapMode: Text.Wrap
                }
                IconButton {
                    tip: qsTr("Lower the budget")
                    enabled: !ai.unlimitedBudget && ai.budget > 0
                    contentItem: Label {
                        text: "−"
                        color: theme.ink1
                        font.pixelSize: 17
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: ai.budget = Math.max(0, ai.budget - (ai.budget > 20 ? 10 : 5))
                }
                IconButton {
                    tip: qsTr("Raise the budget")
                    contentItem: Label {
                        text: "+"
                        color: theme.ink1
                        font.pixelSize: 17
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                    onClicked: {
                        if (ai.unlimitedBudget)
                            ai.unlimitedBudget = false
                        ai.budget = ai.budget + (ai.budget >= 20 ? 10 : 5)
                    }
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                TextButton {
                    size: "compact"
                    primary: !ai.unlimitedBudget && ai.budget <= 0
                    text: qsTr("Pause ($0)")
                    onClicked: {
                        ai.unlimitedBudget = false
                        ai.budget = 0
                    }
                }
                TextButton {
                    size: "compact"
                    primary: ai.unlimitedBudget
                    text: qsTr("No limit")
                    onClicked: ai.unlimitedBudget = !ai.unlimitedBudget
                }
            }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 6
                radius: 3
                visible: !ai.unlimitedBudget
                color: Ui.tint(theme.ink1, 0.08)
                Rectangle {
                    width: parent.width * Math.min(1, ai.budget > 0 ? ai.spentThisMonth / ai.budget : (ai.spentThisMonth > 0 ? 1 : 0))
                    height: parent.height
                    radius: 3
                    color: ai.budget <= 0 || ai.spentThisMonth > ai.budget * 0.8 ? theme.cloud : theme.accent
                }
            }

            Label {
                Layout.fillWidth: true
                visible: ai.connected && ai.textModel !== ""
                text: qsTr("Using %1 to understand and %2 to speak.").arg(ai.textModel).arg(ai.speechModel)
                color: theme.ink3
                font.pixelSize: 12
                wrapMode: Text.Wrap
            }

            Rectangle {
                visible: ai.usageThisMonth.length > 0
                Layout.fillWidth: true
                Layout.preferredHeight: 1
                color: theme.hairline
            }

            ColumnLayout {
                Layout.fillWidth: true
                visible: ai.usageThisMonth.length > 0
                spacing: 6
                Repeater {
                    model: ai.usageThisMonth
                    delegate: RowLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        Label {
                            Layout.fillWidth: true
                            text: {
                                const names = { listen: qsTr("Listening"), ask: qsTr("Asking"), explain: qsTr("Explaining"),
                                                define: qsTr("Defining"), translate: qsTr("Translating"), whoswho: qsTr("Who's who"),
                                                director: qsTr("Narration notes"), cast: qsTr("Full cast"), embed: qsTr("Search by meaning"),
                                                recap: qsTr("Recaps"), "voice design": qsTr("Designing voices"),
                                                voices: qsTr("Voice lists"), illustrate: qsTr("Illustrations") }
                                return names[modelData.feature] || modelData.feature
                            }
                            color: theme.ink2
                            font.pixelSize: 12
                        }
                        Label {
                            text: modelData.audioMinutes > 0.5
                                  ? qsTr("%1 · %n min of audio", "", Math.round(modelData.audioMinutes)).arg(page.money(modelData.cost))
                                  : qsTr("%1 · %n requests", "", modelData.requests).arg(page.money(modelData.cost))
                            color: theme.ink3
                            font.pixelSize: 12
                            font.features: { "tnum": 1 }
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                text: qsTr("Listening is about %1 an hour, or half that for books you download for offline. Asking about a book costs a few cents. These are estimates; Google's bill is the final word.")
                      .arg(page.money(ai.listeningCost(60)))
                color: theme.ink4
                font.pixelSize: 12
                wrapMode: Text.Wrap
                lineHeight: 1.35
            }
        }
    }
}
