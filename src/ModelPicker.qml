import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Every model in the catalogue, whether it is installed, its size if not,
// and whether it runs on this GPU now. Up, Down and Enter pick one.
Popup {
    id: picker
    modal: true
    focus: true
    width: Math.min(parent ? parent.width - 64 : 560, 560)
    padding: 20

    onOpened: {
        for (let i = 0; i < backend.models.length; ++i)
            if (backend.models[i].selected)
                list.currentIndex = i
        list.forceActiveFocus()
    }

    function choose(index) {
        if (index >= 0 && index < backend.models.length)
            backend.selectModel(backend.models[index].id)
        picker.close()
    }

    // A dark dim, matching the window; the style's default washes it out.
    Overlay.modal: Rectangle { color: "#a0000000" }

    background: Rectangle {
        color: "#1c1c21"
        radius: theme.radius
        border.color: "#3a3a40"
    }

    contentItem: ColumnLayout {
        spacing: 12

        Label { text: qsTr("Model"); font.pixelSize: 18; font.bold: true }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.preferredHeight: contentHeight
            interactive: false
            model: backend.models
            keyNavigationEnabled: true
            Keys.onReturnPressed: picker.choose(currentIndex)
            Keys.onEnterPressed: picker.choose(currentIndex)

            delegate: ItemDelegate {
                required property var modelData
                required property int index
                width: ListView.view.width
                highlighted: ListView.isCurrentItem
                onClicked: picker.choose(index)

                contentItem: ColumnLayout {
                    spacing: 2
                    Label {
                        text: modelData.selected ? qsTr("%1 (in use)").arg(modelData.name) : modelData.name
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        color: "#9a9aa0"
                        wrapMode: Text.Wrap
                        text: {
                            const parts = [modelData.publisher, modelData.licence,
                                           modelData.installed ? qsTr("installed") : qsTr("%1 to download").arg(modelData.size)]
                            if (modelData.runsHere === 1)
                                parts.push(qsTr("runs here"))
                            else if (modelData.runsHere === 0)
                                parts.push(qsTr("needs more free video memory than there is now"))
                            return parts.join(" · ")
                        }
                    }
                }
            }
        }
    }
}
