import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The first screen: one big button per part of limgen, Generic generation
// on the left and Theme backgrounds on the right. Left and Right choose,
// Enter or Space opens; 1 and 2 open one directly.
Item {
    id: home

    // Keys act only while nothing else has them (no dialog, no help).
    property bool keysActive: true
    property int current: 0
    readonly property var screens: ["generic", "theme"]
    signal open(string screen)

    readonly property real side: Math.max(200, Math.min(380, (width - 120) / 2, height - 260))

    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: home.keysActive; onActivated: home.current = 0 }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: home.keysActive; onActivated: home.current = 1 }
    Shortcut { sequences: ["Return", "Enter", "Space"]; context: Qt.ApplicationShortcut; enabled: home.keysActive; onActivated: home.open(home.screens[home.current]) }
    Shortcut { sequence: "1"; context: Qt.ApplicationShortcut; enabled: home.keysActive; onActivated: home.open("generic") }
    Shortcut { sequence: "2"; context: Qt.ApplicationShortcut; enabled: home.keysActive; onActivated: home.open("theme") }

    component Choice: Rectangle {
        id: choice
        property int number: 0
        property string title
        property string description
        property bool current: false
        default property alias art: artArea.data
        signal chosen()

        implicitWidth: home.side
        implicitHeight: home.side
        radius: theme.radius
        color: hover.hovered ? "#1c1c21" : "#16161a"
        border.width: current ? 2 : 1
        border.color: current ? Material.accent : "#2a2a30"

        HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
        TapHandler { onTapped: choice.chosen() }

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 24
            spacing: 10

            Item {
                id: artArea
                Layout.fillWidth: true
                Layout.fillHeight: true
            }
            Label {
                Layout.fillWidth: true
                text: choice.title
                font.pixelSize: 22
                font.bold: true
                wrapMode: Text.Wrap
            }
            Label {
                Layout.fillWidth: true
                text: choice.description
                color: "#9a9aa0"
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
            }
            Label {
                text: qsTr("Press %1").arg(Number(choice.number).toLocaleString(Qt.locale(), 'f', 0))
                color: choice.current ? Material.accent : "#6a6a70"
                font.pixelSize: 13
            }
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 28

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("What would you like to make?")
            font.pixelSize: 24
            font.bold: true
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            spacing: 32

            Choice {
                objectName: "genericChoice"
                number: 1
                current: home.current === 0
                title: qsTr("Generic generation")
                description: qsTr("Any image, at the size you choose, with a palette if you want one.")
                onChosen: home.open("generic")

                // A grid of previews, one of them picked.
                Grid {
                    anchors.centerIn: parent
                    columns: 3
                    spacing: 8
                    Repeater {
                        model: 6
                        delegate: Rectangle {
                            required property int index
                            width: home.side / 6.5
                            height: width * 0.6
                            radius: Math.min(4, theme.radius)
                            color: index === 4 ? Material.accent : Qt.rgba(1, 1, 1, 0.05 + 0.03 * index)
                        }
                    }
                }
            }

            Choice {
                objectName: "themeChoice"
                number: 2
                current: home.current === 1
                title: qsTr("Theme backgrounds")
                description: backend.themeName.length > 0
                             ? qsTr("Backgrounds that match %1, added to its background folder.").arg(backend.themeName)
                             : qsTr("Backgrounds for the current Omarchy theme.")
                onChosen: home.open("theme")

                // The theme's palette as stripes.
                Row {
                    anchors.centerIn: parent
                    height: home.side / 4
                    Repeater {
                        model: backend.swatches.length > 0 ? backend.swatches : [{ hex: "#2a2a30" }, { hex: "#3a3a40" }, { hex: "#4a4a50" }]
                        delegate: Rectangle {
                            required property var modelData
                            width: home.side * 0.6 / Math.max(3, backend.swatches.length)
                            height: parent.height
                            color: modelData.hex
                        }
                    }
                }
            }
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("← → to choose, Enter to open, ? for shortcuts")
            color: "#6a6a70"
        }
    }
}
