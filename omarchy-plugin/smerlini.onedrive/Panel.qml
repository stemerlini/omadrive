import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
import qs.Commons
import qs.Commons as Commons
import qs.Ui
import "Model.js" as Model

// A cloud on the bar: the OneDrive accounts rclone mounts, a switch to
// mount each one, and a way to connect another.
Panel {
  id: root
  moduleName: "smerlini.onedrive"
  ipcTarget: "smerlini.onedrive"
  manageIpc: false

  Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }

  readonly property Item panelFlick: panel.loadedContent ? panel.loadedContent.panelFlick : null

  // The keyboard cursor runs over every account, then "Connect" and "Open omadrive".
  property int cursor: 0
  property bool cursorActive: false
  readonly property int accountCount: drive.accounts.length
  readonly property int connectIndex: accountCount
  readonly property int openAppIndex: accountCount + 1
  readonly property int rowCount: accountCount + 2

  readonly property color foreground: bar ? bar.foreground : Commons.Color.foreground
  readonly property color urgent: bar ? bar.urgent : Commons.Color.urgent
  readonly property color dim: Qt.darker(foreground, 1.55)
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family
  readonly property string cloudGlyph: "󰏊"
  readonly property string uploadGlyph: "󰅧"
  readonly property string workGlyph: "󰃖"

  readonly property string heroMeta: {
    if (!drive.installed) return strings.t("notInstalled")
    if (!drive.loaded) return strings.t("checking")
    if (drive.accounts.length === 0) return strings.t("noAccounts")
    if (drive.pending > 0) return strings.tn("pending", drive.pending).arg(Commons.I18n.formatNumber(drive.pending))
    if (!drive.anyMounted) return strings.t("noneMounted")
    return strings.t("upToDate")
  }

  function accountAt(index) {
    return index >= 0 && index < drive.accounts.length ? drive.accounts[index] : null
  }

  function accountCaption(account) {
    var parts = []
    if (account.mounted && account.mountpoint) parts.push(account.mountpoint)
    else parts.push(account.configured ? strings.t("notMounted") : strings.t("notSetUp"))
    if (account.total > 0)
      parts.push(strings.t("storage").arg(Model.bytes(account.used, Commons.I18n.formatNumber))
                                     .arg(Model.bytes(account.total, Commons.I18n.formatNumber)))
    if (account.pending > 0)
      parts.push(strings.tn("pendingShort", account.pending).arg(Commons.I18n.formatNumber(account.pending)))
    return parts.join(" · ")
  }

  function moveCursor(dy) {
    if (!cursorActive) { cursorActive = true; return }
    cursor = Math.max(0, Math.min(rowCount - 1, cursor + dy))
    scrollCursorIntoView()
  }

  function connectAccount() {
    drive.connect()
    root.close()
  }

  function openApp() {
    drive.openApp()
    root.close()
  }

  function openAccount(account) {
    if (!account) return
    var opening = account.mounted && account.mountpoint !== ""
    drive.openAccount(account)
    if (opening) root.close()
  }

  function activate() {
    if (cursor === connectIndex) { if (drive.installed) connectAccount() }
    else if (cursor === openAppIndex) { if (drive.installed) openApp() }
    else openAccount(accountAt(cursor))
  }

  function scrollCursorIntoView() {
    if (!panelFlick || !panel.loadedContent) return
    var item = panel.loadedContent.rowAt(cursor)
    if (!item) return
    Qt.callLater(function () {
      var margin = Style.space(6)
      var top = item.mapToItem(panelFlick.contentItem, 0, 0).y
      var bottom = top + item.height
      var maxY = Math.max(0, panelFlick.contentHeight - panelFlick.height)
      if (top < panelFlick.contentY + margin) panelFlick.contentY = Math.max(0, top - margin)
      else if (bottom > panelFlick.contentY + panelFlick.height - margin)
        panelFlick.contentY = Math.min(maxY, bottom + margin - panelFlick.height)
    })
  }

  implicitWidth: button.implicitWidth
  implicitHeight: button.implicitHeight

  onRowCountChanged: if (cursor > rowCount - 1) cursor = rowCount - 1

  onOpenedChanged: if (opened) {
    cursorActive = false
    cursor = 0
    drive.actionError = ""
    if (panelFlick) panelFlick.contentY = 0
    drive.refresh()
    Qt.callLater(function () { if (panel.loadedContent) panel.loadedContent.forceActiveFocus() })
  }

  Service {
    id: drive
    settings: root.settings
  }

  ShellIpc {
    target: root.ipcTarget
    function open(): void { root.open() }
    function close(): void { root.close() }
    function toggle(): void { root.toggle() }
    function refresh(): string { drive.refresh(); return "ok" }
  }

  BarIconButton {
    id: button
    anchors.fill: parent
    bar: root.bar
    text: drive.pending > 0 ? root.uploadGlyph : root.cloudGlyph
    opacity: drive.loaded && drive.lastError === "" && drive.anyMounted ? 1.0 : 0.6
    onPressed: function (buttonCode) {
      if (buttonCode === Qt.RightButton) drive.refresh()
      else if (buttonCode === Qt.MiddleButton) drive.openApp()
      else root.toggle()
    }
  }

  KeyboardPanel {
    id: panel
    anchorItem: button
    owner: root
    bar: root.bar
    open: root.opened
    focusTarget: panel.loadedContent
    contentWidth: panel.fittedContentWidth(Style.space(400))
    contentHeight: panel.fittedContentHeight(panel.loadedContentHeight, Style.space(600))

    content: PanelKeyCatcher {
      id: keys
      property alias panelFlick: panelFlick
      implicitHeight: column.implicitHeight
      anchors.fill: parent

      function rowAt(index) {
        if (index === root.connectIndex) return connectRow
        if (index === root.openAppIndex) return openAppRow
        return accountRepeater.itemAt(index)
      }

      onMoveRequested: function (dx, dy) { root.moveCursor(dy) }
      onActivateRequested: if (root.cursorActive) root.activate()
      onCloseRequested: root.close()
      onTabRequested: function (direction) { root.switchPanel(direction) }
      onTextKey: function (t) {
        var k = t.toLowerCase()
        if (k === "r") drive.refresh()
        else if (k === "o") root.openApp()
        else if (k === "c") root.connectAccount()
        else if (k === "m") {
          let account = root.cursorActive ? root.accountAt(root.cursor) : (drive.accounts.length === 1 ? drive.accounts[0] : null)
          drive.toggleMount(account)
        }
      }

      Flickable {
        id: panelFlick
        anchors.fill: parent
        contentWidth: width
        contentHeight: column.implicitHeight
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        flickableDirection: Flickable.VerticalFlick
        interactive: contentHeight > height
        ScrollBar.vertical: ScrollBar { policy: ScrollBar.AsNeeded }

        Column {
          id: column
          width: panelFlick.width
          spacing: Style.space(12)

          PanelHero {
            id: hero
            width: parent.width
            title: strings.t("title")
            meta: root.heroMeta
            foreground: root.foreground
            fontFamily: root.fontFamily
            iconComponent: Component {
              Text {
                textFormat: Text.PlainText
                text: drive.pending > 0 ? root.uploadGlyph : root.cloudGlyph
                color: root.foreground
                font.family: root.fontFamily
                font.pixelSize: Style.font.display
              }
            }
            trailingControl: Component {
              PanelActionButton {
                iconText: "󰑐"
                tooltipText: strings.t("refresh")
                foreground: root.foreground
                fontFamily: root.fontFamily
                enabled: !drive.busy
                onClicked: drive.refresh()
              }
            }
          }

          PlainLabel {
            visible: drive.lastError !== ""
            width: parent.width
            text: strings.t("readFailed").arg(drive.lastError)
            color: root.urgent
            font.pixelSize: Style.font.bodySmall
            wrapMode: Text.WordWrap
          }

          PlainLabel {
            visible: drive.actionError !== ""
            width: parent.width
            text: strings.t("actionFailed").arg(drive.actionError)
            color: root.urgent
            font.pixelSize: Style.font.bodySmall
            wrapMode: Text.WordWrap
          }

          PanelSectionHeader {
            visible: drive.accounts.length > 0
            width: parent.width
            text: strings.t("accounts")
            foreground: root.foreground
            fontFamily: root.fontFamily
          }

          Column {
            id: accountColumn
            visible: drive.accounts.length > 0
            width: parent.width
            spacing: Style.space(4)

            Repeater {
              id: accountRepeater
              model: drive.accounts
              AccountRow {
                required property var modelData
                required property int index
                width: accountColumn.width
                account: modelData
                rowIndex: index
              }
            }
          }

          PlainLabel {
            visible: drive.installed && drive.loaded && drive.accounts.length === 0
            width: parent.width
            text: strings.t("noAccountsHelp")
            color: root.dim
            font.pixelSize: Style.font.bodySmall
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
          }

          PanelSeparator { foreground: root.foreground }

          ActionRow {
            id: connectRow
            width: parent.width
            rowIndex: root.connectIndex
            glyph: "󰐕"
            title: strings.t("connect")
            caption: strings.t("connectHelp")
            enabled: drive.installed
            onTriggered: root.connectAccount()
          }

          ActionRow {
            id: openAppRow
            width: parent.width
            rowIndex: root.openAppIndex
            glyph: "󰉋"
            title: strings.t("openApp")
            caption: strings.t("openAppHelp")
            enabled: drive.installed
            onTriggered: root.openApp()
          }
        }
      }
    }
  }

  component PlainLabel: Text {
    textFormat: Text.PlainText
    color: root.foreground
    font.family: root.fontFamily
  }

  component ActionRow: CursorSurface {
    id: action
    property int rowIndex: 0
    property string glyph: ""
    property string title: ""
    property string caption: ""
    signal triggered()

    hasCursor: root.cursorActive && root.cursor === rowIndex
    foreground: root.foreground
    opacity: enabled ? 1.0 : 0.5
    implicitHeight: actionContent.implicitHeight + Style.spacing.rowPaddingX

    MouseArea {
      anchors.fill: parent
      hoverEnabled: true
      cursorShape: Qt.PointingHandCursor
      onEntered: { root.cursorActive = true; root.cursor = action.rowIndex }
      onClicked: action.triggered()
    }

    RowLayout {
      anchors.left: parent.left
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      anchors.leftMargin: Style.space(10)
      anchors.rightMargin: Style.space(10)
      spacing: Style.space(8)

      PlainLabel { text: action.glyph; font.pixelSize: Style.font.icon; Layout.alignment: Qt.AlignVCenter }
      ColumnLayout {
        id: actionContent
        Layout.fillWidth: true
        spacing: Style.space(1)
        PlainLabel { Layout.fillWidth: true; text: action.title; font.pixelSize: Style.font.body; elide: Text.ElideRight }
        PlainLabel { Layout.fillWidth: true; text: action.caption; color: root.dim; font.pixelSize: Style.font.caption; elide: Text.ElideRight }
      }
    }
  }

  component AccountRow: CursorSurface {
    id: row
    property var account: null
    property int rowIndex: 0
    readonly property bool working: drive.acting && account !== null && drive.actionRemote === account.remote

    hasCursor: root.cursorActive && root.cursor === rowIndex
    foreground: root.foreground
    implicitHeight: content.implicitHeight + Style.spacing.rowPaddingX

    MouseArea {
      anchors.fill: parent
      hoverEnabled: true
      cursorShape: Qt.PointingHandCursor
      onEntered: { root.cursorActive = true; root.cursor = row.rowIndex }
      onClicked: root.openAccount(row.account)
    }

    RowLayout {
      anchors.left: parent.left
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      anchors.leftMargin: Style.space(10)
      anchors.rightMargin: Style.space(6)
      spacing: Style.space(8)

      PlainLabel {
        text: row.account && row.account.work ? root.workGlyph : root.cloudGlyph
        opacity: row.account && row.account.mounted ? 1.0 : 0.6
        font.pixelSize: Style.font.icon
        Layout.alignment: Qt.AlignVCenter
      }

      ColumnLayout {
        id: content
        Layout.fillWidth: true
        spacing: Style.space(1)
        PlainLabel { Layout.fillWidth: true; text: row.account ? row.account.label : ""; font.pixelSize: Style.font.body; elide: Text.ElideRight }
        PlainLabel {
          Layout.fillWidth: true
          text: row.working ? (row.account.mounted ? strings.t("unmounting") : strings.t("mounting"))
                            : (row.account ? root.accountCaption(row.account) : "")
          color: root.dim
          font.pixelSize: Style.font.caption
          elide: Text.ElideMiddle
        }
        PlainLabel {
          visible: row.account !== null && row.account.error !== ""
          Layout.fillWidth: true
          text: row.account ? row.account.error : ""
          color: root.urgent
          font.pixelSize: Style.font.caption
          wrapMode: Text.WordWrap
          maximumLineCount: 3
          elide: Text.ElideRight
        }
      }

      PanelActionButton {
        Layout.alignment: Qt.AlignVCenter
        iconText: row.account && row.account.mounted ? "󰅙" : "󰐊"
        tooltipText: row.account && row.account.mounted ? strings.t("unmount") : strings.t("mount")
        foreground: root.foreground
        fontFamily: root.fontFamily
        enabled: !drive.acting && row.account !== null
        onClicked: drive.toggleMount(row.account)
      }
    }
  }
}
