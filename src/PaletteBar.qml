import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The top bar: the theme's name and its palette as swatches. Hovering a
// swatch shows the words the colour becomes in prompts.
Rectangle {
    id: bar
    implicitHeight: 64
    color: "#16161a"

    RowLayout {
        anchors.fill: parent
        anchors.leftMargin: 20
        anchors.rightMargin: 20
        spacing: 16

        Label {
            text: backend.themeName.length > 0 ? backend.themeName : qsTr("No theme")
            font.pixelSize: 18
            font.bold: true
            elide: Text.ElideRight
            Layout.maximumWidth: bar.width / 3
        }

        Row {
            spacing: 8
            Repeater {
                model: backend.swatches
                delegate: Rectangle {
                    required property var modelData
                    width: 32
                    height: 32
                    radius: Math.min(4, theme.radius)
                    color: modelData.hex
                    border.width: 1
                    border.color: "#3a3a40"

                    HoverHandler { id: hover }
                    ToolTip.visible: hover.hovered
                    ToolTip.delay: 300
                    // The role ("background", "red") and the words are model
                    // input names, kept as they are.
                    ToolTip.text: qsTr("%1: %2 (%3)").arg(modelData.role).arg(modelData.words).arg(modelData.hex)
                }
            }
        }

        Item { Layout.fillWidth: true }

        Label {
            text: qsTr("? for shortcuts")
            color: "#9a9aa0"
        }
    }
}
