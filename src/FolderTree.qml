import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// One Commander's folder columns: each folder above the current one is a thin
// strip with its name turned 90°, and the current folder is a full column of
// its folders and files that follows the highlight in the list. A click on a
// strip opens that folder; middle-click opens it in a new tab.
Rectangle {
    id: tree

    required property var folder
    // The item highlighted in the list, highlighted here too.
    property string currentPath: ""
    signal opened(string path)
    signal openedInNewTab(string path)
    signal selected(string path)
    signal activated(string path)

    readonly property var segments: folder.path.split("/").filter(function (s) { return s !== "" })

    color: "#101013"
    radius: theme.radius
    clip: true

    function pathOf(depth) { return "/" + segments.slice(0, depth).join("/") }

    ListView {
        id: columns
        anchors.fill: parent
        anchors.margins: 4
        orientation: ListView.Horizontal
        spacing: 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: tree.segments.length + 1
        ScrollBar.horizontal: ScrollBar { policy: ScrollBar.AsNeeded }
        onCountChanged: Qt.callLater(positionViewAtEnd)
        Component.onCompleted: positionViewAtEnd()

        delegate: Rectangle {
            id: column
            required property int index
            readonly property string path: tree.pathOf(index)
            readonly property bool thin: index < tree.segments.length
            readonly property string name: index === 0 ? "/" : tree.segments[index - 1]

            width: thin ? 24 : 280
            height: ListView.view.height - 8
            radius: Math.min(4, theme.radius)
            color: !thin ? "#16161a" : stripHover.hovered ? "#1f1f23" : "transparent"
            border.width: 1
            border.color: "#1f1f23"

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
                font.pixelSize: 12
                color: stripHover.hovered ? theme.accent : "#c0c0c6"
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
                x: 8
                y: 4
                width: parent.width - 16
                text: column.name
                font.pixelSize: 11
                font.bold: true
                color: "#7a7a80"
                elide: Text.ElideMiddle
            }

            ListView {
                id: entries
                visible: !column.thin
                anchors.top: heading.bottom
                anchors.topMargin: 4
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 2
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                model: column.thin ? null : tree.folder
                currentIndex: column.thin ? -1 : tree.folder.indexOfPath(tree.currentPath)
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
                    readonly property bool current: path === tree.currentPath
                    width: ListView.view.width
                    height: 24
                    radius: Math.min(4, theme.radius)
                    color: current ? Qt.alpha(theme.accent, 0.22) : entryHover.hovered ? "#1c1c20" : "transparent"
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        x: 8
                        width: parent.width - 16
                        text: entry.name + (entry.isDir ? "/" : "")
                        elide: Text.ElideRight
                        color: entry.current ? "#e6e6e6" : entry.isDir ? theme.accent : "#b0b0b6"
                    }
                    HoverHandler { id: entryHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                        onTapped: (eventPoint, button) => {
                            if (button === Qt.MiddleButton) {
                                if (entry.isDir)
                                    tree.openedInNewTab(entry.path)
                            } else {
                                tree.selected(entry.path)
                            }
                        }
                        onDoubleTapped: tree.activated(entry.path)
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: !column.thin && entries.count === 0
                text: qsTr("Empty folder")
                font.pixelSize: 11
                color: "#5a5a60"
            }
        }
    }
}
