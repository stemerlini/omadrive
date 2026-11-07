import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One side of the commander. Files are marked with Space or Ctrl/Shift+click;
// actions apply to the marked files, or to the highlighted one when nothing
// is marked. Rows can be dragged to the other pane, a folder, or other apps.
Rectangle {
    id: pane

    required property var folder
    property string title: ""
    property bool active: false
    property var marked: ({})
    readonly property int markedCount: Object.keys(marked).length
    property string lastPath: ""
    property int lastIndex: 0
    property int anchorIndex: 0
    property bool resetting: false
    // The OneDrive pane: paths are OneDrive paths and it needs a sign-in.
    property bool remote: false
    readonly property bool needsSignIn: remote && !account.signedIn
    readonly property var segments: folder.path.split("/").filter(function (s) { return s !== "" })

    signal focusRequested()
    signal notice(string text)
    signal menuAction(string action)
    signal itemsDropped(var paths, bool fromRemote, string destPath, bool move)

    color: "#131316"
    radius: theme.radius
    border.width: active || pageDrop.containsDrag ? 2 : 1
    border.color: pageDrop.containsDrag ? theme.accent : active ? theme.accent : "#2a2a2e"

    function focusList() { list.forceActiveFocus() }
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
        if (needsSignIn) {
            account.signIn()
            return
        }
        var path = currentPath()
        if (!path)
            return
        if (folder.isDirAt(list.currentIndex)) {
            lastPath = ""
            lastIndex = 0
            folder.path = path
        } else if (remote) {
            notice(qsTr("Press F5 to download this file"))
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
        lastPath = ""
        lastIndex = 0
        folder.path = "/" + segments.slice(0, count).join("/")
    }

    function handleDrop(drop, destPath) {
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
        itemsDropped(paths, fromRemote, destPath, move)
    }

    Connections {
        target: pane.folder
        function onPathChanged() { pane.clearMarks() }
        function onModelAboutToBeReset() { pane.resetting = true }
        function onModelReset() {
            // Wait for the ListView to settle before restoring the highlight.
            Qt.callLater(function () {
                var index = pane.folder.indexOfPath(pane.lastPath)
                if (index < 0)
                    index = Math.max(0, Math.min(pane.lastIndex, list.count - 1))
                list.currentIndex = index
                pane.resetting = false
            })
        }
    }

    DropArea {
        id: pageDrop
        anchors.fill: parent
        onDropped: drop => pane.handleDrop(drop, pane.folder.path)
    }

    Menu {
        id: menu
        MenuItem { text: qsTr("Open"); onTriggered: pane.menuAction("open") }
        MenuSeparator {}
        MenuItem { text: qsTr("Copy to the other pane"); onTriggered: pane.menuAction("copy") }
        MenuItem { text: qsTr("Move to the other pane"); onTriggered: pane.menuAction("move") }
        MenuSeparator {}
        MenuItem { text: qsTr("Rename"); onTriggered: pane.menuAction("rename") }
        MenuItem { text: qsTr("New folder"); onTriggered: pane.menuAction("mkdir") }
        MenuItem { text: qsTr("Move to the trash"); onTriggered: pane.menuAction("trash") }
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            spacing: 4
            Label {
                text: pane.remote && account.signedIn && account.name !== "" ? pane.title + " · " + account.name : pane.title
                font.bold: true
                color: pane.active ? theme.accent : "#9a9aa0"
            }
            Item { Layout.fillWidth: true }
            ToolButton { text: "↑"; implicitWidth: 36; onClicked: { pane.focusRequested(); pane.goUp() } }
            ToolButton { text: "⟳"; implicitWidth: 36; onClicked: { pane.focusRequested(); pane.folder.refresh() } }
            ToolButton { text: "+"; implicitWidth: 36; onClicked: { pane.focusRequested(); pane.menuAction("mkdir") } }
        }

        Flickable {
            id: crumbs
            Layout.fillWidth: true
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
                            text: crumb.index === 0 ? (pane.remote ? "OneDrive" : "/") : pane.segments[crumb.index - 1]
                            color: crumb.index === pane.segments.length ? "#e6e6e6" : theme.accent
                            font.underline: crumbHover.hovered
                            HoverHandler { id: crumbHover; cursorShape: Qt.PointingHandCursor }
                            TapHandler { onTapped: { pane.focusRequested(); pane.goTo(crumb.index) } }
                        }
                    }
                }
            }
        }

        Label {
            Layout.fillWidth: true
            visible: pane.folder.error !== ""
            text: pane.folder.error
            color: "#f7768e"
            wrapMode: Text.Wrap
        }

        ListView {
            id: list
            Layout.fillWidth: true
            Layout.fillHeight: true
            clip: true
            model: pane.folder
            keyNavigationEnabled: true
            currentIndex: 0
            // Dragging a row starts a drag and drop, so the list must not flick.
            interactive: false
            boundsBehavior: Flickable.StopAtBounds
            ScrollBar.vertical: ScrollBar {}

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
                    var data = { "application/x-omadrive-items": JSON.stringify({ remote: pane.remote, paths: dragPaths }) }
                    if (!pane.remote)
                        data["text/uri-list"] = dragPaths.map(function (p) { return "file://" + encodeURI(p) }).join("\r\n")
                    return data
                }

                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 8
                    anchors.rightMargin: 8
                    spacing: 10
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
                    acceptedButtons: Qt.LeftButton | Qt.RightButton
                    onTapped: (eventPoint, button) => {
                        pane.focusRequested()
                        if (button === Qt.RightButton) {
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

        Label {
            Layout.fillWidth: true
            color: "#9a9aa0"
            text: pane.remote && pane.folder.loading ? qsTr("Loading…")
                : pane.markedCount > 0 ? qsTr("%n marked", "", pane.markedCount)
                : qsTr("%n items", "", list.count)
        }
    }

    ColumnLayout {
        anchors.centerIn: parent
        visible: pane.needsSignIn
        spacing: 12
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: account.restoring ? qsTr("Connecting to OneDrive…")
                : account.state === "signingIn" ? qsTr("Waiting for the Microsoft sign-in…")
                : qsTr("Not signed in to OneDrive")
            color: "#e6e6e6"
        }
        Button {
            Layout.alignment: Qt.AlignHCenter
            visible: account.state === "signedOut"
            text: qsTr("Sign in with Microsoft (Enter)")
            highlighted: true
            onClicked: { pane.focusRequested(); account.signIn() }
        }
    }
}
