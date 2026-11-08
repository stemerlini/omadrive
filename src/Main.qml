import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts
import QtCore
import Omadrive

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

    // Tabs are { remote, folder, selected } objects. One pane shows the
    // current tab; F3 opens a second pane beside it for copying between two.
    property var tabs: []
    property int leftTab: 0
    property int rightTab: 1
    property bool split: false
    property bool leftActive: true
    property bool showHidden: false
    property bool showTree: true
    readonly property var activePane: leftActive || !split ? leftPane : rightPane
    readonly property var otherPane: activePane === leftPane ? rightPane : leftPane
    readonly property int activeTab: activePane === leftPane ? leftTab : rightTab
    property string message: ""
    property bool messageIsError: false
    // Ctrl+C / Ctrl+X: { paths, remote, move }
    property var clip: null

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

    Component { id: localListType; FolderList {} }
    Component { id: cloudListType; OneDriveList {} }
    Component { id: tabType; TabState {} }

    function makeTab(remote, path, selected) {
        var folder = (remote ? cloudListType : localListType).createObject(win)
        if (!remote)
            folder.showHidden = showHidden
        folder.path = path || (remote ? "/" : backend.homePath())
        return tabType.createObject(win, { remote: remote, folder: folder, selected: selected || "" })
    }

    function tabLabel(tab) {
        var path = tab.folder.path
        if (tab.remote && path === "/")
            return qsTr("OneDrive")
        if (!tab.remote && path === backend.homePath())
            return qsTr("Home")
        return path === "/" ? "/" : path.split("/").pop()
    }

    function countOf(remote) {
        return tabs.filter(function (t) { return t.remote === remote }).length
    }

    function rememberSelections() {
        leftPane.remember()
        if (split)
            rightPane.remember()
    }

    // Shows a tab in the focused pane. A tab already open in the other pane
    // just takes the focus there.
    function showTab(index) {
        if (index < 0 || index >= tabs.length)
            return
        if (split && index === (activePane === leftPane ? rightTab : leftTab)) {
            switchPane()
            return
        }
        activePane.remember()
        if (activePane === leftPane)
            leftTab = index
        else
            rightTab = index
        if (tabs[index].remote && account.state === "signedOut")
            account.signIn()
        activePane.focusList()
    }

    function cycleTab(step) {
        var index = activeTab
        for (var i = 0; i < tabs.length; i++) {
            index = (index + step + tabs.length) % tabs.length
            if (!split || index !== (activePane === leftPane ? rightTab : leftTab))
                break
        }
        showTab(index)
    }

    // Tab in a single pane flips between this device and OneDrive, landing on
    // the nearest tab of the other kind.
    function toggleLocalCloud() {
        var wanted = !tabs[activeTab].remote
        for (var d = 1; d < tabs.length; d++) {
            for (var s = -1; s <= 1; s += 2) {
                var i = activeTab + s * d
                if (i >= 0 && i < tabs.length && tabs[i].remote === wanted) {
                    showTab(i)
                    return
                }
            }
        }
    }

    function newTab(remote, path) {
        rememberSelections()
        tabs = tabs.concat([makeTab(remote, path, "")])
        showTab(tabs.length - 1)
    }

    function duplicateTab() {
        var tab = tabs[activeTab]
        newTab(tab.remote, tab.folder.path)
    }

    function closeTab(index) {
        var tab = tabs[index]
        if (countOf(tab.remote) <= 1) {
            say(tab.remote ? qsTr("The last OneDrive tab stays open") : qsTr("The last local tab stays open"), false)
            return
        }
        rememberSelections()
        // A pane showing the closed tab moves to its neighbour, skipping the
        // tab in the other pane.
        function fallback(shown, other) {
            if (shown !== index)
                return shown > index ? shown - 1 : shown
            for (var d = 1; d < tabs.length; d++) {
                var candidates = [index + d, index - d]
                for (var c = 0; c < 2; c++) {
                    var i = candidates[c]
                    if (i >= 0 && i < tabs.length && i !== index && (!split || i !== other))
                        return i > index ? i - 1 : i
                }
            }
            return 0
        }
        var left = fallback(leftTab, rightTab)
        var right = split ? fallback(rightTab, leftTab) : (rightTab > index ? rightTab - 1 : Math.min(rightTab, tabs.length - 2))
        tabs = tabs.slice(0, index).concat(tabs.slice(index + 1))
        leftTab = left
        rightTab = right
        tab.folder.destroy()
        tab.destroy()
        activePane.focusList()
    }

    function toggleSplit() {
        rememberSelections()
        if (split) {
            if (activePane === rightPane)
                leftTab = rightTab
            split = false
            leftActive = true
        } else {
            // The second pane starts on the other side: OneDrive next to a
            // local folder and the other way round.
            var wanted = !tabs[leftTab].remote
            var pick = -1
            for (var i = 0; i < tabs.length && pick < 0; i++)
                if (i !== leftTab && tabs[i].remote === wanted)
                    pick = i
            for (i = 0; i < tabs.length && pick < 0; i++)
                if (i !== leftTab)
                    pick = i
            if (pick < 0) {
                tabs = tabs.concat([makeTab(wanted, "", "")])
                pick = tabs.length - 1
            }
            rightTab = pick
            split = true
            leftActive = false
        }
        activePane.focusList()
    }

    function switchPane() {
        if (!split) {
            toggleLocalCloud()
            return
        }
        leftActive = !leftActive
        activePane.focusList()
    }

    function refreshLocal() {
        tabs.forEach(function (t) { if (!t.remote) t.folder.refresh() })
    }

    function recheckCloud(fetch) {
        tabs.forEach(function (t) {
            if (t.remote) {
                t.folder.recheck()
                if (fetch)
                    t.folder.refresh()
            }
        })
    }

    function clearMarks() {
        leftPane.clearMarks()
        rightPane.clearMarks()
    }

    function saveTabs() {
        rememberSelections()
        settings.tabs = JSON.stringify(tabs.map(function (t) {
            return { remote: t.remote, path: t.folder.path, selected: t.selected }
        }))
        settings.leftTab = leftTab
        settings.rightTab = rightTab
        settings.split = split
        settings.leftActive = leftActive
        settings.showHidden = showHidden
        settings.showTree = showTree
    }

    function restoreTabs() {
        showHidden = settings.showHidden
        showTree = settings.showTree
        var saved = []
        // Ctrl+Shift+N opens a window with just the starting tabs.
        if (!freshStart)
            try { saved = JSON.parse(settings.tabs) } catch (e) {}
        var list = []
        saved.forEach(function (t) {
            if (t.remote || backend.isDir(t.path))
                list.push(makeTab(!!t.remote, t.path, t.selected))
        })
        if (!list.some(function (t) { return !t.remote }))
            list.unshift(makeTab(false, "", ""))
        if (!list.some(function (t) { return t.remote }))
            list.push(makeTab(true, "", ""))
        tabs = list
        var clamp = function (i) { return Math.max(0, Math.min(i, list.length - 1)) }
        leftTab = clamp(settings.leftTab)
        rightTab = clamp(settings.rightTab)
        split = settings.split && rightTab !== leftTab
        leftActive = settings.leftActive || !split
    }

    Settings {
        id: settings
        location: StandardPaths.writableLocation(StandardPaths.AppConfigLocation) + "/tabs.conf"
        category: "tabs"
        property string tabs: ""
        property int leftTab: 0
        property int rightTab: 1
        property bool split: false
        property bool leftActive: true
        property bool showHidden: false
        property bool showTree: true
    }

    Component.onCompleted: {
        restoreTabs()
        activePane.focusList()
    }
    onClosing: saveTabs()

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
            clearMarks()
            refreshLocal()
            recheckCloud(false)
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
        // Alt+C/Alt+M out of OneDrive keep the cloud layout: they land in the mirror.
        if (activePane.remote && (!split || !otherPane.remote)) {
            var items = activePane.targets()
            if (items.length === 0)
                return
            if (!account.signedIn)
                say(qsTr("Sign in to OneDrive first"), true)
            else
                transfer.downloadToMirror(items, removeSource)
            return
        }
        if (!split) {
            toggleSplit()
            say(qsTr("Choose the destination in the second pane, then press Alt+C or Alt+M again"), false)
            return
        }
        transferPaths(activePane.targets(), activePane.remote, otherPane.remote, otherPane.folder.path, removeSource)
    }

    function clipboardPut(move) {
        var items = activePane.targets()
        if (items.length === 0)
            return
        clip = { paths: items, remote: activePane.remote, move: move }
        say(move ? qsTr("Cut %n items; paste with Ctrl+V", "", items.length)
                 : qsTr("Copied %n items; paste with Ctrl+V", "", items.length), false)
    }

    function copyPaths() {
        var items = activePane.targets()
        if (items.length === 0)
            items = [activePane.folder.path]
        backend.copyText(items.join("\n"))
        say(qsTr("Copied %n paths", "", items.length), false)
    }

    function paste() {
        if (!clip)
            return
        var c = clip
        if (c.move)
            clip = null
        transferPaths(c.paths, c.remote, activePane.remote, activePane.folder.path, c.move)
    }

    // Dropping onto a tab drops into the folder that tab shows.
    function dropOnTab(drop, tab) {
        var paths = []
        var fromRemote = false
        if (drop.formats.indexOf("application/x-omadrive-items") >= 0) {
            var data = JSON.parse(drop.getDataAsString("application/x-omadrive-items"))
            paths = data.paths
            fromRemote = data.remote
        } else if (drop.hasUrls) {
            for (var i = 0; i < drop.urls.length; i++) {
                var url = drop.urls[i].toString()
                if (url.indexOf("file://") === 0)
                    paths.push(decodeURIComponent(url.substring(7)))
            }
        }
        if (paths.length === 0)
            return
        var move = drop.proposedAction === Qt.MoveAction
        drop.accept(move ? Qt.MoveAction : Qt.CopyAction)
        transferPaths(paths, fromRemote, tab.remote, tab.folder.path, move)
    }

    function paneAction(action) {
        if (action === "open") activePane.openCurrent()
        else if (action === "copy") copy()
        else if (action === "move") move()
        else if (action === "rename") askRename()
        else if (action === "newMenu") newPopup.open()
        else if (action.startsWith("new:")) askNew(Number(action.slice(4)))
        else if (action === "trash") askTrash()
        else if (action === "preview") togglePreview()
        else if (action === "mirror") transfer.downloadToMirror(activePane.targets(), false)
        else if (action === "clipCopy") clipboardPut(false)
        else if (action === "clipCut") clipboardPut(true)
        else if (action === "paste") paste()
        else if (action === "copyPath") copyPaths()
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

    onActiveChanged: if (active) recheckCloud(false)

    function copy() { transferItems(false) }
    function move() { transferItems(true) }

    property var newKind: null
    function askNew(index) {
        newKind = activePane.newKinds[index]
        if (newKind.folder)
            prompt.ask("mkdir", qsTr("New folder"), qsTr("Name of the new folder"), true, newKind.name)
        else
            prompt.ask("mkfile", qsTr("New file"), qsTr("Name of the new file"), true, newKind.name)
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
        } else if (action === "mkfile") {
            transfer.makeFile(activePane.folder.path, text.trim(), newKind.content)
        } else if (action === "rename") {
            transfer.renameItem(activePane.currentPath(), text.trim())
        } else if (action === "trash") {
            transfer.trash(activePane.targets())
        }
    }

    function runPrompt(action, text) {
        // Highlight what was just made once the folder reloads.
        if (action === "mkdir" || action === "mkfile")
            activePane.lastPath = activePane.folder.path.replace(/\/$/, "") + "/" + text.trim()
        if (activePane.remote) {
            runRemotePrompt(action, text)
            return
        }
        if (action === "mkdir") {
            report(backend.makeFolder(activePane.folder.path, text.trim()), qsTr("Folder created"))
        } else if (action === "mkfile") {
            report(backend.makeFile(activePane.folder.path, text.trim(), newKind.content, newKind.executable === true), qsTr("File created"))
        } else if (action === "rename") {
            report(backend.renameItem(activePane.currentPath(), text.trim()), qsTr("Renamed"))
        } else if (action === "trash") {
            var items = activePane.targets()
            report(backend.trashItems(items), qsTr("Moved %n items to the trash", "", items.length))
            activePane.clearMarks()
        }
        refreshLocal()
        recheckCloud(false)
    }

    // Plain-letter shortcuts must not fire while a dialog takes text.
    readonly property bool typing: prompt.visible || newPopup.visible || leftPane.editingPath || rightPane.editingPath

    // Keys follow Windows and One Commander.
    Shortcut { sequences: ["?", "Ctrl+/"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: help.visible ? help.close() : help.open() }
    Shortcut { sequence: "Ctrl+Q"; context: Qt.ApplicationShortcut; onActivated: win.close() }
    Shortcut { sequence: "Ctrl+Shift+N"; context: Qt.ApplicationShortcut; onActivated: backend.newWindow() }
    Shortcut { sequence: "Tab"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.switchPane() }
    Shortcut { sequence: "Alt+F"; context: Qt.ApplicationShortcut; enabled: !win.typing && win.split; onActivated: win.switchPane() }
    Shortcut { sequence: "Space"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.togglePreview() }
    Shortcut { sequences: ["Ins", "Ctrl+Space"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.activePane.toggleMark() }
    Shortcut { sequence: "Esc"; context: Qt.ApplicationShortcut; enabled: !win.typing && (previewPopup.visible || win.activePane.markedCount > 0); onActivated: previewPopup.visible ? previewPopup.close() : win.activePane.clearMarks() }
    Shortcut { sequences: ["Return", "Enter", "Right"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.activePane.openCurrent() }
    Shortcut { sequences: ["Backspace", "Left", "Alt+Up"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.activePane.goUp() }
    Shortcut { sequence: "Alt+Left"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.activePane.tab.goBack() }
    Shortcut { sequence: "Alt+Right"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.activePane.tab.goForward() }
    Shortcut { sequences: ["Ctrl+L", "Alt+D"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.activePane.editPath() }
    Shortcut { sequence: "Alt+C"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.copy() }
    Shortcut { sequence: "Alt+M"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.move() }
    Shortcut { sequence: "Ctrl+N"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: newPopup.open() }
    Shortcut { sequence: "F2"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.askRename() }
    Shortcut { sequence: "Delete"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.askTrash() }
    Shortcut { sequences: ["Ctrl+R", "F5"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.activePane.folder.refresh() }
    Shortcut { sequences: ["`", "Ctrl+B"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.showTree = !win.showTree }
    Shortcut { sequence: "Ctrl+Shift+C"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.copyPaths() }
    Shortcut {
        sequence: "Ctrl+H"
        context: Qt.ApplicationShortcut
        enabled: !win.typing
        onActivated: {
            win.showHidden = !win.showHidden
            win.tabs.forEach(function (t) { if (!t.remote) t.folder.showHidden = win.showHidden })
        }
    }

    Connections {
        target: transfer
        function onFinished(error, text) {
            win.report(error, text)
            win.clearMarks()
            win.refreshLocal()
            win.recheckCloud(true)
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
    Shortcut { sequence: "Ctrl+\\"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.toggleSplit() }
    Shortcut { sequence: "Ctrl+T"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.duplicateTab() }
    Shortcut { sequence: "Ctrl+W"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.closeTab(win.activeTab) }
    Shortcut { sequences: ["Ctrl+Tab", "Ctrl+PgDown"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.cycleTab(1) }
    Shortcut { sequences: ["Ctrl+Shift+Tab", "Ctrl+Backtab", "Ctrl+PgUp"]; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.cycleTab(-1) }
    Shortcut { sequence: "Ctrl+C"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.clipboardPut(false) }
    Shortcut { sequence: "Ctrl+X"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.clipboardPut(true) }
    Shortcut { sequence: "Ctrl+V"; context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.paste() }
    Repeater {
        model: 9
        delegate: Item {
            required property int index
            Shortcut { sequence: "Ctrl+" + (index + 1); context: Qt.ApplicationShortcut; enabled: !win.typing; onActivated: win.showTab(index) }
        }
    }
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

            TabStrip {
                Layout.fillWidth: true
                tabs: win.tabs
                leftTab: win.leftTab
                rightTab: win.rightTab
                split: win.split
                activeTab: win.activeTab
                labelFor: win.tabLabel
                onActivated: index => win.showTab(index)
                onCloseRequested: index => win.closeTab(index)
                onNewTab: remote => win.newTab(remote, "")
                onSplitToggled: win.toggleSplit()
                onHelpRequested: help.open()
                onDropped: (drop, index) => win.dropOnTab(drop, win.tabs[index])
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 10

                FolderPane {
                    id: leftPane
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    tab: win.tabs.length ? win.tabs[win.leftTab] : placeholder
                    active: win.activePane === leftPane
                    paired: win.split
                    canPaste: win.clip !== null
                    onFocusRequested: { win.leftActive = true; focusList() }
                    onNotice: text => win.say(text, false)
                    onMenuAction: action => win.paneAction(action)
                    onOpenRemote: (path, cloud) => win.openRemote(path, cloud)
                    onItemsDropped: (paths, fromRemote, dest, move) => win.transferPaths(paths, fromRemote, remote, dest, move)
                    showTree: win.showTree
                    onTreeToggled: win.showTree = !win.showTree
                    onOpenInNewTab: path => win.newTab(remote, path)
                }

                FolderPane {
                    id: rightPane
                    visible: win.split
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    tab: win.tabs.length > win.rightTab ? win.tabs[win.rightTab] : placeholder
                    active: win.activePane === rightPane
                    paired: win.split
                    canPaste: win.clip !== null
                    onFocusRequested: { win.leftActive = false; focusList() }
                    onNotice: text => win.say(text, false)
                    onMenuAction: action => win.paneAction(action)
                    onOpenRemote: (path, cloud) => win.openRemote(path, cloud)
                    onItemsDropped: (paths, fromRemote, dest, move) => win.transferPaths(paths, fromRemote, remote, dest, move)
                    showTree: win.showTree
                    onTreeToggled: win.showTree = !win.showTree
                    onOpenInNewTab: path => win.newTab(remote, path)
                }
            }

        }
    }

    // Messages and OneDrive progress float over the bottom of the window
    // instead of taking a row of their own.
    Rectangle {
        id: toast
        readonly property string text: win.message !== "" ? win.message
            : transfer.busy ? (transfer.progress !== "" ? transfer.progress : qsTr("Working with OneDrive…")) : ""
        visible: text !== ""
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 34
        width: Math.min(toastLabel.implicitWidth + 32, win.width - 80)
        height: toastLabel.implicitHeight + 16
        radius: theme.radius
        color: "#1c1c20"
        border.color: win.messageIsError && win.message !== "" ? "#f7768e" : theme.accent
        Label {
            id: toastLabel
            anchors.centerIn: parent
            width: Math.min(implicitWidth, parent.width - 32)
            elide: Text.ElideRight
            text: toast.text
            color: win.messageIsError && win.message !== "" ? "#f7768e" : "#e6e6e6"
        }
        TapHandler { onTapped: win.message = "" }
    }

    // Shown by the panes for the moment before the saved tabs are restored.
    TabState {
        id: placeholder
        folder: FolderList {}
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
        padding: 16
        topPadding: 4
        standardButtons: Dialog.Ok | Dialog.Cancel

        background: Rectangle { color: "#18181b"; radius: theme.radius; border.color: "#2a2a2e" }
        header: Label {
            text: prompt.title
            padding: 16
            bottomPadding: 4
            font.bold: true
            color: "#e6e6e6"
        }
        footer: DialogButtonBox {
            standardButtons: prompt.standardButtons
            background: null
            onAccepted: prompt.accept()
            onRejected: prompt.reject()
        }

        function ask(newAction, newTitle, message, input, initial) {
            action = newAction
            title = newTitle
            text = message
            withInput = input
            field.text = initial
            open()
            if (input) {
                field.forceActiveFocus()
                var dot = initial.lastIndexOf(".")
                if (newAction !== "mkdir" && dot > 0)
                    field.select(0, dot)
                else
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

    // Ctrl+N: pick what to create, by arrows and Enter, the underlined
    // letter, or a click.
    Popup {
        id: newPopup
        anchors.centerIn: Overlay.overlay
        modal: true
        padding: 10
        width: 300
        property int chosen: -1

        function choose(index) {
            chosen = index
            close()
        }

        onOpened: { chosen = -1; kindList.currentIndex = 0; kindList.forceActiveFocus() }
        // The name prompt opens only once this has closed, so it keeps the focus.
        onClosed: chosen >= 0 ? win.askNew(chosen) : win.activePane.focusList()

        background: Rectangle { color: "#18181b"; radius: theme.radius; border.color: "#2a2a2e" }

        contentItem: Column {
            spacing: 6
            Label {
                leftPadding: 8
                text: qsTr("New")
                font.bold: true
                color: "#9a9aa0"
            }
            ListView {
                id: kindList
                width: parent.width
                height: contentHeight
                interactive: false
                keyNavigationEnabled: true
                highlightMoveDuration: 0
                model: win.activePane.newKinds
                highlight: Rectangle {
                    color: Qt.alpha(theme.accent, 0.22)
                    border.color: Qt.alpha(theme.accent, 0.7)
                    radius: Math.min(6, theme.radius)
                }
                Keys.onReturnPressed: newPopup.choose(currentIndex)
                Keys.onEnterPressed: newPopup.choose(currentIndex)
                Keys.onPressed: event => {
                    if (event.modifiers & (Qt.ControlModifier | Qt.AltModifier))
                        return
                    for (var i = 0; i < count; ++i) {
                        var label = model[i].label
                        if (event.key === label.charAt(label.indexOf("&") + 1).toUpperCase().charCodeAt(0)) {
                            event.accepted = true
                            newPopup.choose(i)
                            return
                        }
                    }
                }

                delegate: Item {
                    id: kind
                    required property var modelData
                    required property int index
                    readonly property bool current: ListView.isCurrentItem
                    readonly property int key: modelData.label.indexOf("&")
                    width: ListView.view.width
                    height: 38

                    Image {
                        id: kindIcon
                        x: 10
                        anchors.verticalCenter: parent.verticalCenter
                        source: "qrc:/icons/" + kind.modelData.icon + ".svg"
                        sourceSize: Qt.size(20, 20)
                        opacity: kind.current ? 1 : 0.7
                    }
                    Label {
                        anchors.left: kindIcon.right
                        anchors.leftMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        textFormat: Text.StyledText
                        text: kind.modelData.label.slice(0, kind.key) + "<u>" + kind.modelData.label.charAt(kind.key + 1) + "</u>"
                              + kind.modelData.label.slice(kind.key + 2)
                        color: kind.current ? theme.accent : "#e6e6e6"
                    }
                    Label {
                        anchors.right: parent.right
                        anchors.rightMargin: 12
                        anchors.verticalCenter: parent.verticalCenter
                        text: kind.modelData.label.charAt(kind.key + 1).toUpperCase()
                        color: "#5a5a60"
                    }
                    HoverHandler { onHoveredChanged: if (hovered) kindList.currentIndex = kind.index }
                    TapHandler { onTapped: newPopup.choose(kind.index) }
                }
            }
        }
    }

    Popup {
        id: help
        anchors.centerIn: Overlay.overlay
        modal: true
        padding: 20
        onClosed: win.activePane.focusList()

        background: Rectangle { color: "#18181b"; radius: theme.radius; border.color: "#2a2a2e" }

        Grid {
            columnSpacing: 32
            rowSpacing: 6
            flow: Grid.TopToBottom
            rows: 14
            Repeater {
                model: [
                    ["Tab", qsTr("Local ⇄ OneDrive (with two panes: switch pane)")],
                    ["Ctrl+\\", qsTr("One or two panes")],
                    ["Alt+F", qsTr("Switch pane")],
                    ["Ctrl+T / Ctrl+W", qsTr("New tab here / close tab")],
                    ["Ctrl+Tab / Ctrl+Shift+Tab", qsTr("Next or previous tab")],
                    ["Ctrl+1…9", qsTr("Go to tab")],
                    ["Middle-click", qsTr("Folder: open in new tab; tab: close")],
                    ["Ctrl+Shift+N", qsTr("New window")],
                    ["` / Ctrl+B", qsTr("Folder columns or thin path strips")],
                    ["Enter / →", qsTr("Open folder or file")],
                    ["Backspace / Alt+↑", qsTr("Parent folder")],
                    ["Alt+← / Alt+→", qsTr("Back / forward")],
                    ["Ctrl+L / Alt+D", qsTr("Type a path")],
                    ["Space", qsTr("Preview")],
                    ["Ctrl+Space / Ins", qsTr("Mark or unmark")],
                    ["Ctrl+A", qsTr("Select all")],
                    ["Ctrl+C / X / V", qsTr("Copy, cut, paste (across tabs too)")],
                    ["Ctrl+Shift+C", qsTr("Copy path")],
                    ["Alt+C", qsTr("Copy to the other pane (from OneDrive: into %1)").arg(mirror.root.replace(backend.homePath(), "~"))],
                    ["Alt+M", qsTr("Move to the other pane (from OneDrive: into %1)").arg(mirror.root.replace(backend.homePath(), "~"))],
                    ["Drag and drop", qsTr("Copy onto a pane, folder or tab; Shift moves")],
                    ["Ctrl+N", qsTr("New file or folder")],
                    ["F2", qsTr("Rename")],
                    ["Delete", qsTr("Move to the trash")],
                    ["Ctrl+R / F5", qsTr("Refresh")],
                    ["Ctrl+H", qsTr("Show hidden files")],
                    ["Ctrl+Shift+L", qsTr("Sign out of OneDrive")],
                    ["Ctrl+Q", qsTr("Quit")]
                ]
                delegate: Row {
                    required property var modelData
                    spacing: 16
                    Label { width: 150; text: parent.modelData[0]; color: theme.accent; font.bold: true; elide: Text.ElideRight }
                    Label { width: 270; text: parent.modelData[1]; wrapMode: Text.Wrap }
                }
            }
        }
    }
}
