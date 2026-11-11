import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// Shows one tab. Files are marked with Ins or Ctrl/Shift+click; actions apply
// to the marked files, or to the highlighted one when nothing is marked. Rows
// can be dragged to the other pane, a tab, a folder, or other apps.
Rectangle {
    id: pane

    // A tab: { folder, selected }. `selected` is the highlighted path,
    // restored when the tab is shown again.
    required property var tab
    readonly property var folder: tab.folder
    property bool active: false
    // Whether a second pane is open for Alt+C/Alt+M.
    property bool paired: false
    property bool canPaste: false
    property bool showTree: true
    // The path bar turns into a text field (Ctrl+L).
    property bool editingPath: false
    property var marked: ({})
    readonly property int markedCount: Object.keys(marked).length
    property string lastPath: ""
    property int lastIndex: 0
    property int anchorIndex: 0
    property bool resetting: false
    readonly property string currentItemPath: list.currentItem ? list.currentItem.path : ""
    readonly property string currentItemCloud: list.currentItem ? list.currentItem.cloud : ""
    readonly property var segments: folder.path.split("/").filter(function (s) { return s !== "" })

    signal focusRequested()
    signal notice(string text)
    signal menuAction(string action)
    signal itemsDropped(var paths, string destPath, bool move)
    signal openInNewTab(string path)
    signal treeToggled()

    // What Ctrl+N can create; Main.qml asks for the name and makes it.
    readonly property var newKinds: [
        { label: qsTr("&Folder"), icon: "new-folder", name: qsTr("New folder"), folder: true },
        { label: qsTr("&Text document"), icon: "new-text", name: qsTr("New text document.txt"), content: "" },
        { label: qsTr("&Markdown"), icon: "new-markdown", name: qsTr("New document.md"), content: "" },
        { label: qsTr("&Shell script"), icon: "new-script", name: "script.sh", content: "#!/usr/bin/env bash\n\n", executable: true },
        { label: qsTr("&Empty file"), icon: "new-file", name: qsTr("New file"), content: "" }
    ]

    color: "#131316"
    radius: theme.radius
    border.width: active || pageDrop.containsDrag ? 2 : 1
    border.color: pageDrop.containsDrag ? theme.accent : active ? theme.accent : "#2a2a2e"

    function focusList() { list.forceActiveFocus() }
    function remember() { tab.selected = currentItemPath }

    function restoreHighlight() {
        var index = folder.indexOfPath(lastPath)
        if (index < 0)
            index = Math.max(0, Math.min(lastIndex, list.count - 1))
        list.currentIndex = index
        resetting = false
    }

    onTabChanged: {
        resetting = true
        clearMarks()
        lastPath = tab.selected
        lastIndex = 0
        Qt.callLater(restoreHighlight)
    }
    function currentPath() { return folder.pathAt(list.currentIndex) }

    function targets() {
        var keys = Object.keys(marked)
        if (keys.length)
            return keys
        var path = currentPath()
        return path ? [path] : []
    }

    function toggleMark() {
        var path = currentPath()
        if (!path)
            return
        var next = Object.assign({}, marked)
        if (next[path])
            delete next[path]
        else
            next[path] = true
        marked = next
        if (list.currentIndex < list.count - 1)
            list.currentIndex++
    }

    function markAll() {
        var next = {}
        for (var i = 0; i < list.count; i++)
            next[folder.pathAt(i)] = true
        marked = next
    }

    function clearMarks() { marked = ({}) }

    function select(index, modifiers) {
        if (modifiers & Qt.ShiftModifier) {
            var next = {}
            var from = Math.min(anchorIndex, index)
            var to = Math.max(anchorIndex, index)
            for (var i = from; i <= to; i++)
                next[folder.pathAt(i)] = true
            marked = next
        } else if (modifiers & Qt.ControlModifier) {
            var toggled = Object.assign({}, marked)
            var path = folder.pathAt(index)
            if (toggled[path])
                delete toggled[path]
            else
                toggled[path] = true
            marked = toggled
            anchorIndex = index
        } else {
            clearMarks()
            anchorIndex = index
        }
        list.currentIndex = index
    }

    function openCurrent() {
        var path = currentPath()
        if (!path)
            return
        if (folder.isDirAt(list.currentIndex)) {
            lastPath = ""
            lastIndex = 0
            folder.path = path
        } else {
            backend.openExternally(path)
        }
    }

    function goUp() {
        // Land on the folder we came from, like any commander.
        lastPath = folder.path
        folder.up()
    }

    function goTo(count) {
        openFolder("/" + segments.slice(0, count).join("/"))
    }

    function openFolder(path) {
        lastPath = ""
        lastIndex = 0
        folder.path = path
        focusList()
    }

    function editPath() {
        editingPath = true
        pathField.text = folder.path
        pathField.forceActiveFocus()
        pathField.selectAll()
    }

    function finishEdit(accept) {
        var text = pathField.text.trim()
        editingPath = false
        if (accept && text !== "") {
            if (text.indexOf("~") === 0)
                text = backend.homePath() + text.substring(1)
            if (!backend.isDir(text))
                notice(qsTr("No such folder: %1").arg(text))
            else
                openFolder(text)
        }
        focusList()
    }

    function handleDrop(drop, destPath) {
        var paths = []
        if (drop.formats.indexOf("application/x-pathfinder-items") >= 0) {
            paths = JSON.parse(drop.getDataAsString("application/x-pathfinder-items")).paths
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
        itemsDropped(paths, destPath, move)
    }

    Connections {
        target: pane.folder
        function onPathChanged() { pane.clearMarks() }
        function onModelAboutToBeReset() { pane.resetting = true }
        function onModelReset() {
            // Wait for the ListView to settle before restoring the highlight.
            Qt.callLater(pane.restoreHighlight)
        }
    }

    DropArea {
        id: pageDrop
        anchors.fill: parent
        onDropped: drop => pane.handleDrop(drop, pane.folder.path)
    }

    AppMenu {
        id: menu
        // Several marked items: the download entry covers them all.
        readonly property bool many: pane.markedCount > 1
        readonly property bool isDir: pane.folder.isDirAt(list.currentIndex)
        readonly property bool cloud: pane.folder.cloudMount === true

        AppMenuItem { text: qsTr("Open"); iconName: "menu-open"; hint: "Enter"; onTriggered: pane.menuAction("open") }
        AppMenuItem {
            text: qsTr("Open in new tab")
            iconName: "menu-tab"
            hint: qsTr("Middle-click")
            visible: menu.isDir
            onTriggered: pane.openInNewTab(pane.currentPath())
        }
        AppMenuItem { text: qsTr("Preview"); iconName: "menu-preview"; hint: "Space"; visible: !menu.isDir; onTriggered: pane.menuAction("preview") }

        MenuSeparator { visible: menu.cloud; height: visible ? implicitHeight : 0 }
        AppMenuItem {
            text: qsTr("Download to this device")
            iconName: "menu-download"
            visible: menu.cloud
            enabled: menu.many || menu.isDir || pane.currentItemCloud === "online"
            onTriggered: pane.menuAction("cacheDownload")
        }
        AppMenuItem {
            text: qsTr("Copy view link")
            iconName: "menu-link"
            visible: menu.cloud && pane.targets().length === 1
            onTriggered: pane.menuAction("share:view")
        }
        AppMenuItem {
            text: qsTr("Copy edit link")
            iconName: "menu-link"
            visible: menu.cloud && pane.targets().length === 1
            onTriggered: pane.menuAction("share:edit")
        }

        MenuSeparator {}
        AppMenuItem { text: qsTr("Copy"); iconName: "menu-copy"; hint: "Ctrl+C"; onTriggered: pane.menuAction("clipCopy") }
        AppMenuItem { text: qsTr("Cut"); iconName: "menu-cut"; hint: "Ctrl+X"; onTriggered: pane.menuAction("clipCut") }
        AppMenuItem { text: qsTr("Paste"); iconName: "menu-paste"; hint: "Ctrl+V"; enabled: pane.canPaste; onTriggered: pane.menuAction("paste") }
        AppMenuItem { text: qsTr("Copy path"); iconName: "menu-path"; hint: "Ctrl+Shift+C"; onTriggered: pane.menuAction("copyPath") }
        AppMenuItem { text: qsTr("Copy to the other pane"); iconName: "menu-transfer"; hint: "Alt+C"; visible: pane.paired; onTriggered: pane.menuAction("copy") }
        AppMenuItem { text: qsTr("Move to the other pane"); iconName: "menu-transfer"; hint: "Alt+M"; visible: pane.paired; onTriggered: pane.menuAction("move") }

        MenuSeparator {}
        AppMenuItem { text: qsTr("Rename"); iconName: "menu-rename"; hint: "F2"; enabled: !menu.many; onTriggered: pane.menuAction("rename") }
        AppMenu {
            id: newSubMenu
            title: qsTr("New")
            iconName: "menu-new"
            Instantiator {
                model: pane.newKinds
                delegate: AppMenuItem { required property var modelData; required property int index; text: modelData.label; onTriggered: pane.menuAction("new:" + index) }
                onObjectAdded: (index, object) => newSubMenu.insertItem(index, object)
                onObjectRemoved: (index, object) => newSubMenu.removeItem(object)
            }
        }
        AppMenuItem { text: qsTr("Move to the trash"); iconName: "menu-trash"; hint: "Delete"; onTriggered: pane.menuAction("trash") }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 2

            ToolButton { text: "←"; implicitWidth: 30; implicitHeight: 34; focusPolicy: Qt.NoFocus; enabled: pane.tab.back.length > 0; ToolTip.text: qsTr("Back (Alt+Left)"); ToolTip.visible: hovered; ToolTip.delay: 500; onClicked: { pane.focusRequested(); pane.tab.goBack() } }
            ToolButton { text: "→"; implicitWidth: 30; implicitHeight: 34; focusPolicy: Qt.NoFocus; enabled: pane.tab.forward.length > 0; ToolTip.text: qsTr("Forward (Alt+Right)"); ToolTip.visible: hovered; ToolTip.delay: 500; onClicked: { pane.focusRequested(); pane.tab.goForward() } }
            ToolButton { text: "↑"; implicitWidth: 30; implicitHeight: 34; focusPolicy: Qt.NoFocus; ToolTip.text: qsTr("Parent folder (Alt+Up)"); ToolTip.visible: hovered; ToolTip.delay: 500; onClicked: { pane.focusRequested(); pane.goUp() } }

            TextField {
                id: pathField
                Layout.fillWidth: true
                visible: pane.editingPath
                implicitHeight: 34
                topPadding: 4
                bottomPadding: 4
                onAccepted: pane.finishEdit(true)
                Keys.onEscapePressed: pane.finishEdit(false)
                onActiveFocusChanged: if (!activeFocus && pane.editingPath) pane.editingPath = false
            }

            Flickable {
                id: crumbs
                visible: !pane.editingPath
                Layout.fillWidth: true
                Layout.alignment: Qt.AlignVCenter
                implicitHeight: crumbRow.implicitHeight
                contentWidth: crumbRow.implicitWidth
                contentHeight: crumbRow.implicitHeight
                clip: true
                interactive: contentWidth > width
                onContentWidthChanged: contentX = Math.max(0, contentWidth - width)

                Row {
                    id: crumbRow
                    Repeater {
                        model: pane.segments.length + 1
                        delegate: Row {
                            id: crumb
                            required property int index
                            Label {
                                visible: crumb.index > 0
                                anchors.verticalCenter: parent.verticalCenter
                                text: "›"
                                color: "#5a5a60"
                            }
                            Label {
                                padding: 4
                                text: crumb.index === 0 ? "/" : pane.segments[crumb.index - 1]
                                color: crumb.index === pane.segments.length ? "#e6e6e6" : theme.accent
                                font.underline: crumbHover.hovered
                                HoverHandler { id: crumbHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: { pane.focusRequested(); pane.goTo(crumb.index) } }
                            }
                        }
                    }
                }
                // A click beside the path edits it, like an address bar.
                TapHandler {
                    enabled: crumbRow.width < crumbs.width
                    onTapped: eventPoint => { if (eventPoint.position.x > crumbRow.width) { pane.focusRequested(); pane.editPath() } }
                }
            }

            ToolButton {
                implicitWidth: 34; implicitHeight: 34; focusPolicy: Qt.NoFocus
                icon.source: "qrc:/icons/tree.svg"; icon.width: 18; icon.height: 18
                icon.color: pane.showTree ? theme.accent : "#c0c0c6"
                ToolTip.text: pane.showTree ? qsTr("Hide folder columns (`)") : qsTr("Show folder columns (`)")
                ToolTip.visible: hovered; ToolTip.delay: 500
                onClicked: pane.treeToggled()
            }
            ToolButton { text: "↻"; implicitWidth: 34; implicitHeight: 34; focusPolicy: Qt.NoFocus; ToolTip.text: qsTr("Refresh (Ctrl+R)"); ToolTip.visible: hovered; ToolTip.delay: 500; onClicked: { pane.focusRequested(); pane.folder.refresh() } }
            ToolButton { id: newButton; text: "+"; implicitWidth: 34; implicitHeight: 34; focusPolicy: Qt.NoFocus; ToolTip.text: qsTr("New file or folder (Ctrl+N)"); ToolTip.visible: hovered; ToolTip.delay: 500; onClicked: { pane.focusRequested(); pane.menuAction("newMenu") } }
        }

        Label {
            Layout.fillWidth: true
            visible: pane.folder.error !== ""
            text: pane.folder.error
            color: "#f7768e"
            wrapMode: Text.Wrap
        }

        SplitView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            orientation: Qt.Vertical
            handle: Rectangle {
                implicitHeight: 6
                color: SplitHandle.hovered || SplitHandle.pressed ? Qt.alpha(theme.accent, 0.5) : "transparent"
            }

        FolderTree {
            visible: pane.showTree
            SplitView.preferredHeight: 170
            SplitView.minimumHeight: 80
            folder: pane.folder
            onOpened: path => { pane.focusRequested(); pane.openFolder(path) }
            onOpenedInNewTab: path => pane.openInNewTab(path)
            currentPath: pane.currentItemPath
            onSelected: path => { pane.focusRequested(); pane.select(pane.folder.indexOfPath(path), 0); pane.focusList() }
            onActivated: path => { pane.focusRequested(); pane.select(pane.folder.indexOfPath(path), 0); pane.openCurrent() }
        }

        ListView {
            id: list
            SplitView.fillHeight: true
            clip: true
            model: pane.folder
            keyNavigationEnabled: true
            currentIndex: 0
            // Dragging a row starts a drag and drop, so the list must not flick.
            interactive: false
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

            Keys.onPressed: event => {
                const page = Math.max(1, Math.floor(height / 28) - 1)
                if (event.key === Qt.Key_Home)
                    currentIndex = 0
                else if (event.key === Qt.Key_End)
                    currentIndex = count - 1
                else if (event.key === Qt.Key_PageUp)
                    currentIndex = Math.max(0, currentIndex - page)
                else if (event.key === Qt.Key_PageDown)
                    currentIndex = Math.min(count - 1, currentIndex + page)
                else
                    return
                event.accepted = true
            }

            onCurrentIndexChanged: {
                if (pane.resetting || currentIndex < 0)
                    return
                pane.lastIndex = currentIndex
                pane.lastPath = pane.folder.pathAt(currentIndex)
            }

            delegate: Rectangle {
                id: row
                required property int index
                required property string name
                required property string path
                required property bool isDir
                required property string sizeText
                required property string modified
                required property var model
                // Files in an rclone mount: "online", "local" or "pending".
                readonly property string cloud: model.cloud ? model.cloud : ""

                // Dragging a marked row takes every marked item with it.
                readonly property var dragPaths: pane.marked[path] ? Object.keys(pane.marked) : [path]

                width: ListView.view.width
                height: 28
                radius: Math.min(4, theme.radius)
                border.width: folderDrop.containsDrag ? 2 : 0
                border.color: theme.accent
                color: pane.marked[path] ? Qt.alpha(theme.accent, 0.28)
                     : ListView.isCurrentItem ? (pane.active ? Qt.alpha(theme.accent, 0.16) : "#1c1c20")
                     : "transparent"

                Drag.active: dragHandler.active
                Drag.dragType: Drag.Automatic
                Drag.supportedActions: Qt.CopyAction | Qt.MoveAction
                Drag.proposedAction: Qt.CopyAction
                Drag.mimeData: {
                    return {
                        "application/x-pathfinder-items": JSON.stringify({ paths: dragPaths }),
                        "text/uri-list": dragPaths.map(function (p) { return "file://" + encodeURI(p) }).join("\r\n")
                    }
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 10
                    Image {
                        // Inside a mount the column is kept while a folder's state is worked out.
                        visible: row.cloud !== "" || pane.folder.cloudMount === true
                        source: row.cloud !== "" ? "qrc:/icons/" + row.cloud + ".svg" : ""
                        sourceSize: Qt.size(16, 16)
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        HoverHandler { id: cloudHover }
                        ToolTip.visible: cloudHover.hovered && row.cloud !== ""
                        ToolTip.delay: 400
                        ToolTip.text: row.isDir
                                      ? (row.cloud === "local" ? qsTr("Everything in it is on this device")
                                         : row.cloud === "pending" ? qsTr("Something in it is waiting to upload")
                                         : qsTr("Not all of it is on this device"))
                                      : (row.cloud === "local" ? qsTr("Available on this device")
                                         : row.cloud === "pending" ? qsTr("Waiting to upload")
                                         : qsTr("Online only"))
                    }
                    Label {
                        Layout.fillWidth: true
                        text: row.isDir ? row.name + "/" : row.name
                        elide: Text.ElideRight
                        color: row.isDir ? theme.accent : "#e6e6e6"
                    }
                    Label {
                        Layout.preferredWidth: 80
                        horizontalAlignment: Text.AlignRight
                        text: row.sizeText
                        color: "#9a9aa0"
                    }
                    Label {
                        Layout.preferredWidth: 130
                        text: row.modified
                        color: "#9a9aa0"
                        elide: Text.ElideRight
                    }
                }

                TapHandler {
                    acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                    onTapped: (eventPoint, button) => {
                        pane.focusRequested()
                        if (button === Qt.MiddleButton) {
                            if (row.isDir)
                                pane.openInNewTab(row.path)
                        } else if (button === Qt.RightButton) {
                            if (!pane.marked[row.path])
                                pane.select(row.index, 0)
                            menu.popup()
                        } else {
                            pane.select(row.index, point.modifiers)
                        }
                    }
                    onDoubleTapped: (eventPoint, button) => {
                        if (button === Qt.LeftButton) {
                            list.currentIndex = row.index
                            pane.openCurrent()
                        }
                    }
                }

                DragHandler {
                    id: dragHandler
                    acceptedButtons: Qt.LeftButton
                    target: null
                    onActiveChanged: if (active) pane.focusRequested()
                }

                DropArea {
                    id: folderDrop
                    anchors.fill: parent
                    enabled: row.isDir
                    onDropped: drop => pane.handleDrop(drop, row.path)
                }
            }
        }

        }

        Label {
            Layout.fillWidth: true
            color: "#9a9aa0"
            text: pane.markedCount > 0 ? qsTr("%n marked", "", pane.markedCount)
                : qsTr("%n items", "", list.count)
        }
    }
}
