import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts

// The colours of a custom palette: one chip per colour (a click changes
// it, the small x removes it) and + to add one, up to five. Hovering a chip
// shows the words it becomes in the prompt.
RowLayout {
    id: chips
    spacing: 8

    readonly property bool dialogOpen: dialog.visible
    // The chip being changed, or -1 while one is added.
    property int editing: -1

    function add() {
        if (studio.customColors.length >= studio.maxColors)
            return
        editing = -1
        dialog.selectedColor = Material.accent
        dialog.open()
    }

    Repeater {
        model: studio.customColors
        delegate: Rectangle {
            id: chip
            required property var modelData
            required property int index
            Layout.preferredWidth: 32
            Layout.preferredHeight: 32
            radius: Math.min(6, theme.radius)
            color: modelData.hex
            border.width: 1
            border.color: "#3a3a40"

            HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
            ToolTip.visible: hover.hovered
            ToolTip.delay: 300
            // The words are model input, kept in English.
            ToolTip.text: qsTr("%1 (%2). Click to change it.").arg(modelData.words).arg(modelData.hex)

            TapHandler {
                onTapped: {
                    chips.editing = chip.index
                    dialog.selectedColor = chip.modelData.hex
                    dialog.open()
                }
            }

            Rectangle {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: -5
                width: 16
                height: 16
                radius: 8
                color: "#2a2a30"
                border.color: "#4a4a50"
                visible: hover.hovered

                Label {
                    anchors.centerIn: parent
                    text: "×"
                    font.pixelSize: 12
                    color: "#e4e4e4"
                }
                TapHandler { onTapped: studio.removeColor(chip.index) }
            }
        }
    }

    Button {
        objectName: "addColorButton"
        text: "+"
        flat: true
        visible: studio.customColors.length < studio.maxColors
        focusPolicy: Qt.NoFocus
        Layout.preferredWidth: 40
        onClicked: chips.add()
        ToolTip.visible: hovered
        ToolTip.delay: 500
        ToolTip.text: qsTr("Add a colour")
    }

    Label {
        visible: studio.customColors.length === 0
        text: qsTr("Add up to five colours")
        color: "#9a9aa0"
    }

    ColorDialog {
        id: dialog
        title: chips.editing < 0 ? qsTr("Add a colour") : qsTr("Change the colour")
        onAccepted: {
            const hex = selectedColor.toString()
            if (chips.editing < 0)
                studio.addColor(hex)
            else
                studio.setColor(chips.editing, hex)
        }
    }
}
