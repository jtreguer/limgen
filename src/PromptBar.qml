import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The subject, the style and the Generate button; with P, also the full
// prompt they become, which can be edited.
Rectangle {
    id: bar

    property bool promptVisible: false
    readonly property bool typing: subjectField.activeFocus || promptArea.activeFocus
    signal submitted()

    implicitHeight: column.implicitHeight + 24
    color: "#16161a"

    // The order of backend.styleIds: photo, cinematic, painting, minimal.
    readonly property var styleNames: [qsTr("Photo"), qsTr("Cinematic"), qsTr("Painting"), qsTr("Minimal")]

    ColumnLayout {
        id: column
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        RowLayout {
            Layout.fillWidth: true
            spacing: 12

            TextField {
                id: subjectField
                Layout.fillWidth: true
                placeholderText: qsTr("What should the background show? For example: oil rig at dusk")
                text: backend.subject
                selectByMouse: true
                onTextEdited: backend.subject = text
                onAccepted: bar.submitted()
            }

            ComboBox {
                Layout.preferredWidth: 160
                model: bar.styleNames
                currentIndex: backend.styleIndex
                focusPolicy: Qt.NoFocus
                onActivated: (index) => backend.styleIndex = index
            }

            Button {
                readonly property bool stops: backend.running || backend.batchWaiting
                text: stops ? qsTr("Cancel") : qsTr("Generate")
                // The main action is filled with the theme accent, with text
                // in whichever of black or white reads on it.
                Material.elevation: 0
                Material.background: stops || !enabled ? undefined : Material.accent
                Material.foreground: stops || !enabled ? undefined : theme.accentForeground
                enabled: stops || backend.canGenerate
                focusPolicy: Qt.NoFocus
                onClicked: stops ? backend.cancel() : bar.submitted()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: bar.promptVisible
            spacing: 8

            Label {
                Layout.fillWidth: true
                text: backend.promptEdited ? qsTr("Full prompt, edited; it no longer follows the subject and the theme")
                                           : qsTr("Full prompt, built from the subject, the style and the theme")
                color: "#9a9aa0"
                elide: Text.ElideRight
            }
            Button {
                text: qsTr("Reset")
                flat: true
                visible: backend.promptEdited
                focusPolicy: Qt.NoFocus
                onClicked: backend.resetPrompt()
            }
        }

        // How kept drafts are finished; folded with the prompt, since both
        // are set once and rarely changed.
        RowLayout {
            Layout.fillWidth: true
            visible: bar.promptVisible
            spacing: 12

            Label { text: qsTr("Colour pass"); color: "#9a9aa0" }
            Slider {
                id: colorPass
                Layout.preferredWidth: 180
                from: 0
                to: 1
                stepSize: 0.05
                focusPolicy: Qt.NoFocus
                value: backend.colorPass
                // Saved once the handle is let go, not at every step of a drag.
                onPressedChanged: if (!pressed) backend.colorPass = value
            }
            Label {
                Layout.preferredWidth: 48
                color: "#9a9aa0"
                text: {
                    translator.language
                    return colorPass.value <= 0 ? qsTr("Off")
                                                : qsTr("%1%").arg(Math.round(colorPass.value * 100).toLocaleString(Qt.locale(), 'f', 0))
                }
            }
            Label { text: qsTr("Kept images as"); color: "#9a9aa0" }
            ComboBox {
                Layout.preferredWidth: 110
                // File formats are names, not words, in every language.
                model: ["PNG", "JPEG"]
                currentIndex: backend.formatIndex
                focusPolicy: Qt.NoFocus
                onActivated: (index) => backend.formatIndex = index
            }
            Item { Layout.fillWidth: true }
        }

        ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: 96
            visible: bar.promptVisible

            TextArea {
                id: promptArea
                // Model input, so it stays in English in every language.
                text: backend.fullPrompt
                wrapMode: TextArea.Wrap
                selectByMouse: true
                onTextChanged: if (activeFocus) backend.fullPrompt = text
            }
        }
    }
}
