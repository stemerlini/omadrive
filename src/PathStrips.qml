import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material

// The folder columns when collapsed: one thin strip per folder on the path,
// its name written top to bottom. A click goes to that folder, a
// middle-click opens it in a new tab; the button on top brings the columns back.
Rectangle {
    id: strips

    required property var segments
    property bool remote: false
    signal opened(int depth)
    signal openedInNewTab(string path)
    signal expandRequested()

    readonly property int stripWidth: 22

    implicitWidth: Math.min(strips.segments.length + 1, 8) * (stripWidth + 2) + 6
    color: "#101013"
    radius: theme.radius
    clip: true

    ToolButton {
        id: expand
        anchors.horizontalCenter: parent.horizontalCenter
        y: 2
        implicitWidth: 30; implicitHeight: 30
        focusPolicy: Qt.NoFocus
        icon.source: "qrc:/icons/tree.svg"; icon.width: 16; icon.height: 16
        ToolTip.text: qsTr("Show folder columns (`)")
        ToolTip.visible: hovered; ToolTip.delay: 500
        onClicked: strips.expandRequested()
    }

    ListView {
        id: list
        anchors.fill: parent
        anchors.topMargin: expand.height + 4
        anchors.margins: 3
        orientation: ListView.Horizontal
        spacing: 2
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        model: strips.segments.length + 1
        onCountChanged: Qt.callLater(positionViewAtEnd)
        Component.onCompleted: positionViewAtEnd()

        delegate: Rectangle {
            id: strip
            required property int index
            readonly property bool current: index === strips.segments.length
            readonly property string name: index === 0 ? (strips.remote ? "OneDrive" : "/") : strips.segments[index - 1]

            width: strips.stripWidth
            height: ListView.view.height
            radius: Math.min(4, theme.radius)
            color: current ? Qt.alpha(theme.accent, 0.22) : hover.hovered ? "#1f1f23" : "transparent"
            border.width: current ? 1 : 0
            border.color: Qt.alpha(theme.accent, 0.6)

            Text {
                anchors.top: parent.top
                anchors.topMargin: 6
                anchors.bottom: parent.bottom
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: strip.name.split("").join("\n")
                lineHeight: 0.92
                clip: true
                font.pixelSize: 12
                font.bold: strip.current
                color: strip.current ? theme.accent : "#c0c0c6"
            }

            HoverHandler { id: hover; cursorShape: Qt.PointingHandCursor }
            ToolTip.text: strip.name
            ToolTip.visible: hover.hovered
            ToolTip.delay: 500
            TapHandler { onTapped: strips.opened(strip.index) }
            TapHandler {
                acceptedButtons: Qt.MiddleButton
                onTapped: strips.openedInNewTab("/" + strips.segments.slice(0, strip.index).join("/"))
            }
        }
    }
}
