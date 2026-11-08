import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

// The row of tabs above the panes, with the split switch and the account.
// The tab in the focused pane is filled; with two panes, the other one is
// outlined. Files dropped on a tab go into the folder it shows.
RowLayout {
    id: strip

    property var tabs: []
    property int leftTab: 0
    property int rightTab: 1
    property bool split: false
    property int activeTab: 0
    property var labelFor: function (tab) { return "" }

    signal activated(int index)
    signal closeRequested(int index)
    signal newTab(bool remote)
    signal splitToggled()
    signal dropped(var drop, int index)

    spacing: 4

    function countOf(remote) {
        return tabs.filter(function (t) { return t.remote === remote }).length
    }

    Flickable {
        id: scroller
        Layout.fillWidth: true
        implicitHeight: 34
        contentWidth: tabRow.implicitWidth
        clip: true
        interactive: contentWidth > width
        boundsBehavior: Flickable.StopAtBounds

        Row {
            id: tabRow
            spacing: 4

            Repeater {
                model: strip.tabs
                delegate: Rectangle {
                    id: chip
                    required property var modelData
                    required property int index
                    readonly property bool current: index === strip.activeTab
                    readonly property bool shownElsewhere: strip.split && !current
                        && (index === strip.leftTab || index === strip.rightTab)
                    readonly property bool closable: strip.countOf(modelData.remote) > 1

                    width: Math.min(220, content.implicitWidth + 20)
                    height: 34
                    radius: theme.radius
                    color: tabDrop.containsDrag ? Qt.alpha(theme.accent, 0.35)
                         : current ? Qt.alpha(theme.accent, 0.22)
                         : chipHover.hovered ? "#1c1c20" : "transparent"
                    border.width: shownElsewhere || tabDrop.containsDrag ? 1 : 0
                    border.color: theme.accent

                    RowLayout {
                        id: content
                        anchors.fill: parent
                        anchors.leftMargin: 10
                        anchors.rightMargin: 6
                        spacing: 6
                        Image {
                            source: chip.modelData.remote ? "qrc:/icons/tab-cloud.svg" : "qrc:/icons/tab-device.svg"
                            sourceSize: Qt.size(16, 16)
                            Layout.preferredWidth: 16
                            Layout.preferredHeight: 16
                            opacity: chip.current ? 1 : 0.7
                        }
                        Label {
                            Layout.maximumWidth: 150
                            text: strip.labelFor(chip.modelData)
                            elide: Text.ElideMiddle
                            color: chip.current ? "#e6e6e6" : "#9a9aa0"
                            font.bold: chip.current
                        }
                        // Keeps the width steady whether or not the × shows.
                        Item {
                            implicitWidth: 18
                            implicitHeight: 18
                            Label {
                                anchors.centerIn: parent
                                visible: chip.closable && (chipHover.hovered || chip.current)
                                text: "×"
                                color: closeHover.hovered ? "#e6e6e6" : "#7a7a80"
                                HoverHandler { id: closeHover; cursorShape: Qt.PointingHandCursor }
                                TapHandler { onTapped: strip.closeRequested(chip.index) }
                            }
                        }
                    }

                    HoverHandler { id: chipHover }
                    ToolTip.visible: chipHover.hovered && !closeHover.hovered
                    ToolTip.delay: 600
                    ToolTip.text: (chip.modelData.remote ? "OneDrive: " : "") + chip.modelData.folder.path
                        + (chip.index < 9 ? "   Ctrl+" + (chip.index + 1) : "")

                    TapHandler {
                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                        onTapped: (eventPoint, button) => {
                            if (button === Qt.MiddleButton)
                                strip.closeRequested(chip.index)
                            else
                                strip.activated(chip.index)
                        }
                    }

                    DropArea {
                        id: tabDrop
                        anchors.fill: parent
                        onDropped: drop => strip.dropped(drop, chip.index)
                    }
                }
            }

            ToolButton {
                text: "+"
                implicitWidth: 34
                implicitHeight: 34
                focusPolicy: Qt.NoFocus
                ToolTip.text: qsTr("New tab (Ctrl+T)")
                ToolTip.visible: hovered
                ToolTip.delay: 500
                onClicked: newMenu.popup()
                Menu {
                    id: newMenu
                    MenuItem { text: qsTr("New local tab"); onTriggered: strip.newTab(false) }
                    MenuItem { text: qsTr("New OneDrive tab"); onTriggered: strip.newTab(true) }
                }
            }
        }
    }

    ToolButton {
        icon.source: strip.split ? "qrc:/icons/single.svg" : "qrc:/icons/split.svg"
        icon.width: 18
        icon.height: 18
        icon.color: strip.split ? theme.accent : "#c0c0c6"
        implicitWidth: 34
        implicitHeight: 34
        focusPolicy: Qt.NoFocus
        ToolTip.text: strip.split ? qsTr("One pane (F3)") : qsTr("Two panes (F3)")
        ToolTip.visible: hovered
        ToolTip.delay: 500
        onClicked: strip.splitToggled()
    }

    ToolButton {
        id: accountButton
        implicitHeight: 34
        focusPolicy: Qt.NoFocus
        text: account.signedIn ? (account.name !== "" ? account.name : qsTr("OneDrive"))
            : account.restoring ? qsTr("Connecting…")
            : account.state === "signingIn" ? qsTr("Signing in…")
            : qsTr("Sign in")
        font.capitalization: Font.MixedCase
        onClicked: account.signedIn ? accountMenu.popup() : account.state === "signedOut" ? account.signIn() : null
        Menu {
            id: accountMenu
            MenuItem { text: qsTr("Sign out (Ctrl+Shift+L)"); onTriggered: account.signOut() }
        }
    }
}
