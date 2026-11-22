import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// What limgen made for the current theme, drafts and finished images,
// newest first. The arrows move, F shows one full size, V varies it, and L
// or Esc goes back to the batch.
Rectangle {
    id: view

    property int selected: 0
    // The arrows move by a whole row up and down. Thumbnails are about
    // 240 pixels wide and share the width between them.
    readonly property int columns: Math.max(1, Math.floor(grid.width / 240))
    signal select(int index)
    signal open(int index)

    color: "#0e0e10"

    onSelectedChanged: grid.positionViewAtIndex(selected, GridView.Contain)

    // The batch's grid lies underneath; no click goes through to it.
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 16
        spacing: 10

        Label {
            font.pixelSize: 18
            font.bold: true
            text: backend.themeName !== "" ? qsTr("Library for %1").arg(backend.themeName) : qsTr("Library")
        }

        Label {
            visible: backend.library.length === 0
            color: "#9a9aa0"
            text: qsTr("Nothing made for this theme yet.")
        }

        GridView {
            id: grid
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            cellWidth: Math.floor(width / view.columns)
            cellHeight: Math.round(cellWidth * 0.6)
            model: backend.library

            delegate: Item {
                required property var modelData
                required property int index
                width: grid.cellWidth
                height: grid.cellHeight

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: 6
                    radius: theme.radius
                    color: "#16161a"
                    border.width: index === view.selected ? 2 : 1
                    border.color: index === view.selected ? Material.accent : "#2a2a30"
                    clip: true

                    Image {
                        anchors.fill: parent
                        anchors.margins: 2
                        source: modelData.image
                        sourceSize.width: 400
                        fillMode: Image.PreserveAspectFit
                        asynchronous: true
                        smooth: true
                    }

                    Rectangle {
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.margins: 8
                        visible: modelData.kind === "final" || modelData.kept
                        radius: Math.min(4, theme.radius)
                        color: Material.accent
                        width: badge.implicitWidth + 12
                        height: badge.implicitHeight + 6

                        Label {
                            id: badge
                            anchors.centerIn: parent
                            font.pixelSize: 12
                            font.bold: true
                            color: theme.accentForeground
                            text: modelData.kind === "final" ? qsTr("Finished") : qsTr("Kept")
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: view.select(index)
                        onDoubleClicked: view.open(index)
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            wrapMode: Text.Wrap
            color: "#9a9aa0"
            font.pixelSize: 13
            text: qsTr("V makes variations of the selected image, F shows it full size, L or Esc goes back.")
        }
    }
}
