import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every shortcut that works, over a dimmed window. ? or Esc closes it.
// Shortcuts appear here as their features arrive.
Rectangle {
    color: "#cc0e0e10"

    MouseArea { anchors.fill: parent }

    readonly property var rows: [
        ["Space", qsTr("Generate, or Enter in the subject field")],
        ["Esc", qsTr("Cancel the batch, close full view, or leave a text field")],
        ["←  →  ↑  ↓", qsTr("Move between drafts")],
        ["F", qsTr("Show the selected draft full size")],
        ["R", qsTr("The same prompt again, with new seeds")],
        ["P", qsTr("Show or hide the full prompt")],
        ["M", qsTr("Choose the model")],
        ["?", qsTr("Show or hide this list")],
        ["Q", qsTr("Quit (asks first while a batch or a download runs)")],
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
                model: rows.length * 2
                delegate: Label {
                    required property int index
                    readonly property var row: rows[Math.floor(index / 2)]
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
