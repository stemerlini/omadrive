import QtQuick
import Quickshell
import Quickshell.Io
import "Model.js" as Model

// Runs `omadrive --recent`, which signs in with omadrive's saved accounts and
// prints each drive's newest files. Nothing here talks to the network itself.
Item {
  id: root

  property var settings: ({})

  property var accounts: []
  property bool loaded: false       // a reply has arrived at least once
  property bool installed: true
  property string lastError: ""
  property date checkedAt: new Date(0)
  readonly property bool busy: recent.running
  readonly property var files: {
    var out = []
    for (var i = 0; i < accounts.length; i++)
      for (var j = 0; j < accounts[i].files.length; j++) out.push(accounts[i].files[j])
    return out
  }

  readonly property bool autoRefresh: setting("autoRefresh", true) !== false
  readonly property int refreshIntervalSec: intSetting("refreshIntervalSec", 300, 60, 3600)
  readonly property int filesPerAccount: intSetting("filesPerAccount", 8, 3, 20)

  property string _out: ""

  function setting(name, fallback) {
    var value = settings ? settings[name] : undefined
    return value === undefined || value === null ? fallback : value
  }

  function intSetting(name, fallback, min, max) {
    var n = parseInt(String(setting(name, fallback)), 10)
    if (!isFinite(n)) n = fallback
    return Math.max(min, Math.min(max, n))
  }

  function refresh() {
    if (recent.running) return
    _out = ""
    recent.command = ["omadrive", "--recent", String(filesPerAccount)]
    recent.running = true
  }

  function open(file) {
    if (!file) return
    if (file.isLocal && file.local) Quickshell.execDetached(["uwsm-app", "--", "xdg-open", file.local])
    else if (file.webUrl) Qt.openUrlExternally(file.webUrl)
  }

  function openApp() {
    Quickshell.execDetached(["uwsm-app", "--", "omadrive"])
  }

  Timer {
    interval: root.refreshIntervalSec * 1000
    repeat: true
    running: root.autoRefresh
    triggeredOnStart: true
    onTriggered: root.refresh()
  }

  Process {
    id: recent
    running: false
    command: []
    stdout: StdioCollector { id: out; waitForEnd: true }
    stderr: StdioCollector { id: err; waitForEnd: true }
    onExited: function(exitCode) {
      var text = String(out.text || "")
      if (exitCode === 127 || (exitCode !== 0 && text === "" && String(err.text || "").indexOf("No such file") >= 0)) {
        root.installed = false
        return
      }
      root.installed = true
      if (exitCode !== 0 || text.length > 1048576) {
        root.lastError = Model.text(String(err.text || "").trim() || "omadrive exited with " + exitCode)
        return
      }
      var parsed = Model.parse(text, root.filesPerAccount)
      if (!parsed.ok) {
        root.lastError = "Unreadable reply from omadrive"
        return
      }
      root.accounts = parsed.accounts
      root.lastError = ""
      root.loaded = true
      root.checkedAt = new Date()
    }
  }
}
