import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// One side of the commander. Files are marked with Space; actions apply to
// the marked files, or to the highlighted one when nothing is marked.
Rectangle {
    id: pane

    required property var folder
    property string title: ""
    property bool active: false
    property var marked: ({})
    readonly property int markedCount: Object.keys(marked).length
    property string lastPath: ""
    property int lastIndex: 0
    property bool resetting: false
    // The OneDrive pane: paths are OneDrive paths and it needs a sign-in.
    property bool remote: false
    readonly property bool needsSignIn: remote && !account.signedIn

    signal focusRequested()
    signal notice(string text)

    color: "#131316"
    radius: theme.radius
    border.width: active ? 2 : 1
    border.color: active ? theme.accent : "#2a2a2e"

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

    function clearMarks() { marked = ({}) }

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

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 6

        RowLayout {
            Layout.fillWidth: true
            Label {
                text: pane.remote && account.signedIn && account.name !== "" ? pane.title + " · " + account.name : pane.title
                font.bold: true
                color: pane.active ? theme.accent : "#9a9aa0"
            }
            Label {
                Layout.fillWidth: true
                text: pane.folder.path
                elide: Text.ElideLeft
                horizontalAlignment: Text.AlignRight
                color: "#e6e6e6"
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

                width: ListView.view.width
                height: 28
                radius: Math.min(4, theme.radius)
                color: pane.marked[path] ? Qt.alpha(theme.accent, 0.28)
                     : ListView.isCurrentItem ? (pane.active ? Qt.alpha(theme.accent, 0.16) : "#1c1c20")
                     : "transparent"

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

                MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        pane.focusRequested()
                        list.currentIndex = row.index
                    }
                    onDoubleClicked: {
                        list.currentIndex = row.index
                        pane.openCurrent()
                    }
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
            text: account.state === "signingIn" ? qsTr("Waiting for the Microsoft sign-in…")
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
