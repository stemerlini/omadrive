import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

ApplicationWindow {
    id: win
    width: 1200
    height: 720
    minimumWidth: 720
    minimumHeight: 460
    visible: true
    title: "Omadrive"
    color: "#0e0e10"

    Material.theme: Material.Dark
    Material.accent: theme.accent
    Material.roundedScale: theme.radius

    property bool leftActive: true
    property bool rightRemote: true
    readonly property var activePane: leftActive ? leftPane : rightPane
    readonly property var otherPane: leftActive ? rightPane : leftPane
    property string message: ""
    property bool messageIsError: false

    function say(text, isError) {
        message = text
        messageIsError = isError
        messageTimer.restart()
    }

    function report(error, doneText) {
        if (error === "")
            say(doneText, false)
        else
            say(error, true)
    }

    function switchPane() {
        leftActive = !leftActive
        activePane.focusList()
    }

    // Left is always the local disk; the right pane is OneDrive or a second
    // local folder (Ctrl+O). OneDrive work runs in the background and ends in
    // transfer.onFinished.
    function crossing() { return activePane.remote !== otherPane.remote }

    function transferPaths(items, fromRemote, toRemote, destPath, removeSource) {
        // Dropping onto the same folder, or a folder into itself, does nothing.
        items = items.filter(function (p) {
            var parent = p.substring(0, p.lastIndexOf("/")) || "/"
            return destPath !== p && destPath.indexOf(p + "/") !== 0
                && !(fromRemote === toRemote && destPath === parent)
        })
        if (items.length === 0)
            return
        if (!fromRemote && !toRemote) {
            var error = removeSource ? backend.moveItems(items, destPath) : backend.copyItems(items, destPath)
            report(error, removeSource ? qsTr("Moved %n items", "", items.length) : qsTr("Copied %n items", "", items.length))
            leftPane.clearMarks()
            rightPane.clearMarks()
            leftFolder.refresh()
            rightFolder.refresh()
            oneDrive.recheck()
        } else if (!account.signedIn) {
            say(qsTr("Sign in to OneDrive first"), true)
        } else if (fromRemote && toRemote) {
            say(qsTr("Copying inside OneDrive is not supported yet"), true)
        } else if (fromRemote) {
            transfer.download(items, destPath, removeSource)
        } else {
            transfer.upload(items, destPath, removeSource)
        }
    }

    function transferItems(removeSource) {
        // F5/F6 out of OneDrive keep the cloud layout: they land in the mirror.
        if (activePane.remote && !otherPane.remote) {
            var items = activePane.targets()
            if (items.length === 0)
                return
            if (!account.signedIn)
                say(qsTr("Sign in to OneDrive first"), true)
            else
                transfer.downloadToMirror(items, removeSource)
            return
        }
        transferPaths(activePane.targets(), activePane.remote, otherPane.remote, otherPane.folder.path, removeSource)
    }

    function paneAction(action) {
        if (action === "open") activePane.openCurrent()
        else if (action === "copy") copy()
        else if (action === "move") move()
        else if (action === "rename") askRename()
        else if (action === "mkdir") askNewFolder()
        else if (action === "trash") askTrash()
        else if (action === "preview") togglePreview()
        else if (action === "mirror") transfer.downloadToMirror(activePane.targets(), false)
    }

    // A OneDrive file opens from the mirror, downloading it first if needed.
    property string pendingOpen: ""
    function openRemote(path, cloud) {
        if (cloud === "local") {
            backend.openExternally(mirror.pathFor(path))
        } else if (cloud === "changed") {
            backend.openExternally(mirror.pathFor(path))
            say(qsTr("Opened the copy on this device; it differs from OneDrive"), false)
        } else {
            pendingOpen = path
            transfer.downloadToMirror([path], false)
        }
    }

    function togglePreview() {
        if (previewPopup.visible) {
            previewPopup.close()
        } else {
            previewPopup.open()
            preview.show(activePane.currentItemPath, activePane.remote)
        }
    }

    // While the preview is open it follows the highlight, after a short pause
    // so holding an arrow key does not fetch every file on the way.
    readonly property string previewKey: activePane.remote + "|" + activePane.currentItemPath
    onPreviewKeyChanged: if (previewPopup.visible) previewTimer.restart()
    Timer {
        id: previewTimer
        interval: 150
        onTriggered: preview.show(win.activePane.currentItemPath, win.activePane.remote)
    }

    onActiveChanged: if (active) oneDrive.recheck()

    function copy() { transferItems(false) }
    function move() { transferItems(true) }

    function toggleRight() {
        rightRemote = !rightRemote
        rightPane.clearMarks()
        if (rightRemote && account.state === "signedOut")
            account.signIn()
    }

    function askNewFolder() {
        prompt.ask("mkdir", qsTr("New folder"), qsTr("Name of the new folder"), true, "")
    }

    function askRename() {
        var path = activePane.currentPath()
        if (path)
            prompt.ask("rename", qsTr("Rename"), path.split("/").pop(), true, path.split("/").pop())
    }

    function askTrash() {
        var items = activePane.targets()
        if (items.length > 0)
            prompt.ask("trash", qsTr("Trash"), qsTr("Move %n items to the trash?", "", items.length), false, "")
    }

    function runRemotePrompt(action, text) {
        if (!account.signedIn) {
            say(qsTr("Sign in to OneDrive first"), true)
        } else if (action === "mkdir") {
            transfer.makeFolder(activePane.folder.path, text.trim())
        } else if (action === "rename") {
            transfer.renameItem(activePane.currentPath(), text.trim())
        } else if (action === "trash") {
            transfer.trash(activePane.targets())
        }
    }

    function runPrompt(action, text) {
        if (activePane.remote) {
            runRemotePrompt(action, text)
            return
        }
        if (action === "mkdir") {
            report(backend.makeFolder(activePane.folder.path, text.trim()), qsTr("Folder created"))
        } else if (action === "rename") {
            report(backend.renameItem(activePane.currentPath(), text.trim()), qsTr("Renamed"))
        } else if (action === "trash") {
            var items = activePane.targets()
            report(backend.trashItems(items), qsTr("Moved %n items to the trash", "", items.length))
            activePane.clearMarks()
        }
        activePane.folder.refresh()
        otherPane.folder.refresh()
        oneDrive.recheck()
    }

    // Plain-letter shortcuts must not fire while a dialog takes text.
    readonly property bool typing: prompt.visible

    Shortcut { sequence: "?"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: help.visible ? help.close() : help.open() }
    Shortcut { sequence: "Q"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: Qt.quit() }
    Shortcut { sequence: "Tab"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.switchPane() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.togglePreview() }
    Shortcut { sequences: ["Ins", "Ctrl+Space"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.activePane.toggleMark() }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: previewPopup.visible && !win.typing; onActivated: previewPopup.close() }
    Shortcut { sequences: ["Return", "Enter", "Right"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.activePane.openCurrent() }
    Shortcut { sequences: ["Backspace", "Left"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.activePane.goUp() }
    Shortcut { sequence: "F5"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.copy() }
    Shortcut { sequence: "F6"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.move() }
    Shortcut { sequence: "F7"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.askNewFolder() }
    Shortcut { sequence: "F2"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.askRename() }
    Shortcut { sequences: ["F8", "Delete"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.askTrash() }
    Shortcut {
        sequence: "Ctrl+H"
        context: Qt.ApplicationShortcut
        enabled: !win.typing
        onActivated: {
            var show = !leftFolder.showHidden
            leftFolder.showHidden = show
            rightFolder.showHidden = show
        }
    }

    Connections {
        target: transfer
        function onFinished(error, text) {
            win.report(error, text)
            leftPane.clearMarks()
            rightPane.clearMarks()
            leftFolder.refresh()
            rightFolder.refresh()
            oneDrive.recheck()
            oneDrive.refresh()
            if (win.pendingOpen !== "") {
                if (error === "")
                    backend.openExternally(mirror.pathFor(win.pendingOpen))
                win.pendingOpen = ""
            }
        }
    }

    Connections {
        target: account
        function onStateChanged() {
            if (account.error !== "")
                win.say(account.error, true)
            else if (account.signedIn)
                win.say(qsTr("Signed in as %1").arg(account.name), false)
        }
    }

    Shortcut { sequence: "Ctrl+A"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.activePane.markAll() }
    Shortcut { sequence: "Ctrl+O"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.toggleRight() }
    Shortcut { sequence: "Ctrl+Shift+L"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: account.signOut() }

    Timer { id: messageTimer; interval: 6000; onTriggered: win.message = "" }

    // Right-to-left languages mirror the window; the Translator sets the
    // application's layout direction.
    Item {
        anchors.fill: parent
        LayoutMirroring.enabled: Qt.application.layoutDirection === Qt.RightToLeft
        LayoutMirroring.childrenInherit: true

        ColumnLayout {
            anchors.fill: parent
            anchors.margins: 10
            spacing: 8

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 10

                FolderPane {
                    id: leftPane
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    folder: leftFolder
                    title: qsTr("Local")
                    active: win.leftActive
                    onFocusRequested: { win.leftActive = true; focusList() }
                    onNotice: text => win.say(text, false)
                    onMenuAction: action => win.paneAction(action)
                    onOpenRemote: (path, cloud) => win.openRemote(path, cloud)
                    onItemsDropped: (paths, fromRemote, dest, move) => win.transferPaths(paths, fromRemote, remote, dest, move)
                }

                FolderPane {
                    id: rightPane
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    folder: win.rightRemote ? oneDrive : rightFolder
                    remote: win.rightRemote
                    title: win.rightRemote ? qsTr("OneDrive") : qsTr("Local")
                    active: !win.leftActive
                    onFocusRequested: { win.leftActive = false; focusList() }
                    onNotice: text => win.say(text, false)
                    onMenuAction: action => win.paneAction(action)
                    onOpenRemote: (path, cloud) => win.openRemote(path, cloud)
                    onItemsDropped: (paths, fromRemote, dest, move) => win.transferPaths(paths, fromRemote, remote, dest, move)
                }
            }

            RowLayout {
                Layout.fillWidth: true
                spacing: 4
                Repeater {
                    model: [
                        ["F5", qsTr("Copy"), "copy"], ["F6", qsTr("Move"), "move"],
                        ["F7", qsTr("New folder"), "mkdir"], ["F2", qsTr("Rename"), "rename"],
                        ["F8", qsTr("Trash"), "trash"]
                    ]
                    delegate: Button {
                        required property var modelData
                        flat: true
                        text: modelData[0] + "  " + modelData[1]
                        focusPolicy: Qt.NoFocus
                        onClicked: win.paneAction(modelData[2])
                    }
                }
                Button { flat: true; text: "?"; focusPolicy: Qt.NoFocus; onClicked: help.open() }
                Label {
                    Layout.fillWidth: true
                    Layout.leftMargin: 8
                    elide: Text.ElideRight
                    text: win.message !== "" ? win.message
                        : transfer.busy ? (transfer.progress !== "" ? transfer.progress : qsTr("Working with OneDrive…"))
                        : qsTr("Space preview · Ins mark · drag between panes, Shift moves")
                    color: win.message === "" ? "#9a9aa0" : win.messageIsError ? "#f7768e" : theme.accent
                }
            }
        }
    }

    PreviewPopup {
        id: previewPopup
        onClosed: { preview.clear(); win.activePane.focusList() }
    }

    Dialog {
        id: prompt
        property string action: ""
        property bool withInput: true
        property string text: ""
        anchors.centerIn: Overlay.overlay
        modal: true
        width: Math.min(win.width - 40, 460)
        standardButtons: Dialog.Ok | Dialog.Cancel

        function ask(newAction, newTitle, message, input, initial) {
            action = newAction
            title = newTitle
            text = message
            withInput = input
            field.text = initial
            open()
            if (input) {
                field.forceActiveFocus()
                field.selectAll()
            }
        }

        onAccepted: win.runPrompt(action, field.text)
        onClosed: win.activePane.focusList()

        ColumnLayout {
            anchors.fill: parent
            Label { Layout.fillWidth: true; text: prompt.text; wrapMode: Text.Wrap }
            TextField {
                id: field
                Layout.fillWidth: true
                visible: prompt.withInput
                onAccepted: prompt.accept()
            }
        }
    }

    Popup {
        id: help
        anchors.centerIn: Overlay.overlay
        modal: true
        padding: 20
        onClosed: win.activePane.focusList()

        Column {
            spacing: 6
            Repeater {
                model: [
                    ["Tab", qsTr("Switch pane")],
                    ["↑ ↓", qsTr("Move")],
                    ["Enter / →", qsTr("Open folder or file")],
                    ["Backspace / ←", qsTr("Parent folder")],
                    ["Space", qsTr("Preview")],
                    ["Ins / Ctrl+Space", qsTr("Mark or unmark")],
                    ["Ctrl+A", qsTr("Mark all")],
                    ["Click / Ctrl / Shift+click", qsTr("Select, toggle mark, mark range")],
                    ["Double-click", qsTr("Open")],
                    ["Right-click", qsTr("Context menu")],
                    ["Drag and drop", qsTr("Copy; hold Shift to move")],
                    ["F5", qsTr("Copy to the other pane (from OneDrive: into %1)").arg(mirror.root.replace(backend.homePath(), "~"))],
                    ["F6", qsTr("Move to the other pane (from OneDrive: into %1)").arg(mirror.root.replace(backend.homePath(), "~"))],
                    ["F7", qsTr("New folder")],
                    ["F2", qsTr("Rename")],
                    ["F8 / Delete", qsTr("Move to the trash")],
                    ["Ctrl+H", qsTr("Show hidden files")],
                    ["Ctrl+O", qsTr("Right pane: OneDrive or local")],
                    ["Ctrl+Shift+L", qsTr("Sign out of OneDrive")],
                    ["Q", qsTr("Quit")]
                ]
                delegate: Row {
                    required property var modelData
                    spacing: 24
                    Label { width: 190; text: parent.modelData[0]; color: theme.accent; font.bold: true }
                    Label { text: parent.modelData[1] }
                }
            }
        }
    }
}
