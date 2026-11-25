import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// A batch as a grid: the theme screen's 2x2, or the Generic screen's
// previews in as many columns as suit them. The Repeater counts drafts
// rather than holding the list itself, so the tiles stay put while their
// progress and previews change every step, instead of being rebuilt and
// flickering.
Item {
    id: grid

    property var drafts: backend.drafts
    property int columns: 2
    property string emptyText: backend.paletteWords.length > 0
                               ? qsTr("Type what the background should show, then press Enter. Prompts for this theme use: %1.")
                                     .arg(backend.paletteWords)
                               : qsTr("Type what the background should show, then press Enter.")
    property int selected: 0
    signal select(int index)
    signal open(int index)

    GridLayout {
        anchors.fill: parent
        columns: grid.columns
        rowSpacing: 12
        columnSpacing: 12
        visible: grid.drafts.length > 0

        Repeater {
            model: grid.drafts.length
            delegate: DraftTile {
                required property int index
                Layout.fillWidth: true
                Layout.fillHeight: true
                // Equal tiles, whatever their images.
                Layout.preferredWidth: 1
                Layout.preferredHeight: 1
                draft: grid.drafts[index]
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
        visible: grid.drafts.length === 0
        text: grid.emptyText
        color: "#9a9aa0"
        font.pixelSize: 16
    }
}
