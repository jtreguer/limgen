import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// One draft: its live preview while it is made, then the draft itself, and
// whether it is being kept, was kept, or was discarded.
Rectangle {
    id: tile

    property var draft
    property bool selected: false
    readonly property string phase: draft ? draft.state : "waiting"
    readonly property bool discarded: draft ? draft.discarded === true : false
    readonly property bool keeping: draft ? draft.keeping === true : false
    readonly property bool kept: draft ? draft.kept === true : false
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
        // The last preview stays until the next has loaded, rather than the
        // tile going blank for a moment at every step.
        retainWhileLoading: true
        smooth: true
        opacity: tile.discarded ? 0.15 : 1
        source: !tile.draft ? "" : tile.draft.image !== "" ? tile.draft.image : tile.draft.preview
    }

    Label {
        anchors.centerIn: parent
        width: parent.width - 32
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        visible: tile.discarded
        color: "#d0d0d6"
        text: qsTr("Discarded. Ctrl+Z brings it back.")
    }

    // Kept, or kept and shown as the background.
    Rectangle {
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 10
        visible: tile.kept
        radius: Math.min(4, theme.radius)
        color: Material.accent
        width: badge.implicitWidth + 16
        height: badge.implicitHeight + 8

        Label {
            id: badge
            anchors.centerIn: parent
            font.pixelSize: 13
            font.bold: true
            color: theme.accentForeground
            text: tile.draft && tile.draft.set ? qsTr("Kept, now the background") : qsTr("Kept")
        }
    }

    Rectangle {
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        height: 34
        color: "#c0101014"
        visible: tile.keeping
        radius: Math.min(4, theme.radius)

        Label {
            anchors.left: parent.left
            anchors.leftMargin: 10
            anchors.verticalCenter: parent.verticalCenter
            anchors.verticalCenterOffset: -3
            font.pixelSize: 13
            color: "#d0d0d6"
            text: {
                translator.language
                if (!tile.draft || tile.draft.keepFraction < 0)
                    return qsTr("Waiting to be finished")
                return qsTr("Finishing, %1%").arg(Math.round(tile.draft.keepFraction * 100).toLocaleString(Qt.locale(), 'f', 0))
            }
        }

        ProgressBar {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            from: 0
            to: 1
            indeterminate: !tile.draft || tile.draft.keepFraction < 0
            value: tile.draft && tile.draft.keepFraction > 0 ? tile.draft.keepFraction : 0
        }
    }

    // A generic preview's variety hint, the words that made it differ.
    Rectangle {
        anchors.left: parent.left
        anchors.bottom: parent.bottom
        anchors.margins: 8
        width: Math.min(parent.width - 16, hintLabel.implicitWidth + 12)
        height: hintLabel.implicitHeight + 6
        radius: Math.min(4, theme.radius)
        color: "#c0101014"
        visible: tile.phase === "done" && !tile.keeping && !tile.discarded && tile.draft !== null &&
                 tile.draft !== undefined && (tile.draft.hint || "") !== ""

        Label {
            id: hintLabel
            anchors.centerIn: parent
            width: Math.min(implicitWidth, parent.width - 12)
            elide: Text.ElideRight
            font.pixelSize: 12
            color: "#d0d0d6"
            // Model input, kept in English.
            text: tile.draft && tile.draft.hint ? tile.draft.hint : ""
        }
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
