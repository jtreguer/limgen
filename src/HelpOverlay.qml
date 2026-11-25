import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every shortcut that works on the screen shown, over a dimmed window.
// ? or Esc closes it.
Rectangle {
    id: overlay
    color: "#cc0e0e10"

    // "home", "theme" or "generic".
    property string screen: "theme"

    MouseArea { anchors.fill: parent }

    readonly property var homeRows: [
        ["←  →", qsTr("Choose Generic generation or Theme backgrounds")],
        ["Enter  Space", qsTr("Open the one chosen")],
        ["1  2", qsTr("Open Generic generation or Theme backgrounds")],
        ["?", qsTr("Show or hide this list")],
        ["Q", qsTr("Quit (asks first while work would be lost)")],
    ]

    readonly property var genericRows: [
        ["Space", qsTr("Make the previews, or Enter in the prompt field")],
        ["Esc", qsTr("Close what is open, leave a text field, stop the work, or go back to the previews")],
        ["←  →  ↑  ↓", qsTr("Move between previews")],
        ["F", qsTr("Show the selected preview full size")],
        ["U", qsTr("Upscale the selected preview to the chosen size")],
        ["Ctrl+S", qsTr("Save the upscaled image")],
        ["H", qsTr("Back to the start")],
        ["?", qsTr("Show or hide this list")],
        ["Q", qsTr("Quit (asks first while work would be lost)")],
    ]

    readonly property var rows: screen === "home" ? homeRows : screen === "generic" ? genericRows : themeRows

    readonly property var themeRows: [
        ["Space", qsTr("Generate, or Enter in the subject field")],
        ["Esc", qsTr("Close what is open, leave a text field, cancel the batch, or stop making variations")],
        ["←  →  ↑  ↓", qsTr("Move between drafts")],
        ["F", qsTr("Show the selected draft full size")],
        ["K", qsTr("Keep: finish it and add it to the theme's backgrounds")],
        ["S", qsTr("Keep it and show it as the background now")],
        ["D", qsTr("Discard the selected draft")],
        ["Ctrl+Z  Ctrl+Shift+Z", qsTr("Undo or redo the last keep or discard")],
        ["V", qsTr("Variations of the selected image")],
        ["B  Shift+B", qsTr("Make variations of the theme's next or previous background")],
        ["L", qsTr("Show or hide the library")],
        ["R", qsTr("The same prompt again, with new seeds")],
        ["P", qsTr("Show or hide the full prompt")],
        ["M", qsTr("Choose the model")],
        ["H", qsTr("Back to the start")],
        ["?", qsTr("Show or hide this list")],
        ["Q", qsTr("Quit (asks first while a batch, a keep or a download runs)")],
    ]

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 560)
        height: list.implicitHeight + 48
        radius: theme.radius
        color: "#1c1c21"
        border.color: "#3a3a40"

        GridLayout {
            id: list
            anchors.fill: parent
            anchors.margins: 24
            columns: 2
            columnSpacing: 24
            rowSpacing: 10

            Label { text: qsTr("Shortcuts"); font.pixelSize: 18; font.bold: true; Layout.columnSpan: 2 }

            Repeater {
                model: overlay.rows.length * 2
                delegate: Label {
                    required property int index
                    readonly property var row: overlay.rows[Math.floor(index / 2)]
                    text: index % 2 === 0 ? row[0] : row[1]
                    font.bold: index % 2 === 0
                    color: index % 2 === 0 ? Material.accent : "#e4e4e4"
                    Layout.fillWidth: index % 2 === 1
                    wrapMode: Text.Wrap
                }
            }
        }
    }
}
