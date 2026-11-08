import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import Quickshell
import qs.Commons
import qs.Commons as Commons
import qs.Ui
import "Model.js" as Model

// Like the OneDrive flyout on Windows: the newest files of every account,
// one click to open them, and a way into omadrive.
Panel {
  id: root
  moduleName: "smerlini.onedrive"
  ipcTarget: "smerlini.onedrive"
  manageIpc: false

  Commons.Translations { id: strings; directory: Qt.resolvedUrl("i18n") }

  readonly property Item panelFlick: panel.loadedContent ? panel.loadedContent.panelFlick : null
  readonly property Item fileColumn: panel.loadedContent ? panel.loadedContent.fileColumn : null

  // The keyboard cursor runs over every file, then the "Open omadrive" row.
  property int cursor: 0
  property bool cursorActive: false
  readonly property int rowCount: drive.files.length + 1

  readonly property color foreground: bar ? bar.foreground : Commons.Color.foreground
  readonly property color urgent: bar ? bar.urgent : Commons.Color.urgent
  readonly property color dim: Qt.darker(foreground, 1.55)
  readonly property string fontFamily: bar ? bar.fontFamily : Style.font.family
  readonly property string cloudGlyph: "󰏊"

  readonly property var activeAccount: drive.accounts.filter(function (a) { return a.active })[0] || drive.accounts[0] || null
  readonly property string heroMeta: {
    if (!drive.installed) return strings.t("notInstalled")
    if (!drive.loaded) return drive.busy ? strings.t("firstRun") : strings.t("checking")
    if (drive.accounts.length === 0) return strings.t("noAccounts")
    if (drive.busy) return strings.t("checking")
    return strings.t("upToDate").arg(drive.checkedAt.toLocaleTimeString(Qt.locale(Commons.I18n.formatLocale), Locale.ShortFormat))
  }

  function when(date) {
    if (!date) return ""
    var locale = Qt.locale(Commons.I18n.formatLocale)
    var time = date.toLocaleTimeString(locale, Locale.ShortFormat)
    var today = new Date()
    var start = new Date(today.getFullYear(), today.getMonth(), today.getDate())
    if (date >= start) return strings.t("today").arg(time)
    if (date >= new Date(start.getTime() - 86400000)) return strings.t("yesterday").arg(time)
    return date.toLocaleDateString(locale, Locale.ShortFormat)
  }

  function fileMeta(file) {
    var parts = [when(file.changed)]
    if (file.by) parts.push(file.by)
    if (file.folder) parts.push(file.folder)
    return parts.filter(function (p) { return p !== "" }).join(" · ")
  }

  function glyphFor(name) {
    var ext = name.indexOf(".") > 0 ? name.split(".").pop().toLowerCase() : ""
    if (["jpg", "jpeg", "png", "gif", "webp", "heic", "svg", "bmp", "tif", "tiff"].indexOf(ext) >= 0) return "󰋩"
    if (["mp4", "mov", "mkv", "webm", "avi", "m4v"].indexOf(ext) >= 0) return "󰕧"
    if (ext === "pdf") return "󰈦"
    if (["doc", "docx", "odt", "rtf"].indexOf(ext) >= 0) return "󰈬"
    if (["xls", "xlsx", "ods", "csv"].indexOf(ext) >= 0) return "󰈛"
    if (["ppt", "pptx", "odp"].indexOf(ext) >= 0) return "󰈧"
    if (["zip", "7z", "tar", "gz", "rar"].indexOf(ext) >= 0) return "󰗄"
    return "󰈙"
  }


  function moveCursor(dy) {
    if (!cursorActive) { cursorActive = true; return }
    cursor = Math.max(0, Math.min(rowCount - 1, cursor + dy))
    scrollCursorIntoView()
  }

  function activate() {
    if (cursor < drive.files.length) drive.open(drive.files[cursor])
    else drive.openApp()
    root.close()
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

  onOpenedChanged: if (opened) {
    cursorActive = false
    cursor = 0
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
    text: root.cloudGlyph
    opacity: drive.loaded && drive.lastError === "" ? 1.0 : 0.6
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
      property alias fileColumn: fileColumn
      implicitHeight: column.implicitHeight
      anchors.fill: parent

      // Rows carry their cursor index; the last is "Open omadrive".
      function rowAt(index) {
        if (index >= drive.files.length) return openAppRow
        var found = null
        function walk(item) {
          for (var i = 0; i < item.children.length && !found; i++) {
            var child = item.children[i]
            if (child.rowIndex === index && child.isFileRow) found = child
            else walk(child)
          }
        }
        walk(fileColumn)
        return found
      }

      onMoveRequested: function (dx, dy) { root.moveCursor(dy) }
      onActivateRequested: if (root.cursorActive) root.activate()
      onCloseRequested: root.close()
      onTabRequested: function (direction) { root.switchPanel(direction) }
      onTextKey: function (t) {
        if (t === "r" || t === "R") drive.refresh()
        else if (t === "o" || t === "O") { drive.openApp(); root.close() }
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
            title: root.activeAccount ? root.activeAccount.driveName : strings.t("title")
            meta: root.heroMeta
            foreground: root.foreground
            fontFamily: root.fontFamily
            iconComponent: Component {
              Text {
                textFormat: Text.PlainText
                text: root.cloudGlyph
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

          Column {
            id: fileColumn
            width: parent.width
            spacing: Style.space(14)

            Repeater {
              model: drive.accounts
              Column {
                id: section
                required property var modelData
                required property int index
                // Where this account's files start in the cursor order.
                readonly property int offset: {
                  var n = 0
                  for (var i = 0; i < index; i++) n += drive.accounts[i].files.length
                  return n
                }
                width: fileColumn.width
                spacing: Style.space(6)

                RowLayout {
                  width: parent.width
                  spacing: Style.space(8)
                  PanelSectionHeader {
                    Layout.fillWidth: true
                    text: section.modelData.driveName
                    foreground: root.foreground
                    fontFamily: root.fontFamily
                  }
                  PlainLabel {
                    visible: section.modelData.total > 0
                    text: strings.t("storage").arg(Model.bytes(section.modelData.used, Commons.I18n.formatNumber))
                                                .arg(Model.bytes(section.modelData.total, Commons.I18n.formatNumber))
                    color: root.dim
                    font.pixelSize: Style.font.caption
                  }
                }

                PlainLabel {
                  visible: section.modelData.error !== ""
                  width: parent.width
                  text: strings.t("readFailed").arg(section.modelData.error)
                  color: root.urgent
                  font.pixelSize: Style.font.caption
                  wrapMode: Text.WordWrap
                }

                PlainLabel {
                  visible: section.modelData.files.length === 0 && section.modelData.error === ""
                  width: parent.width
                  text: strings.t("noFiles")
                  color: root.dim
                  font.pixelSize: Style.font.bodySmall
                  horizontalAlignment: Text.AlignHCenter
                }

                Repeater {
                  model: section.modelData.files
                  FileRow {
                    required property var modelData
                    required property int index
                    width: section.width
                    file: modelData
                    rowIndex: section.offset + index
                  }
                }
              }
            }
          }

          PanelSeparator { foreground: root.foreground }

          CursorSurface {
            id: openAppRow
            width: parent.width
            hasCursor: root.cursorActive && root.cursor === root.rowCount - 1
            foreground: root.foreground
            implicitHeight: appRow.implicitHeight + Style.spacing.rowPaddingX

            MouseArea {
              anchors.fill: parent
              hoverEnabled: true
              cursorShape: Qt.PointingHandCursor
              onEntered: { root.cursorActive = true; root.cursor = root.rowCount - 1 }
              onClicked: { drive.openApp(); root.close() }
            }

            RowLayout {
              id: appRow
              anchors.left: parent.left
              anchors.right: parent.right
              anchors.verticalCenter: parent.verticalCenter
              anchors.leftMargin: Style.space(10)
              anchors.rightMargin: Style.space(10)
              spacing: Style.space(8)

              PlainLabel { text: "󰉋"; font.pixelSize: Style.font.icon }
              ColumnLayout {
                Layout.fillWidth: true
                spacing: Style.space(1)
                PlainLabel { Layout.fillWidth: true; text: strings.t("openApp"); font.pixelSize: Style.font.body; elide: Text.ElideRight }
                PlainLabel { Layout.fillWidth: true; text: strings.t("openAppHelp"); color: root.dim; font.pixelSize: Style.font.caption; elide: Text.ElideRight }
              }
            }
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

  component FileRow: CursorSurface {
    id: row
    property var file: null
    property int rowIndex: 0
    readonly property bool isFileRow: true

    hasCursor: root.cursorActive && root.cursor === rowIndex
    foreground: root.foreground
    implicitHeight: content.implicitHeight + Style.spacing.rowPaddingX

    MouseArea {
      anchors.fill: parent
      hoverEnabled: true
      cursorShape: Qt.PointingHandCursor
      onEntered: { root.cursorActive = true; root.cursor = row.rowIndex }
      onClicked: { drive.open(row.file); root.close() }
    }

    RowLayout {
      anchors.left: parent.left
      anchors.right: parent.right
      anchors.verticalCenter: parent.verticalCenter
      anchors.leftMargin: Style.space(10)
      anchors.rightMargin: Style.space(10)
      spacing: Style.space(8)

      PlainLabel {
        text: row.file ? root.glyphFor(row.file.name) : ""
        font.pixelSize: Style.font.icon
        Layout.alignment: Qt.AlignVCenter
      }

      ColumnLayout {
        id: content
        Layout.fillWidth: true
        spacing: Style.space(1)
        PlainLabel { Layout.fillWidth: true; text: row.file ? row.file.name : ""; font.pixelSize: Style.font.body; elide: Text.ElideMiddle }
        PlainLabel { Layout.fillWidth: true; text: row.file ? root.fileMeta(row.file) : ""; color: root.dim; font.pixelSize: Style.font.caption; elide: Text.ElideRight }
      }

      // Like OneDrive's status column: a tick when this device has the file, a cloud when it is online only.
      PlainLabel {
        text: row.file && row.file.isLocal ? "󰄬" : "󰅟"
        color: root.dim
        font.pixelSize: Style.font.bodySmall
        Layout.alignment: Qt.AlignVCenter
        ToolTip.visible: statusHover.containsMouse
        ToolTip.text: row.file && row.file.isLocal ? strings.t("onDevice") : strings.t("onlineOnly")
        MouseArea { id: statusHover; anchors.fill: parent; hoverEnabled: true; acceptedButtons: Qt.NoButton }
      }
    }
  }
}
