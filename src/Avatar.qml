import QtQuick
import QtQuick.Controls

// A round account picture (made round by Account), with initials until it
// loads and a ring in the theme colour for the active account.
Item {
    id: avatar

    property string source: ""
    property string name: ""
    property int size: 28
    property bool ring: false
    property bool dim: false
    property bool warn: false  // needs signing in again

    implicitWidth: size
    implicitHeight: size

    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: "#2a2a2e"
        visible: picture.status !== Image.Ready
        Label {
            anchors.centerIn: parent
            text: avatar.name.split(/[ ,]+/).filter(function (w) { return w !== "" }).slice(0, 2)
                  .map(function (w) { return w.charAt(0).toUpperCase() }).join("")
            font.pixelSize: Math.round(avatar.size * 0.38)
            font.bold: true
            color: "#c0c0c6"
        }
    }
    Image {
        id: picture
        anchors.fill: parent
        source: avatar.source
        sourceSize: Qt.size(avatar.size * 2, avatar.size * 2)
        cache: false
        smooth: true
        mipmap: true
    }
    Rectangle {
        anchors.fill: parent
        radius: width / 2
        color: "#0e0e10"
        opacity: avatar.dim ? 0.45 : 0
        Behavior on opacity { NumberAnimation { duration: 120 } }
    }
    Rectangle {
        visible: avatar.ring
        anchors.centerIn: parent
        width: parent.width + 6
        height: width
        radius: width / 2
        color: "transparent"
        border.width: 2
        border.color: theme.accent
    }
    Rectangle {
        visible: avatar.warn
        width: Math.max(8, avatar.size * 0.3)
        height: width
        radius: width / 2
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        color: "#f7768e"
        border.width: 2
        border.color: "#0e0e10"
    }
}
