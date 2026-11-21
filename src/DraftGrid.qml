import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The batch as a 2x2 grid. The Repeater counts drafts rather than holding
// the list itself, so the tiles stay put while their progress and previews
// change every step, instead of being rebuilt and flickering.
Item {
    id: grid

    property int selected: 0
    signal select(int index)
    signal open(int index)

    GridLayout {
        anchors.fill: parent
        columns: 2
        rowSpacing: 12
        columnSpacing: 12
        visible: backend.drafts.length > 0

        Repeater {
            model: backend.drafts.length
            delegate: DraftTile {
                required property int index
                Layout.fillWidth: true
                Layout.fillHeight: true
                draft: backend.drafts[index]
                selected: index === grid.selected
                onClicked: grid.select(index)
                onDoubleClicked: grid.open(index)
            }
        }
    }

    Label {
        anchors.centerIn: parent
        width: Math.min(parent.width - 64, 560)
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        visible: backend.drafts.length === 0
        text: backend.paletteWords.length > 0
              ? qsTr("Type what the background should show, then press Enter. Prompts for this theme use: %1.")
                    .arg(backend.paletteWords)
              : qsTr("Type what the background should show, then press Enter.")
        color: "#9a9aa0"
        font.pixelSize: 16
    }
}
