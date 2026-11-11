import QtQuick
import QtQuick.Controls

// The app's context menus: dark, rounded, with icons and keys.
Menu {
    id: menu
    // Shown beside this menu's entry when it is a submenu.
    property string iconName: ""

    topPadding: 5
    bottomPadding: 5
    delegate: AppMenuItem {}
    background: Rectangle {
        implicitWidth: 270
        color: theme.surface
        radius: theme.radius
        border.color: Qt.alpha(theme.accent, 0.5)
    }
}
