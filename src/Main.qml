import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1200
    height: 820
    minimumWidth: 820
    minimumHeight: 580
    visible: true
    title: "Limgen"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool helpVisible: false
    property bool fullView: false
    property bool promptVisible: false
    property int selected: 0
    property bool libraryVisible: false
    property int librarySelected: 0
    property bool quitting: false
    // Single-letter shortcuts are off while the user types, or a dialog is
    // up. Under the help only ? and Q act, so nothing changes out of sight.
    readonly property bool typing: promptBar.typing
    readonly property bool dialogOpen: modelPicker.opened || quitDialog.opened || themeDialog.opened
    readonly property bool keysFree: !typing && !dialogOpen
    readonly property bool actionKeysFree: keysFree && !helpVisible
    readonly property var selectedDraft: backend.drafts.length > selected ? backend.drafts[selected] : null
    readonly property var libraryItem: backend.library.length > librarySelected ? backend.library[librarySelected] : null
    // The selection follows its image when the list changes (a discard, a
    // keep, a new batch), and stays in range when the image is gone.
    property string librarySelectedId: ""
    // Read from the list itself: libraryItem may not have caught up yet.
    onLibrarySelectedChanged: {
        const item = backend.library[librarySelected]
        librarySelectedId = item ? item.id : ""
    }
    Connections {
        target: backend
        function onLibraryChanged() {
            const items = backend.library
            for (let i = 0; i < items.length; ++i) {
                if (items[i].id === win.librarySelectedId) {
                    win.librarySelected = i
                    return
                }
            }
            win.librarySelected = Math.max(0, Math.min(win.librarySelected, items.length - 1))
            const item = items[win.librarySelected]
            win.librarySelectedId = item ? item.id : ""
        }
    }
    // What F shows and V varies: the library's selection while it is open.
    readonly property var shownDraft: libraryVisible ? libraryItem : selectedDraft
    readonly property bool setupNeeded: !backend.engineReady || !backend.modelInstalled

    // The drafts sit in a 2x2 grid: left and right move along a row, up and
    // down between the rows.
    function move(dx, dy) {
        if (libraryVisible) {
            const next = librarySelected + dx + dy * libraryView.columns
            if (next >= 0 && next < backend.library.length)
                librarySelected = next
            return
        }
        const count = backend.drafts.length
        if (count === 0)
            return
        const column = selected % 2 + dx
        const row = Math.floor(selected / 2) + dy
        if (column < 0 || column > 1 || row < 0)
            return
        const next = row * 2 + column
        if (next < count)
            selected = next
    }

    function requestQuit() {
        if (backend.busy) {
            quitDialog.open()
            return
        }
        quitting = true
        Qt.quit()
    }

    function leaveFields() { grid.forceActiveFocus() }

    // Opening or closing the library also closes a full view, which shows
    // whichever of the two is open.
    function toggleLibrary() {
        if (!libraryVisible) {
            backend.loadLibrary()
            librarySelected = 0
        }
        fullView = false
        libraryVisible = !libraryVisible
    }

    function vary() {
        fullView = false
        if (libraryVisible) {
            if (libraryItem) {
                backend.varyLibrary(libraryItem.id)
                libraryVisible = false
            }
        } else {
            backend.vary(selected)
        }
    }

    onClosing: (close) => {
        if (backend.busy && !quitting) {
            close.accepted = false
            quitDialog.open()
        }
    }

    // Keeping a draft made for another theme asks where it should go.
    Connections {
        target: backend
        function onThemeChoiceNeeded(index, madeFor, current, set) {
            themeDialog.index = index
            themeDialog.madeFor = madeFor
            themeDialog.current = current
            themeDialog.set = set
            themeDialog.open()
        }
    }

    // A new batch starts with its first draft selected and the grid shown.
    // This handler runs only when the value changes, not on every change of
    // the backend's state, such as a letter typed during the batch.
    readonly property bool batchRunning: backend.running
    onBatchRunningChanged: {
        if (batchRunning) {
            fullView = false
            libraryVisible = false
            selected = 0
        }
    }

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: win.helpVisible = !win.helpVisible }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: win.keysFree; onActivated: win.requestQuit() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree; onActivated: backend.generate() }
    Shortcut { sequence: "R"; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree; onActivated: backend.rerun() }
    Shortcut { sequence: "P"; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree; onActivated: win.promptVisible = !win.promptVisible }
    Shortcut { sequence: "M"; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree && !backend.busy; onActivated: modelPicker.open() }
    Shortcut {
        sequence: "F"
        context: Qt.ApplicationShortcut
        enabled: win.actionKeysFree
        onActivated: {
            if (win.fullView)
                win.fullView = false
            else if (win.shownDraft && win.shownDraft.state === "done")
                win.fullView = true
        }
    }
    // K, S and D act on the batch's drafts, so not while the library is open.
    Shortcut { sequence: "K"; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree && !win.libraryVisible; onActivated: backend.keep(win.selected) }
    Shortcut { sequence: "S"; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree && !win.libraryVisible; onActivated: backend.keepAndSet(win.selected) }
    Shortcut { sequence: "D"; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree && !win.libraryVisible; onActivated: backend.discard(win.selected) }
    Shortcut { sequence: "V"; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree; onActivated: win.vary() }
    Shortcut { sequence: "B"; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree; onActivated: backend.cycleBackground(1) }
    Shortcut { sequence: "Shift+B"; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree; onActivated: backend.cycleBackground(-1) }
    Shortcut { sequence: "L"; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree; onActivated: win.toggleLibrary() }
    // Off while typing, so a text field keeps its own undo.
    Shortcut { sequences: [StandardKey.Undo]; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree; onActivated: backend.undo() }
    Shortcut { sequences: [StandardKey.Redo]; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree; onActivated: backend.redo() }
    Shortcut { sequence: "Left"; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree && !win.fullView; onActivated: win.move(-1, 0) }
    Shortcut { sequence: "Right"; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree && !win.fullView; onActivated: win.move(1, 0) }
    Shortcut { sequence: "Up"; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree && !win.fullView; onActivated: win.move(0, -1) }
    Shortcut { sequence: "Down"; context: Qt.ApplicationShortcut; enabled: win.actionKeysFree && !win.fullView; onActivated: win.move(0, 1) }
    // Esc closes what is open, leaves a text field, cancels the batch, or
    // goes back from variations to plain batches, in that order. Dialogs
    // handle their own Esc.
    Shortcut {
        sequence: "Esc"
        context: Qt.ApplicationShortcut
        enabled: !win.dialogOpen
        onActivated: {
            if (win.helpVisible)
                win.helpVisible = false
            else if (win.fullView)
                win.fullView = false
            else if (win.libraryVisible)
                win.libraryVisible = false
            else if (win.typing)
                win.leaveFields()
            else if (backend.running || backend.batchWaiting)
                backend.cancel()
            else
                backend.clearSource()
        }
    }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            PaletteBar {
                Layout.fillWidth: true
                onModelClicked: if (!backend.busy) modelPicker.open()
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                BackgroundColumn {
                    Layout.preferredWidth: 196
                    Layout.fillHeight: true
                }

                Item {
                    Layout.fillWidth: true
                    Layout.fillHeight: true

                    SetupPanel {
                        anchors.fill: parent
                        visible: win.setupNeeded
                        onChooseModel: modelPicker.open()
                    }

                    DraftGrid {
                        id: grid
                        anchors.fill: parent
                        anchors.margins: 16
                        visible: !win.setupNeeded
                        focus: true
                        selected: win.selected
                        onSelect: (index) => win.selected = index
                        onOpen: (index) => {
                            win.selected = index
                            if (backend.drafts[index].state === "done")
                                win.fullView = true
                        }
                    }

                    LibraryView {
                        id: libraryView
                        anchors.fill: parent
                        visible: win.libraryVisible
                        selected: win.librarySelected
                        onSelect: (index) => win.librarySelected = index
                        onOpen: (index) => {
                            win.librarySelected = index
                            win.fullView = true
                        }
                    }
                }
            }

            PromptBar {
                id: promptBar
                Layout.fillWidth: true
                promptVisible: win.promptVisible
                onSubmitted: {
                    win.leaveFields()
                    backend.generate()
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
                    text: backend.status
                    wrapMode: Text.Wrap
                    maximumLineCount: 3
                    elide: Text.ElideRight
                    color: backend.statusIsError ? "#ffb4a9" : "#9a9aa0"
                }
                Label {
                    text: backend.estimate
                    color: "#9a9aa0"
                }
            }
        }

        FullView {
            anchors.fill: parent
            visible: win.fullView && win.shownDraft !== null && win.shownDraft.state === "done"
            draft: win.shownDraft
            onClosed: win.fullView = false
        }

        HelpOverlay {
            anchors.fill: parent
            visible: win.helpVisible
        }
    }

    ModelPicker {
        id: modelPicker
        anchors.centerIn: Overlay.overlay
    }

    Dialog {
        id: quitDialog
        anchors.centerIn: Overlay.overlay
        modal: true
        title: qsTr("Quit limgen?")
        width: Math.min(win.width - 64, 460)
        Overlay.modal: Rectangle { color: "#a0000000" }

        Label {
            width: parent.width
            wrapMode: Text.Wrap
            text: backend.running || backend.batchWaiting
                  ? qsTr("A batch is running. Quitting stops it, and its drafts are not kept.")
                  : backend.keeping
                    ? qsTr("A kept draft is still being finished. Quitting stops it; keep the draft again to finish it.")
                    : qsTr("A download is running. Quitting pauses it; it resumes where it stopped next time.")
        }

        footer: DialogButtonBox {
            Button {
                text: qsTr("Keep working")
                flat: true
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            Button {
                text: qsTr("Quit")
                highlighted: true
                DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
            }
        }
        onAccepted: {
            backend.stopAll()
            win.quitting = true
            Qt.quit()
        }
    }

    Dialog {
        id: themeDialog
        property int index: -1
        property string madeFor
        property string current
        property bool set: false
        anchors.centerIn: Overlay.overlay
        modal: true
        title: qsTr("Which theme should it go to?")
        width: Math.min(win.width - 64, 500)
        Overlay.modal: Rectangle { color: "#a0000000" }

        Label {
            width: parent.width
            wrapMode: Text.Wrap
            text: qsTr("This draft was made for %1, and the theme is now %2.").arg(themeDialog.madeFor).arg(themeDialog.current)
        }

        footer: DialogButtonBox {
            Button {
                text: qsTr("Cancel")
                flat: true
                DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
            }
            Button {
                text: themeDialog.madeFor
                flat: true
                onClicked: {
                    backend.keepInto(themeDialog.index, themeDialog.madeFor, themeDialog.set)
                    themeDialog.close()
                }
            }
            Button {
                text: themeDialog.current
                highlighted: true
                onClicked: {
                    backend.keepInto(themeDialog.index, themeDialog.current, themeDialog.set)
                    themeDialog.close()
                }
            }
        }
    }
}
