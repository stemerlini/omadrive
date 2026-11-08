import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// One Commander's folder columns: the current folder is a full column of its
// subfolders; each folder above it on the path is a thin strip with its name
// written top to bottom. A click opens that folder or strip; middle-click
// opens it in a new tab.
Rectangle {
    id: tree

    required property var folder
    property bool remote: false
    signal opened(string path)
    signal openedInNewTab(string path)

    readonly property var segments: folder.path.split("/").filter(function (s) { return s !== "" })
    property int revision: 0

    color: "#101013"
    radius: theme.radius
    clip: true

    Connections {
        target: tree.folder
        function onFoldersChanged() { tree.revision++ }
    }

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
            readonly property string next: index < tree.segments.length ? tree.segments[index] : ""
            readonly property bool thin: index < tree.segments.length
            readonly property string name: index === 0 ? (tree.remote ? "OneDrive" : "/") : tree.segments[index - 1]
            readonly property var names: { tree.revision; return thin ? [] : tree.folder.foldersIn(path) }

            width: thin ? 24 : 220
            height: ListView.view.height - 8
            radius: Math.min(4, theme.radius)
            color: !thin ? "#16161a" : stripHover.hovered ? "#1f1f23" : "transparent"
            border.width: 1
            border.color: "#1f1f23"

            Text {
                visible: column.thin
                anchors.fill: parent
                anchors.topMargin: 6
                horizontalAlignment: Text.AlignHCenter
                text: column.name.split("").join("\n")
                lineHeight: 0.92
                clip: true
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
                model: column.names
                ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }
                onModelChanged: Qt.callLater(function () {
                    var i = column.names.indexOf(column.next)
                    if (i >= 0)
                        entries.positionViewAtIndex(i, ListView.Center)
                })

                delegate: Rectangle {
                    id: entry
                    required property string modelData
                    readonly property bool onPath: modelData === column.next
                    width: ListView.view.width
                    height: 24
                    radius: Math.min(4, theme.radius)
                    color: onPath ? Qt.alpha(theme.accent, 0.22) : entryHover.hovered ? "#1c1c20" : "transparent"
                    Label {
                        anchors.verticalCenter: parent.verticalCenter
                        x: 8
                        width: parent.width - 16
                        text: entry.modelData
                        elide: Text.ElideRight
                        color: entry.onPath ? "#e6e6e6" : "#b0b0b6"
                    }
                    HoverHandler { id: entryHover; cursorShape: Qt.PointingHandCursor }
                    TapHandler {
                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                        onTapped: (eventPoint, button) => {
                            var target = (column.path === "/" ? "" : column.path) + "/" + entry.modelData
                            if (button === Qt.MiddleButton)
                                tree.openedInNewTab(target)
                            else
                                tree.opened(target)
                        }
                    }
                }
            }

            Label {
                anchors.centerIn: parent
                visible: !column.thin && column.names.length === 0
                text: qsTr("No folders")
                font.pixelSize: 11
                color: "#5a5a60"
            }
        }
    }
}
