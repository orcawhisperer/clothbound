import QtQuick
import QtQuick.Controls

import "Format.js" as Format
import "Icons.js" as Icons
import "Ui.js" as Ui

// The book being read aloud: play and pause, a paragraph back or on, the
// pace, a sleep timer, the voice, and a way back to the voice's place when
// the reader has turned elsewhere.
Rectangle {
    id: bar

    property var look

    signal voiceRequested()
    signal followRequested()

    width: Math.max(240, Math.min(parent.width - 48, row.implicitWidth + 28))
    height: 52
    radius: Math.min(26, Math.max(theme.radius, 10))
    color: look.popover
    border.color: look.border

    // A soft shadow so the bar floats over the page.
    Rectangle {
        z: -1
        anchors.fill: parent
        anchors.topMargin: 4
        anchors.bottomMargin: -4
        radius: parent.radius
        color: "black"
        opacity: look.dark ? 0.35 : 0.12
    }

    Item {
        anchors.fill: parent
        anchors.leftMargin: 12
        anchors.rightMargin: 12
        clip: true

        Row {
            id: row
            anchors.centerIn: parent
            spacing: 4

            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                look: bar.look
                glyph: Icons.skipBack
                tone: bar.look.fg
                tip: qsTr("Previous paragraph (,)")
                onClicked: narration.previousParagraph()
            }
            AbstractButton {
                id: play
                anchors.verticalCenter: parent.verticalCenter
                implicitWidth: 40
                implicitHeight: 40
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                background: Rectangle {
                    radius: 20
                    color: play.hovered ? Qt.lighter(bar.look.link, 1.08) : bar.look.link
                }
                contentItem: Item {
                    Glyph {
                        anchors.centerIn: parent
                        anchors.horizontalCenterOffset: narration.playing ? 0 : 1
                        width: 18
                        height: 18
                        visible: !(narration.playing && (narration.buffering || narration.preparing))
                        path: narration.playing ? Icons.pause : Icons.play
                        stroke: 2.2
                        color: Ui.readableOn(bar.look.link)
                    }
                    BusyIndicator {
                        anchors.centerIn: parent
                        visible: (narration.buffering || narration.preparing) && narration.playing
                        running: visible
                        implicitWidth: 28
                        implicitHeight: 28
                    }
                }
                onClicked: narration.toggle()
                ToolTip.visible: hovered
                ToolTip.text: narration.playing ? qsTr("Pause (P)") : qsTr("Play (P)")
                ToolTip.delay: 700
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                look: bar.look
                glyph: Icons.skipForward
                tone: bar.look.fg
                tip: qsTr("Next paragraph (.)")
                onClicked: narration.nextParagraph()
            }

            Item { width: 10; height: 1 }

            // The voice and pace.
            AbstractButton {
                id: voiceButton
                anchors.verticalCenter: parent.verticalCenter
                implicitHeight: 36
                implicitWidth: voiceLabel.implicitWidth + 20
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                background: Rectangle {
                    radius: Math.min(8, theme.radius)
                    color: voiceButton.hovered ? Ui.tint(bar.look.fg, 0.08) : "transparent"
                }
                contentItem: Label {
                    id: voiceLabel
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: narration.voiceName
                    color: bar.look.fg
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }
                onClicked: bar.voiceRequested()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Choose the voice (V)")
                ToolTip.delay: 700
            }
            AbstractButton {
                id: rateButton
                anchors.verticalCenter: parent.verticalCenter
                implicitHeight: 36
                implicitWidth: rateLabel.implicitWidth + 18
                hoverEnabled: true
                focusPolicy: Qt.NoFocus
                background: Rectangle {
                    radius: Math.min(8, theme.radius)
                    color: rateButton.hovered ? Ui.tint(bar.look.fg, 0.08) : "transparent"
                }
                contentItem: Label {
                    id: rateLabel
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    text: Number(narration.rate).toLocaleString(Qt.locale(translator.language), 'f', narration.rate * 20 % 2 ? 2 : 1) + "×"
                    color: bar.look.muted
                    font.pixelSize: 13
                }
                onClicked: {
                    const steps = [1.0, 1.15, 1.3, 1.5, 1.75, 2.0, 0.75, 0.9]
                    const at = steps.findIndex(step => Math.abs(step - narration.rate) < 0.01)
                    narration.rate = steps[(at + 1) % steps.length]
                }
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Speed (< and >)")
                ToolTip.delay: 700
            }

            Label {
                anchors.verticalCenter: parent.verticalCenter
                visible: text !== ""
                leftPadding: 6
                rightPadding: 6
                text: narration.problem !== "" ? narration.problem
                    : narration.preparing ? qsTr("Preparing ahead · %1%").arg(narration.preparedPercent)
                    : narration.minutesLeftInChapter > 0 && translator.language !== ""
                      ? qsTr("%1 left in chapter").arg(Format.duration(narration.minutesLeftInChapter)) : ""
                color: narration.problem !== "" ? (theme.colors.orange || "#e5a050") : bar.look.muted
                font.pixelSize: 12
                elide: Text.ElideRight
                width: Math.min(implicitWidth, Math.min(320, Math.max(80, bar.width - 340)))
            }

            TextButton {
                anchors.verticalCenter: parent.verticalCenter
                look: bar.look
                visible: !narration.following
                text: qsTr("Follow along")
                onClicked: bar.followRequested()
            }

            IconButton {
                id: sleepButton
                anchors.verticalCenter: parent.verticalCenter
                look: bar.look
                glyph: Icons.moon
                tone: bar.look.fg
                active: narration.sleepMinutes > 0 || narration.sleepAtChapterEnd
                tip: narration.sleepMinutes > 0 ? qsTr("Stopping in %n min", "", narration.sleepMinutes)
                   : narration.sleepAtChapterEnd ? qsTr("Stopping at the end of the chapter") : qsTr("Sleep timer")
                onClicked: sleepMenu.opened ? sleepMenu.close() : sleepMenu.open()

                Popup {
                    id: sleepMenu
                    y: -height - 10
                    x: sleepButton.width / 2 - width / 2
                    width: 220
                    padding: 6
                    background: Rectangle {
                        radius: Math.min(10, theme.radius)
                        color: bar.look.popover
                        border.color: bar.look.border
                    }
                    contentItem: Column {
                        spacing: 2
                        Repeater {
                            model: [15, 30, 60]
                            delegate: MenuRow {
                                required property int modelData
                                look: bar.look
                                text: qsTr("In %n minutes", "", modelData)
                                onClicked: { narration.sleepIn(modelData); sleepMenu.close() }
                            }
                        }
                        MenuRow {
                            look: bar.look
                            text: qsTr("At the end of the chapter")
                            checked: narration.sleepAtChapterEnd
                            onClicked: { narration.sleepAtEndOfChapter(); sleepMenu.close() }
                        }
                        MenuRow {
                            look: bar.look
                            text: qsTr("Off")
                            checked: narration.sleepMinutes === 0 && !narration.sleepAtChapterEnd
                            onClicked: { narration.sleepIn(0); sleepMenu.close() }
                        }
                    }
                }
            }

            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                look: bar.look
                glyph: Icons.tray
                tone: bar.look.fg
                tip: qsTr("Keep listening in the background")
                onClicked: win.listenInBackground()
            }
            IconButton {
                anchors.verticalCenter: parent.verticalCenter
                look: bar.look
                glyph: Icons.close
                tone: bar.look.muted
                tip: qsTr("Stop listening (⇧P)")
                onClicked: narration.stop()
            }
        }
    }
}
