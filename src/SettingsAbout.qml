import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// Settings › About: the version, the licence, and who made what Dogear uses.
ColumnLayout {
    id: page

    spacing: 16

    SettingsCard {
        RowLayout {
            Layout.fillWidth: true
            spacing: 16
            Image {
                source: "qrc:/dogear.svg"
                sourceSize.width: 56
                sourceSize.height: 56
                Layout.preferredWidth: 56
                Layout.preferredHeight: 56
            }
            ColumnLayout {
                spacing: 2
                Label { text: "Dogear"; color: theme.ink1; font.family: "Literata"; font.pixelSize: 22; font.weight: Font.DemiBold }
                Label { text: qsTr("Version %1").arg(Qt.application.version); color: theme.ink3; font.pixelSize: 13 }
                Label { text: qsTr("An ebook reader for Omarchy. MIT licensed."); color: theme.ink2; font.pixelSize: 13 }
            }
        }
    }

    SettingsCard {
        title: qsTr("Credits")
        Label {
            Layout.fillWidth: true
            textFormat: Text.StyledText
            text: qsTr("Built-in voices use <b>sherpa-onnx</b> (Apache 2.0) with <b>ONNX Runtime</b> (MIT) and <b>espeak-ng</b> (GPL 3.0), downloaded with the voices; <b>Kokoro-82M</b> (Apache 2.0); <b>Pocket TTS</b> by Kyutai (CC BY 4.0, its voices from Kyutai's recordings); and <b>Piper</b> voices (MIT, each voice under its recording's own license, in its folder). The reading face is <b>Literata</b> and the interface face <b>Inter</b> (both OFL).")
            color: theme.ink2
            font.pixelSize: 12
            wrapMode: Text.Wrap
            lineHeight: 1.3
        }
        Label {
            Layout.fillWidth: true
            text: qsTr("Dogear reads with Qt (LGPL) and Chromium's engine.")
            color: theme.ink3
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
    }
}
