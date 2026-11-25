import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts

// The Generic screen: a prompt, the final size, how many previews, the
// model and the engine, and an optional palette. The previews come first,
// small; the one picked is upscaled to the chosen size and saved where the
// user likes. Nothing here depends on the current theme.
Item {
    id: screen

    // Keys act only while this screen is shown and nothing else has them.
    property bool keysFree: true
    signal home()

    property int selected: 0
    property bool showResult: false
    property bool fullView: false
    readonly property bool typing: promptField.activeFocus || widthField.activeFocus || heightField.activeFocus
    // An open list counts as a dialog: its keys are its own, not the screen's.
    readonly property bool dialogOpen: chips.dialogOpen || saveDialog.visible || modelBox.popup.opened ||
                                       engineBox.popup.opened || sizeBox.popup.opened || gridBox.popup.opened ||
                                       varietyBox.popup.opened || paletteBox.popup.opened
    readonly property bool actionKeys: visible && keysFree && !typing
    readonly property bool setupNeeded: !backend.engineReady || !backend.modelInstalled
    readonly property var selectedDraft: studio.drafts.length > selected ? studio.drafts[selected] : null
    readonly property bool hasResult: studio.result.image !== undefined
    // The size an upscale makes: the one the previews on screen were made
    // for, which may differ from the Resolution box when it changed since.
    readonly property bool hasBatch: studio.batchWidth > 0
    readonly property int upscaleWidth: hasBatch ? studio.batchWidth : studio.targetWidth
    readonly property int upscaleHeight: hasBatch ? studio.batchHeight : studio.targetHeight
    readonly property bool batchSizeDiffers: hasBatch && (studio.batchWidth !== studio.targetWidth ||
                                                       studio.batchHeight !== studio.targetHeight)
    readonly property bool canUpscale: selectedDraft !== null && selectedDraft.state === "done" && !studio.busy && !backend.gpuBusy
    // The previews' shape: those on screen, or the next batch's.
    readonly property real ratio: {
        const d = studio.drafts.length > 0 ? studio.drafts[0] : null
        if (d && d.width > 0)
            return d.width / d.height
        return studio.draftHeight > 0 ? studio.draftWidth / studio.draftHeight : 16 / 9
    }
    // The column count that shows the previews largest in the space there is.
    readonly property int columns: {
        const n = Math.max(1, studio.drafts.length > 0 ? studio.drafts.length : studio.count)
        const w = Math.max(1, previewArea.width), h = Math.max(1, previewArea.height)
        let best = 1, bestArea = -1
        for (let c = 1; c <= n; ++c) {
            const rows = Math.ceil(n / c)
            const tileW = (w - (c - 1) * 12) / c, tileH = (h - (rows - 1) * 12) / rows
            const imageW = Math.min(tileW, tileH * ratio)
            const area = imageW * imageW / ratio
            if (area > bestArea + 1) {
                best = c
                bestArea = area
            }
        }
        return best
    }

    function generate() {
        leaveFields()
        studio.generate()
    }

    function leaveFields() { screen.forceActiveFocus() }

    function move(dx, dy) {
        const count = studio.drafts.length
        if (count === 0 || showResult)
            return
        const column = selected % columns + dx
        const row = Math.floor(selected / columns) + dy
        if (column < 0 || column >= columns || row < 0)
            return
        const next = row * columns + column
        if (next < count)
            selected = next
    }

    function save() {
        if (!hasResult || studio.saving)
            return
        saveDialog.currentFolder = studio.saveFolder
        saveDialog.selectedFile = studio.saveFolder + "/" + studio.suggestedName
        saveDialog.open()
    }

    // Esc closes the full view, leaves a field, stops the work, or goes
    // back from the result to the previews, in that order. True when it
    // did something.
    function handleEscape() {
        if (fullView)
            fullView = false
        else if (typing)
            leaveFields()
        else if (studio.running || studio.upscaling)
            studio.cancel()
        else if (showResult)
            showResult = false
        else
            return false
        return true
    }

    function sizeLabel(size) {
        translator.language
        if (!size)
            return ""
        const dims = size.width + "×" + size.height
        switch (size.kind) {
        case "monitor": return qsTr("Monitor, %1").arg(dims)
        case "fallback": return qsTr("%1 (no monitor found)").arg(dims)
        case "wide": return qsTr("%1, ultrawide").arg(dims)
        case "portrait": return qsTr("%1, portrait").arg(dims)
        case "square": return qsTr("%1, square").arg(dims)
        case "custom": return qsTr("Custom size")
        default: return dims
        }
    }

    function paletteLabel(palette) {
        translator.language
        if (!palette)
            return ""
        if (palette.id === "")
            return qsTr("No palette")
        if (palette.id === "custom")
            return qsTr("My colours")
        return palette.id === backend.themeName ? qsTr("%1 (current theme)").arg(palette.id) : palette.id
    }

    // A new batch starts with its first preview selected and the previews
    // shown; a finished upscale shows its result.
    readonly property bool batchRunning: studio.running
    onBatchRunningChanged: {
        if (batchRunning) {
            showResult = false
            fullView = false
            selected = 0
        }
    }
    Connections {
        target: studio
        function onResultChanged() {
            if (screen.hasResult) {
                screen.showResult = true
                screen.fullView = false
            }
        }
    }

    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: screen.actionKeys; onActivated: screen.generate() }
    Shortcut { sequence: "U"; context: Qt.ApplicationShortcut; enabled: screen.actionKeys; onActivated: studio.upscale(screen.selected) }
    Shortcut { sequences: [StandardKey.Save]; context: Qt.ApplicationShortcut; enabled: screen.visible && screen.keysFree; onActivated: screen.save() }
    Shortcut {
        sequence: "F"
        context: Qt.ApplicationShortcut
        enabled: screen.actionKeys
        onActivated: {
            if (screen.fullView)
                screen.fullView = false
            else if (!screen.showResult && screen.selectedDraft && screen.selectedDraft.state === "done")
                screen.fullView = true
        }
    }
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: screen.actionKeys && !screen.fullView; onActivated: screen.move(-1, 0) }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: screen.actionKeys && !screen.fullView; onActivated: screen.move(1, 0) }
    Shortcut { sequence: "Up"; context: Qt.ApplicationShortcut; enabled: screen.actionKeys && !screen.fullView; onActivated: screen.move(0, -1) }
    Shortcut { sequence: "Down"; context: Qt.ApplicationShortcut; enabled: screen.actionKeys && !screen.fullView; onActivated: screen.move(0, 1) }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // The top bar: home, the screen's name, the model and the engine.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: 64
            color: "#16161a"

            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                spacing: 12

                Button {
                    objectName: "homeButton"
                    text: qsTr("Home")
                    flat: true
                    focusPolicy: Qt.NoFocus
                    onClicked: screen.home()
                    ToolTip.visible: hovered
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Back to the start (H)")
                }
                Label {
                    text: qsTr("Generic generation")
                    font.pixelSize: 18
                    font.bold: true
                    elide: Text.ElideRight
                    Layout.minimumWidth: Math.min(implicitWidth, 120)
                }
                Item { Layout.fillWidth: true }
                // The model's and the engine's names say what the two boxes
                // are; their tooltips say it too, to save the bar's width.
                ComboBox {
                    id: modelBox
                    objectName: "modelBox"
                    Layout.preferredWidth: 180
                    hoverEnabled: true
                    ToolTip.visible: hovered && !popup.visible
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Model")
                    model: backend.models
                    textRole: "name"
                    focusPolicy: Qt.NoFocus
                    enabled: !backend.busy && !studio.busy
                    currentIndex: {
                        for (let i = 0; i < backend.models.length; ++i)
                            if (backend.models[i].selected)
                                return i
                        return -1
                    }
                    onActivated: (index) => backend.selectModel(backend.models[index].id)
                    delegate: ItemDelegate {
                        required property var modelData
                        required property int index
                        width: ListView.view ? ListView.view.width : implicitWidth
                        highlighted: modelBox.highlightedIndex === index
                        text: {
                            translator.language
                            return modelData.installed ? modelData.name : qsTr("%1 (not installed)").arg(modelData.name)
                        }
                    }
                }
                ComboBox {
                    id: engineBox
                    objectName: "engineBox"
                    Layout.preferredWidth: 110
                    hoverEnabled: true
                    ToolTip.visible: hovered && !popup.visible
                    ToolTip.delay: 500
                    ToolTip.text: qsTr("Engine")
                    visible: backend.engines.length > 1
                    model: backend.engines
                    textRole: "name"
                    focusPolicy: Qt.NoFocus
                    enabled: !backend.busy && !studio.busy && !backend.engineChecking
                    currentIndex: backend.engineIndex
                    onActivated: (index) => backend.selectEngine(index)
                }
                Label {
                    text: backend.gpuText
                    color: "#9a9aa0"
                    elide: Text.ElideRight
                    Layout.minimumWidth: Math.min(implicitWidth, 120)
                }
                Label { text: qsTr("? for shortcuts"); color: "#9a9aa0" }
            }
        }

        // The previews, or the upscaled result.
        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            SetupPanel {
                anchors.fill: parent
                visible: screen.setupNeeded
                onChooseModel: modelBox.popup.open()
            }

            Item {
                id: previewArea
                anchors.fill: parent
                anchors.margins: 16
                visible: !screen.setupNeeded && !screen.showResult

                DraftGrid {
                    id: grid
                    objectName: "previewGrid"
                    anchors.fill: parent
                    drafts: studio.drafts
                    columns: screen.columns
                    selected: screen.selected
                    emptyText: qsTr("Type what the image should show, choose its size, then press Enter. The previews come first; upscale the one you like.")
                    onSelect: (index) => screen.selected = index
                    onOpen: (index) => {
                        screen.selected = index
                        if (studio.drafts[index].state === "done")
                            screen.fullView = true
                    }
                }
            }

            Item {
                anchors.fill: parent
                anchors.margins: 16
                visible: !screen.setupNeeded && screen.showResult && screen.hasResult

                Image {
                    anchors.fill: parent
                    anchors.bottomMargin: 24
                    source: studio.result.image || ""
                    fillMode: Image.PreserveAspectFit
                    asynchronous: true
                    smooth: true
                    mipmap: true
                }
                Label {
                    anchors.bottom: parent.bottom
                    anchors.horizontalCenter: parent.horizontalCenter
                    color: "#9a9aa0"
                    font.pixelSize: 12
                    text: {
                        translator.language
                        if (!screen.hasResult)
                            return ""
                        return qsTr("%1×%2, seed %3").arg(studio.result.width).arg(studio.result.height)
                                                       .arg(Number(studio.result.seed).toLocaleString(Qt.locale(), 'f', 0))
                    }
                }
            }
        }

        // The prompt and the choices.
        Rectangle {
            Layout.fillWidth: true
            implicitHeight: controls.implicitHeight + 24
            color: "#16161a"

            ColumnLayout {
                id: controls
                anchors.fill: parent
                anchors.margins: 12
                spacing: 10

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    TextField {
                        id: promptField
                        objectName: "promptField"
                        Layout.fillWidth: true
                        placeholderText: qsTr("What should the image show? For example: a red fox in fresh snow")
                        text: studio.prompt
                        selectByMouse: true
                        onTextEdited: studio.prompt = text
                        onAccepted: screen.generate()
                    }
                    Button {
                        readonly property bool stops: studio.running || studio.upscaling
                        text: stops ? qsTr("Cancel") : qsTr("Generate")
                        Material.elevation: 0
                        Material.background: stops || !enabled ? undefined : Material.accent
                        Material.foreground: stops || !enabled ? undefined : theme.accentForeground
                        enabled: stops || studio.canGenerate
                        focusPolicy: Qt.NoFocus
                        onClicked: stops ? studio.cancel() : screen.generate()
                    }
                }

                Flow {
                    Layout.fillWidth: true
                    spacing: 12

                    RowLayout {
                        spacing: 8
                        Label { text: qsTr("Resolution"); color: "#9a9aa0" }
                        ComboBox {
                            id: sizeBox
                            objectName: "sizeBox"
                            Layout.preferredWidth: 230
                            model: studio.sizes
                            focusPolicy: Qt.NoFocus
                            currentIndex: studio.sizeIndex
                            displayText: screen.sizeLabel(studio.sizes[currentIndex])
                            onActivated: (index) => studio.sizeIndex = index
                            delegate: ItemDelegate {
                                required property var modelData
                                required property int index
                                width: ListView.view ? ListView.view.width : implicitWidth
                                highlighted: sizeBox.highlightedIndex === index
                                text: screen.sizeLabel(modelData)
                            }
                        }
                        TextField {
                            id: widthField
                            objectName: "widthField"
                            visible: studio.customSize
                            Layout.preferredWidth: 80
                            text: studio.customWidth
                            inputMethodHints: Qt.ImhDigitsOnly
                            // Any number up to five digits gets through, so the setter can
                            // clamp it and the field then shows the value in use.
                            validator: RegularExpressionValidator { regularExpression: /[0-9]{1,5}/ }
                            selectByMouse: true
                            // The value in use goes back into the field, which an
                            // out-of-range number would otherwise leave showing.
                            onEditingFinished: {
                                studio.customWidth = parseInt(text)
                                text = Qt.binding(() => studio.customWidth)
                            }
                        }
                        Label { text: "×"; visible: studio.customSize }
                        TextField {
                            id: heightField
                            objectName: "heightField"
                            visible: studio.customSize
                            Layout.preferredWidth: 80
                            text: studio.customHeight
                            inputMethodHints: Qt.ImhDigitsOnly
                            // Any number up to five digits gets through, so the setter can
                            // clamp it and the field then shows the value in use.
                            validator: RegularExpressionValidator { regularExpression: /[0-9]{1,5}/ }
                            selectByMouse: true
                            onEditingFinished: {
                                studio.customHeight = parseInt(text)
                                text = Qt.binding(() => studio.customHeight)
                            }
                        }
                    }

                    RowLayout {
                        spacing: 8
                        Label { text: qsTr("Previews"); color: "#9a9aa0" }
                        ComboBox {
                            id: gridBox
                            objectName: "gridBox"
                            Layout.preferredWidth: 90
                            // Grid shapes, the same in every language.
                            model: studio.gridNames
                            focusPolicy: Qt.NoFocus
                            currentIndex: studio.gridIndex
                            enabled: !studio.running
                            onActivated: (index) => studio.gridIndex = index
                        }
                    }

                    RowLayout {
                        spacing: 8
                        Label { text: qsTr("Variety"); color: "#9a9aa0" }
                        ComboBox {
                            id: varietyBox
                            objectName: "varietyBox"
                            Layout.preferredWidth: 190
                            // The order of Prompt::varietyIds(): seeds, framing, light.
                            model: [qsTr("Seeds only"), qsTr("Framing"), qsTr("Framing and light")]
                            focusPolicy: Qt.NoFocus
                            currentIndex: studio.varietyIndex
                            enabled: !studio.running
                            onActivated: (index) => studio.varietyIndex = index
                            hoverEnabled: true
                            ToolTip.visible: hovered && !popup.visible
                            ToolTip.delay: 500
                            ToolTip.text: qsTr("How much the previews differ. The models change little from one seed to the next; with Framing each preview gets its own camera hint, with Framing and light its own light too. Each preview is then a run of its own, which takes a little longer.")
                        }
                    }

                    RowLayout {
                        spacing: 8
                        Label { text: qsTr("Palette"); color: "#9a9aa0" }
                        ComboBox {
                            id: paletteBox
                            objectName: "paletteBox"
                            Layout.preferredWidth: 220
                            model: studio.palettes
                            focusPolicy: Qt.NoFocus
                            currentIndex: studio.paletteIndex
                            displayText: screen.paletteLabel(studio.palettes[currentIndex])
                            onActivated: (index) => {
                                studio.paletteIndex = index
                                if (studio.customPalette && studio.customColors.length === 0)
                                    chips.add()
                            }
                            delegate: ItemDelegate {
                                id: paletteItem
                                required property var modelData
                                required property int index
                                width: ListView.view ? ListView.view.width : implicitWidth
                                highlighted: paletteBox.highlightedIndex === index
                                contentItem: RowLayout {
                                    spacing: 10
                                    Label {
                                        Layout.fillWidth: true
                                        text: screen.paletteLabel(paletteItem.modelData)
                                        elide: Text.ElideRight
                                    }
                                    Row {
                                        spacing: 3
                                        Repeater {
                                            model: paletteItem.modelData.swatches
                                            delegate: Rectangle {
                                                required property var modelData
                                                width: 14
                                                height: 14
                                                radius: Math.min(3, theme.radius)
                                                color: modelData
                                                border.width: 1
                                                border.color: "#3a3a40"
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        // The chosen theme's colours, or the chips to pick them.
                        Row {
                            spacing: 4
                            visible: !studio.customPalette && studio.paletteIndex > 0
                            Repeater {
                                model: studio.paletteIndex > 0 && studio.palettes[studio.paletteIndex]
                                       ? studio.palettes[studio.paletteIndex].swatches : []
                                delegate: Rectangle {
                                    required property var modelData
                                    width: 24
                                    height: 24
                                    radius: Math.min(4, theme.radius)
                                    color: modelData
                                    border.width: 1
                                    border.color: "#3a3a40"
                                }
                            }
                        }
                        ColorChips {
                            id: chips
                            visible: studio.customPalette
                        }
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 12

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2
                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            text: studio.sizeProblem.length > 0 ? studio.sizeProblem
                                  : screen.batchSizeDiffers
                                    ? qsTr("These previews upscale to %1×%2; generate again for %3×%4.")
                                          .arg(studio.batchWidth).arg(studio.batchHeight)
                                          .arg(studio.targetWidth).arg(studio.targetHeight)
                                    : qsTr("Previews at %1×%2, upscaled to %3×%4").arg(studio.draftWidth).arg(studio.draftHeight)
                                                                                    .arg(studio.targetWidth).arg(studio.targetHeight)
                            color: studio.sizeProblem.length > 0 ? "#ffb4a9" : "#9a9aa0"
                        }
                        Label {
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                            visible: studio.paletteWords.length > 0
                            // The words are model input, kept in English.
                            text: qsTr("The palette adds: %1").arg(studio.paletteWords)
                            color: "#9a9aa0"
                        }
                    }
                    Button {
                        text: qsTr("Previews")
                        flat: true
                        focusPolicy: Qt.NoFocus
                        visible: screen.showResult
                        onClicked: screen.showResult = false
                    }
                    Button {
                        text: qsTr("Result")
                        flat: true
                        focusPolicy: Qt.NoFocus
                        visible: !screen.showResult && screen.hasResult
                        onClicked: screen.showResult = true
                    }
                    Button {
                        objectName: "upscaleButton"
                        text: qsTr("Upscale selected")
                        focusPolicy: Qt.NoFocus
                        enabled: screen.canUpscale && !screen.showResult
                        onClicked: studio.upscale(screen.selected)
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: qsTr("Upscale the selected preview to %1×%2 (U)").arg(screen.upscaleWidth).arg(screen.upscaleHeight)
                    }
                    Button {
                        objectName: "saveButton"
                        text: qsTr("Save image")
                        focusPolicy: Qt.NoFocus
                        enabled: screen.hasResult && !studio.saving
                        Material.elevation: 0
                        Material.background: enabled ? Material.accent : undefined
                        Material.foreground: enabled ? theme.accentForeground : undefined
                        onClicked: screen.save()
                        ToolTip.visible: hovered
                        ToolTip.delay: 500
                        ToolTip.text: qsTr("Save the upscaled image (Ctrl+S)")
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 16
            Layout.rightMargin: 16
            Layout.topMargin: 6
            Layout.bottomMargin: 8
            spacing: 16

            Label {
                Layout.fillWidth: true
                // Downloads and engine checks report in the backend's line.
                text: studio.status.length > 0 ? studio.status : screen.setupNeeded ? backend.status : ""
                wrapMode: Text.Wrap
                maximumLineCount: 3
                elide: Text.ElideRight
                color: (studio.status.length > 0 ? studio.statusIsError : backend.statusIsError) ? "#ffb4a9" : "#9a9aa0"
            }
            Label {
                text: studio.estimate
                color: "#9a9aa0"
            }
        }
    }

    FullView {
        anchors.fill: parent
        visible: screen.fullView && screen.selectedDraft !== null && screen.selectedDraft.state === "done"
        draft: screen.selectedDraft
        onClosed: screen.fullView = false
    }

    FileDialog {
        id: saveDialog
        title: qsTr("Save image")
        fileMode: FileDialog.SaveFile
        nameFilters: [qsTr("PNG image (*.png)"), qsTr("JPEG image (*.jpg *.jpeg)")]
        // A name typed without an ending gets the filter's, before the
        // dialog asks about replacing a file of that name.
        defaultSuffix: selectedNameFilter.index === 1 ? "jpg" : "png"
        onAccepted: studio.saveImage(selectedFile, selectedNameFilter.index === 1)
    }
}
