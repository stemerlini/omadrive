import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import Pathfinder

// One Commander's folder columns: each folder above the current one is a thin
// strip with its name turned 90°, the current folder is a full column that
// follows the highlight in the list, and the last column looks one step
// ahead: the contents of the highlighted folder, or the highlighted file's
// details. A click on a strip opens that folder; middle-click opens it in a
// new tab.
Rectangle {
    id: tree

    required property var folder
    // The item highlighted in the list, highlighted here too.
    property string currentPath: ""
    property bool currentIsDir: false
    property string currentSize: ""
    property string currentModified: ""
    property string currentCloud: ""
    signal opened(string path)
    signal openedInNewTab(string path)
    signal selected(string path)
    signal activated(string path)
    // A click in the look-ahead column: open `folder` with `path` highlighted.
    signal entered(string folder, string path)

    readonly property var segments: folder.path.split("/").filter(function (s) { return s !== "" })
    readonly property string currentName: currentPath.split("/").pop()

    color: theme.panel
    radius: theme.radius
    border.width: 1
    border.color: Qt.alpha(theme.accent, 0.2)
    clip: true

    function pathOf(depth) { return "/" + segments.slice(0, depth).join("/") }

    // The look-ahead folder loads after a short pause, so holding an arrow
    // key does not read every folder on the way.
    FolderList {
        id: aheadList
        showHidden: tree.folder.showHidden
    }
    readonly property bool aheadReady: currentIsDir && aheadList.path === currentPath && aheadList.error === ""
    Timer {
        id: aheadTimer
        interval: 120
        onTriggered: if (tree.currentIsDir && tree.currentPath !== "") aheadList.path = tree.currentPath
    }
    onCurrentPathChanged: aheadTimer.restart()
    onCurrentIsDirChanged: aheadTimer.restart()

    ListView {
        id: columns
        anchors.fill: parent
        anchors.margins: 6
        orientation: ListView.Horizontal
        spacing: 4
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        // Ancestors, the current folder, then the look-ahead column.
        model: tree.segments.length + 2
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
        onCountChanged: Qt.callLater(positionViewAtEnd)
        onWidthChanged: Qt.callLater(positionViewAtEnd)
        Component.onCompleted: positionViewAtEnd()

        delegate: Rectangle {
            id: column
            required property int index
            readonly property bool thin: index < tree.segments.length
            readonly property bool ahead: index === tree.segments.length + 1
            readonly property string path: tree.pathOf(index)
            readonly property string name: ahead ? tree.currentName : index === 0 ? "/" : tree.segments[index - 1]

            width: thin ? 26 : 280
            height: ListView.view.height - 10
            radius: Math.min(4, theme.radius)
            color: thin ? (stripHover.hovered ? Qt.alpha(theme.accent, 0.12) : "transparent") : theme.background
            border.width: thin ? 0 : 1
            border.color: Qt.alpha(theme.accent, ahead ? 0.12 : 0.25)
            visible: !ahead || tree.currentPath !== ""

            // Turned 90° clockwise, so it reads from the top down.
            Text {
                visible: column.thin
                anchors.centerIn: parent
                width: parent.height - 12
                height: parent.width
                rotation: 90
                verticalAlignment: Text.AlignVCenter
                text: column.name
                elide: Text.ElideRight
                font.family: theme.fontFamily
                font.pixelSize: 12
                color: stripHover.hovered ? theme.accent : theme.muted
            }
            HoverHandler { id: stripHover; enabled: column.thin; cursorShape: Qt.PointingHandCursor }
            ToolTip.text: column.name
            ToolTip.visible: column.thin && stripHover.hovered
            ToolTip.delay: 500
            TapHandler {
                enabled: column.thin
                acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                onTapped: (eventPoint, button) => button === Qt.MiddleButton ? tree.openedInNewTab(column.path) : tree.opened(column.path)
            }

            Label {
                id: heading
                visible: !column.thin
                x: 10
                y: 6
                width: parent.width - 20
                text: column.name + (column.ahead && tree.currentIsDir ? "/" : "")
                font.bold: true
                font.pixelSize: 12
                color: column.ahead ? theme.secondary : theme.accent
                elide: Text.ElideMiddle
            }

            ListView {
                id: entries
                visible: !column.thin && (!column.ahead || tree.currentIsDir)
                anchors.top: heading.bottom
                anchors.topMargin: 6
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 3
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: column.thin ? null : column.ahead ? (tree.aheadReady ? aheadList : null) : tree.folder
                currentIndex: column.thin || column.ahead ? -1 : tree.folder.indexOfPath(tree.currentPath)
                highlightMoveDuration: 0
                highlightFollowsCurrentItem: false
                onCurrentIndexChanged: if (currentIndex >= 0) positionViewAtIndex(currentIndex, ListView.Contain)
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

                delegate: Rectangle {
                    id: entry
                    required property int index
                    required property string name
                    required property string path
                    required property bool isDir
                    readonly property bool current: !column.ahead && path === tree.currentPath
                    width: ListView.view.width
                    height: 24
                    radius: Math.min(3, theme.radius)
                    color: current ? Qt.alpha(theme.accent, 0.25) : entryHover.hovered ? Qt.alpha(theme.accent, 0.08) : "transparent"
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        x: 8
                        width: parent.width - 16
                        text: entry.name + (entry.isDir ? "/" : "")
                        elide: Text.ElideRight
                        font.pixelSize: 13
                        color: entry.current ? theme.bright : entry.isDir ? theme.accent : theme.foreground
                    }
                    HoverHandler { id: entryHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                        onTapped: (eventPoint, button) => {
                            if (button === Qt.MiddleButton) {
                                if (entry.isDir)
                                    tree.openedInNewTab(entry.path)
                            } else if (column.ahead) {
                                tree.entered(tree.currentPath, entry.path)
                            } else {
                                tree.selected(entry.path)
                            }
                        }
                        onDoubleTapped: column.ahead ? (entry.isDir ? tree.opened(entry.path) : tree.activated(entry.path))
                                                     : tree.activated(entry.path)
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: !column.thin && (!column.ahead || tree.aheadReady) && entries.visible && entries.count === 0
                text: qsTr("Empty folder")
                font.pixelSize: 12
                color: theme.muted
            }
            Label {
                anchors.centerIn: parent
                width: parent.width - 24
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                visible: column.ahead && tree.currentIsDir && aheadList.error !== "" && !aheadTimer.running && aheadList.path !== tree.currentPath
                text: aheadList.error
                font.pixelSize: 12
                color: theme.error
            }

            // A file: what it is, in the place its contents would be.
            Column {
                visible: column.ahead && !tree.currentIsDir
                anchors.top: heading.bottom
                anchors.topMargin: 10
                x: 10
                width: parent.width - 20
                spacing: 10
                Repeater {
                    model: [
                        [qsTr("Size"), tree.currentSize],
                        [qsTr("Modified"), tree.currentModified],
                        [qsTr("OneDrive"), tree.currentCloud === "local" ? qsTr("On this device")
                                          : tree.currentCloud === "pending" ? qsTr("Waiting to upload")
                                          : tree.currentCloud === "online" ? qsTr("Online only") : ""]
                    ]
                    delegate: Column {
                        required property var modelData
                        visible: modelData[1] !== ""
                        width: parent.width
                        spacing: 1
                        Label { text: modelData[0]; font.pixelSize: 11; color: theme.muted }
                        Label { text: modelData[1]; width: parent.width; elide: Text.ElideRight; color: theme.foreground }
                    }
                }
                Label {
                    topPadding: 6
                    width: parent.width
                    wrapMode: Text.Wrap
                    text: qsTr("Enter open · Space preview")
                    font.pixelSize: 11
                    color: theme.muted
                }
            }
        }
    }
}
