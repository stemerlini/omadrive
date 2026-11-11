import QtQuick

// One tab: its folder list, the highlighted path to restore, and the folders
// visited before and after the current one for Alt+Left and Alt+Right.
QtObject {
    id: tab

    property var folder: null
    property string selected: ""
    property var back: []
    property var forward: []
    property string here: ""
    property bool stepping: false

    function goBack() {
        if (back.length === 0)
            return
        forward = forward.concat([here])
        stepping = true
        folder.path = back[back.length - 1]
        back = back.slice(0, -1)
    }

    function goForward() {
        if (forward.length === 0)
            return
        back = back.concat([here])
        stepping = true
        folder.path = forward[forward.length - 1]
        forward = forward.slice(0, -1)
    }

    property Connections watcher: Connections {
        target: tab.folder
        function onPathChanged() {
            var path = tab.folder.path
            if (!tab.stepping && tab.here !== "" && path !== tab.here) {
                tab.back = tab.back.slice(-49).concat([tab.here])
                tab.forward = []
            }
            tab.stepping = false
            tab.here = path
        }
    }

    Component.onCompleted: here = folder ? folder.path : ""
}
