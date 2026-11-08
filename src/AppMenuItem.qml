import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

// A menu row: icon, text and its key on the right. Hidden rows take no room.
MenuItem {
    id: item

    property string iconName: subMenu && subMenu.iconName ? subMenu.iconName : ""
    property string hint: ""

    height: visible ? implicitHeight : 0
    implicitHeight: 32
    leftPadding: 10
    rightPadding: subMenu ? 28 : 12

    contentItem: RowLayout {
        spacing: 10
        Image {
            Layout.preferredWidth: 16
            Layout.preferredHeight: 16
            sourceSize: Qt.size(16, 16)
            source: item.iconName === "" ? "" : item.iconName.indexOf("/") >= 0 ? item.iconName : "qrc:/icons/" + item.iconName + ".svg"
            opacity: item.enabled ? (item.highlighted ? 1 : 0.8) : 0.3
        }
        Label {
            Layout.fillWidth: true
            text: item.text
            elide: Text.ElideRight
            color: !item.enabled ? "#55555b" : item.highlighted ? theme.accent : "#e6e6e6"
        }
        Label {
            visible: item.hint !== ""
            text: item.hint
            color: "#5a5a60"
            font.pixelSize: 12
        }
    }

    background: Rectangle {
        implicitWidth: 260
        x: 4
        width: item.width - 8
        height: item.height
        radius: Math.min(5, theme.radius)
        color: item.highlighted && item.enabled ? Qt.alpha(theme.accent, 0.16) : "transparent"
    }
}
