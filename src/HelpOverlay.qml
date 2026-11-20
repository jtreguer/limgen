import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every shortcut that works, over a dimmed window. ? or Esc closes it.
// Shortcuts appear here as their features arrive.
Rectangle {
    color: "#cc0e0e10"

    MouseArea { anchors.fill: parent }

    Rectangle {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 460)
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

            Label { text: "?"; font.bold: true; color: Material.accent }
            Label { text: qsTr("Show or hide this list"); Layout.fillWidth: true; wrapMode: Text.Wrap }

            Label { text: "Q"; font.bold: true; color: Material.accent }
            Label { text: qsTr("Quit"); Layout.fillWidth: true; wrapMode: Text.Wrap }
        }
    }
}
