import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1100
    height: 720
    minimumWidth: 720
    minimumHeight: 480
    visible: true
    title: "Limgen"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: win.helpVisible; onActivated: win.helpVisible = false }
    // Nothing can be lost yet, so Q quits at once. Once batches and keeps
    // exist it asks first, and once the subject field arrives (L4) a bare Q
    // must not fire while the user is typing in it.
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; onActivated: Qt.quit() }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            PaletteBar {
                Layout.fillWidth: true
            }

            // What prompts for this theme will be built from, until the
            // batch grid takes this space.
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 32
                spacing: 12
                visible: backend.paletteWords.length > 0

                Item { Layout.fillHeight: true }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    text: qsTr("Prompts for this theme use")
                    color: "#9a9aa0"
                    font.pixelSize: 15
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    // Model input, so it stays in English in every language.
                    text: backend.paletteWords
                    font.pixelSize: 22
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: backend.mood
                    color: "#9a9aa0"
                    font.pixelSize: 16
                }
                Item { Layout.fillHeight: true }
            }

            Item {
                Layout.fillHeight: true
                visible: backend.paletteWords.length === 0
            }

            Label {
                Layout.fillWidth: true
                Layout.margins: 12
                visible: text.length > 0
                text: backend.status
                wrapMode: Text.Wrap
                color: "#ffb4a9"
            }
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
        }
    }
}
