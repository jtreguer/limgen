import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Shown until limgen can make drafts: while the engine is checked, when the
// engine cannot run (and what to do about it), and when the chosen model is
// not installed, with its licence, size and the free disk space before the
// user agrees to download it (docs/design.md sections 5 and 7.1).
Item {
    id: panel
    signal chooseModel()

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 640)
        spacing: 12

        RowLayout {
            visible: backend.engineChecking
            spacing: 12
            BusyIndicator { running: parent.visible }
            Label { text: qsTr("Checking the engine and the GPU"); font.pixelSize: 16 }
        }

        ColumnLayout {
            Layout.fillWidth: true
            visible: !backend.engineChecking && !backend.engineReady
            spacing: 8

            Label { text: qsTr("The engine cannot run"); font.pixelSize: 20; font.bold: true }
            Label {
                Layout.fillWidth: true
                text: backend.engineProblem
                wrapMode: Text.Wrap
                color: "#ffb4a9"
            }
            Button {
                text: qsTr("Check again")
                onClicked: backend.recheckEngine()
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.topMargin: backend.engineReady ? 0 : 24
            visible: !backend.engineChecking && !backend.modelInstalled
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: qsTr("%1 is not installed yet").arg(backend.modelName)
                font.pixelSize: 20
                font.bold: true
                wrapMode: Text.Wrap
            }
            Label { text: qsTr("By %1").arg(backend.publisher); color: "#9a9aa0" }
            Label {
                Layout.fillWidth: true
                text: qsTr("Licence: %1").arg(backend.licenceText)
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                textFormat: Text.StyledText
                text: "<a href=\"" + backend.licenceUrl + "\">" + backend.licenceUrl + "</a>"
                linkColor: Material.accent
                elide: Text.ElideRight
                onLinkActivated: (link) => Qt.openUrlExternally(link)
            }
            Label { text: backend.downloadSize }
            Label {
                Layout.fillWidth: true
                text: backend.diskText
                color: backend.diskTooFull ? "#ffb4a9" : "#9a9aa0"
                wrapMode: Text.Wrap
            }

            ProgressBar {
                Layout.fillWidth: true
                visible: backend.downloading
                from: 0
                to: 1
                value: backend.downloadFraction
            }
            Label {
                visible: backend.downloading
                text: backend.downloadText
                color: "#9a9aa0"
            }

            RowLayout {
                spacing: 12
                Button {
                    text: backend.downloading ? qsTr("Pause") : qsTr("Download")
                    Material.elevation: 0
                    Material.background: backend.downloading || !enabled ? undefined : Material.accent
                    Material.foreground: backend.downloading || !enabled ? undefined : theme.accentForeground
                    enabled: backend.downloading || !backend.diskTooFull
                    onClicked: backend.downloading ? backend.pauseDownload() : backend.startDownload()
                }
                Button {
                    text: qsTr("Choose another model")
                    flat: true
                    enabled: !backend.downloading
                    onClicked: panel.chooseModel()
                }
            }

            Label {
                Layout.fillWidth: true
                Layout.topMargin: 8
                text: qsTr("limgen goes online only when you press Download. The files come from their publishers and are checked against their SHA-256 before use.")
                wrapMode: Text.Wrap
                color: "#9a9aa0"
                font.pixelSize: 13
            }
        }
    }
}
