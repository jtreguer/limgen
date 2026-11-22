import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The current theme's backgrounds, its own and the user's, small. Clicking
// one (or B) makes it the source of the next batch, which then varies it;
// clicking it again goes back to plain batches (docs/design.md section 7.3).
Rectangle {
    id: column
    color: "#121215"

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Label {
            text: qsTr("Backgrounds")
            font.bold: true
        }

        Label {
            Layout.fillWidth: true
            visible: backend.backgrounds.length === 0
            wrapMode: Text.Wrap
            color: "#9a9aa0"
            font.pixelSize: 13
            text: qsTr("This theme has no backgrounds yet.")
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            spacing: 8
            model: backend.backgrounds

            delegate: Rectangle {
                required property var modelData
                required property int index
                readonly property bool chosen: backend.source.path === modelData.path

                width: ListView.view.width
                height: Math.round(width * 9 / 16)
                radius: theme.radius
                color: "#1c1c21"
                border.width: chosen ? 2 : 0
                border.color: Material.accent

                Image {
                    anchors.fill: parent
                    anchors.margins: parent.border.width
                    source: modelData.url
                    // Decoded small: a theme background is often 4K.
                    sourceSize.width: 360
                    fillMode: Image.PreserveAspectCrop
                    asynchronous: true
                    smooth: true
                }

                MouseArea {
                    anchors.fill: parent
                    onClicked: backend.useBackground(index)
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: backend.backgrounds.length > 0
            wrapMode: Text.Wrap
            color: "#9a9aa0"
            font.pixelSize: 12
            text: qsTr("Click one, or press B, to make variations of it.")
        }
    }
}
