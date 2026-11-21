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
                text: backend.running ? qsTr("Cancel") : qsTr("Generate")
                // The main action is filled with the theme accent, with text
                // in whichever of black or white reads on it.
                Material.elevation: 0
                Material.background: backend.running || !enabled ? undefined : Material.accent
                Material.foreground: backend.running || !enabled ? undefined : theme.accentForeground
                enabled: backend.running || backend.canGenerate
                focusPolicy: Qt.NoFocus
                onClicked: backend.running ? backend.cancel() : bar.submitted()
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
