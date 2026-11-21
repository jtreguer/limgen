import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// One draft: its live preview while it is made, then the draft itself.
Rectangle {
    id: tile

    property var draft
    property bool selected: false
    readonly property string phase: draft ? draft.state : "waiting"
    signal clicked()
    signal doubleClicked()

    radius: theme.radius
    color: "#16161a"
    border.width: selected ? 2 : 1
    border.color: selected ? Material.accent : "#2a2a30"
    clip: true

    Image {
        id: picture
        anchors.fill: parent
        anchors.margins: tile.border.width
        fillMode: Image.PreserveAspectFit
        asynchronous: true
        // Previews get a new file name each step; nothing gains from caching.
        cache: false
        smooth: true
        source: !tile.draft ? "" : tile.draft.image !== "" ? tile.draft.image : tile.draft.preview
    }

    BusyIndicator {
        anchors.centerIn: parent
        running: visible
        visible: tile.phase !== "done" && picture.status !== Image.Ready
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 34
        color: "#c0101014"
        visible: tile.phase !== "done"
        radius: Math.min(4, theme.radius)

        Label {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: -3
            font.pixelSize: 13
            color: "#d0d0d6"
            // Read translator.language so the numbers re-render on a switch.
            text: {
                translator.language
                if (!tile.draft)
                    return ""
                const locale = Qt.locale()
                if (tile.phase === "sampling")
                    return qsTr("Step %1 of %2").arg(Number(tile.draft.step).toLocaleString(locale, 'f', 0))
                                                .arg(Number(tile.draft.steps).toLocaleString(locale, 'f', 0))
                if (tile.phase === "sampled")
                    return qsTr("Sampled; decoded when the batch ends")
                return qsTr("Waiting")
            }
        }

        ProgressBar {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            from: 0
            to: 1
            value: !tile.draft || tile.draft.steps <= 0 ? 0
                   : tile.phase === "sampled" ? 1 : tile.draft.step / tile.draft.steps
        }
    }

    MouseArea {
        anchors.fill: parent
        onClicked: tile.clicked()
        onDoubleClicked: tile.doubleClicked()
    }
}
