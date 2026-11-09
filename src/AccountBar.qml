import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// The accounts at the top right: the others small and dimmed, one click to
// switch to them; the active one larger with a ring, opening the account list.
RowLayout {
    id: bar
    spacing: 8

    signal addWorkApp()

    readonly property var others: account.accounts.filter(function (a) { return !a.active })
    readonly property var current: {
        var a = account.accounts.filter(function (a) { return a.active })
        return a.length ? a[0] : null
    }

    Repeater {
        model: bar.others
        delegate: Avatar {
            required property var modelData
            size: 22
            source: modelData.avatar
            name: modelData.name
            dim: !otherHover.hovered
            warn: modelData.status === "expired"
            HoverHandler { id: otherHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: account.switchTo(modelData.id) }
            ToolTip.visible: otherHover.hovered
            ToolTip.delay: 400
            ToolTip.text: qsTr("Switch to %1\n%2   Ctrl+Shift+A").arg(modelData.driveName).arg(modelData.email)
        }
    }

    Item {
        implicitWidth: bar.current ? 34 : signInButton.implicitWidth
        implicitHeight: 34

        Avatar {
            id: me
            visible: bar.current !== null
            anchors.centerIn: parent
            size: 28
            ring: true
            source: bar.current ? bar.current.avatar : ""
            name: bar.current ? bar.current.name : ""
            warn: bar.current !== null && bar.current.status === "expired"
            opacity: bar.current && bar.current.status === "connecting" ? 0.5 : 1
            HoverHandler { id: meHover; cursorShape: Qt.PointingHandCursor }
            TapHandler { onTapped: list.opened ? list.close() : list.open() }
            ToolTip.visible: meHover.hovered && !list.opened
            ToolTip.delay: 400
            ToolTip.text: bar.current ? bar.current.name + "\n" + bar.current.email : ""
        }
        BusyIndicator {
            anchors.centerIn: parent
            width: 40
            height: 40
            running: account.busy || (bar.current !== null && bar.current.status === "connecting")
            visible: running
        }
        ToolButton {
            id: signInButton
            visible: bar.current === null
            anchors.centerIn: parent
            implicitHeight: 34
            focusPolicy: Qt.NoFocus
            text: account.busy ? qsTr("Signing in…") : qsTr("Sign in")
            font.capitalization: Font.MixedCase
            enabled: !account.busy
            onClicked: account.signIn()
        }

        Popup {
            id: list
            y: parent.height + 6
            x: parent.width - width
            width: 320
            padding: 6
            modal: false
            focus: true
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutsideParent
            background: Rectangle {
                color: "#18181b"
                radius: theme.radius
                border.color: "#2a2a2e"
            }

            contentItem: ColumnLayout {
                spacing: 2

                Repeater {
                    model: account.accounts
                    delegate: ItemDelegate {
                        id: row
                        required property var modelData
                        Layout.fillWidth: true
                        implicitHeight: 64
                        hoverEnabled: true
                        onClicked: {
                            list.close()
                            if (modelData.status === "expired" && modelData.active)
                                account.signIn()
                            else
                                account.switchTo(modelData.id)
                        }
                        background: Rectangle {
                            radius: Math.min(5, theme.radius)
                            color: row.modelData.active ? Qt.alpha(theme.accent, 0.14)
                                 : row.hovered ? "#222226" : "transparent"
                        }
                        contentItem: RowLayout {
                            spacing: 12
                            Avatar {
                                size: 34
                                source: row.modelData.avatar
                                name: row.modelData.name
                                warn: row.modelData.status === "expired"
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 1
                                Label {
                                    Layout.fillWidth: true
                                    text: row.modelData.name
                                    elide: Text.ElideRight
                                    font.bold: true
                                    color: "#e6e6e6"
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: row.modelData.email
                                    elide: Text.ElideRight
                                    color: "#9a9aa0"
                                    font.pixelSize: 12
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: row.modelData.status === "expired" ? qsTr("Signed out — click to sign in")
                                        : row.modelData.status === "connecting" ? qsTr("Connecting…")
                                        : row.modelData.driveName
                                    elide: Text.ElideRight
                                    color: row.modelData.status === "expired" ? "#f7768e" : "#6a6a70"
                                    font.pixelSize: 11
                                }
                            }
                            Label {
                                visible: row.modelData.active
                                text: "✓"
                                color: theme.accent
                                font.bold: true
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    Layout.topMargin: 4
                    Layout.bottomMargin: 4
                    implicitHeight: 1
                    color: "#2a2a2e"
                    visible: account.accounts.length > 0
                }

                AccountAction {
                    iconName: "menu-adduser"
                    text: account.busy ? qsTr("Waiting for the browser…") : qsTr("Add account (work or personal)…")
                    enabled: !account.busy
                    onClicked: { list.close(); account.addAccount() }
                }
                AccountAction {
                    iconName: "menu-adduser"
                    text: qsTr("Add work account with a custom app ID…")
                    enabled: !account.busy
                    onClicked: { list.close(); bar.addWorkApp() }
                }
                AccountAction {
                    visible: bar.current !== null
                    iconName: "menu-signout"
                    text: qsTr("Sign out of this account")
                    hint: "Ctrl+Shift+L"
                    onClicked: { list.close(); account.signOut() }
                }
            }
        }
    }

    component AccountAction: ItemDelegate {
        id: act
        property string iconName
        property string hint: ""
        Layout.fillWidth: true
        implicitHeight: visible ? 34 : 0
        hoverEnabled: true
        background: Rectangle {
            radius: Math.min(5, theme.radius)
            color: act.hovered && act.enabled ? Qt.alpha(theme.accent, 0.16) : "transparent"
        }
        contentItem: RowLayout {
            spacing: 10
            Image {
                Layout.preferredWidth: 16
                Layout.preferredHeight: 16
                sourceSize: Qt.size(16, 16)
                source: "qrc:/icons/" + act.iconName + ".svg"
                opacity: act.enabled ? 0.85 : 0.3
            }
            Label {
                Layout.fillWidth: true
                text: act.text
                elide: Text.ElideMiddle
                color: !act.enabled ? "#55555b" : act.hovered ? theme.accent : "#e6e6e6"
            }
            Label {
                visible: act.hint !== ""
                text: act.hint
                color: "#5a5a60"
                font.pixelSize: 12
            }
        }
    }
}
