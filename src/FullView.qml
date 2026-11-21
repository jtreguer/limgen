import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// One draft over the whole window. F, Esc or a click closes it.
Rectangle {
    id: view
    property var draft: null
    signal closed()

    color: "#f0080809"

    Image {
        anchors.fill: parent
        anchors.margins: 24
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        smooth: true
        source: view.draft ? view.draft.image : ""
    }

    Label {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: 6
        color: "#9a9aa0"
        font.pixelSize: 12
        text: {
            translator.language
            return view.draft ? qsTr("Seed %1").arg(Number(view.draft.seed).toLocaleString(Qt.locale(), 'f', 0)) : ""
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: view.closed()
    }
}
