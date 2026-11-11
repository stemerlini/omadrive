import QtQuick
import Quickshell
import Quickshell.Io
import "Model.js" as Model

// Asks `pathfinder cloud` about the OneDrive accounts rclone mounts. Every
// command is an argument array; nothing is ever passed through a shell.
Item {
  id: root

  property var settings: ({})

  property var accounts: []
  property bool loaded: false       // a reply has arrived at least once
  property bool installed: true
  property string lastError: ""
  property string actionError: ""
  property string actionRemote: ""  // the account being mounted or unmounted
  property date checkedAt: new Date(0)
  readonly property bool busy: status.running
  readonly property bool acting: action.running
  readonly property int pending: {
    var n = 0
    for (var i = 0; i < accounts.length; i++) n += accounts[i].pending
    return n
  }
  readonly property bool anyMounted: accounts.some(function (a) { return a.mounted })

  readonly property bool autoRefresh: setting("autoRefresh", true) !== false
  readonly property int refreshIntervalSec: intSetting("refreshIntervalSec", 300, 60, 3600)

  property bool _again: false

  function setting(name, fallback) {
    var value = settings ? settings[name] : undefined
    return value === undefined || value === null ? fallback : value
  }

  function intSetting(name, fallback, min, max) {
    var n = parseInt(String(setting(name, fallback)), 10)
    if (!isFinite(n)) n = fallback
    return Math.max(min, Math.min(max, n))
  }

  function missing(exitCode, errText) {
    return exitCode === 127 || (exitCode !== 0 && /No such file|not found|ENOENT/.test(errText))
  }

  function refresh() {
    if (status.running) { _again = true; return }
    status.command = ["pathfinder", "cloud", "status"]
    status.running = true
  }

  function setMounted(account, mount) {
    if (!account || !Model.remote(account.remote) || action.running) return
    actionError = ""
    actionRemote = account.remote
    action.command = ["pathfinder", "cloud", mount ? "mount" : "unmount", account.remote]
    action.running = true
  }

  function toggleMount(account) {
    if (account) setMounted(account, !account.mounted)
  }

  function openAccount(account) {
    if (!account) return
    if (account.mounted && account.mountpoint) Quickshell.execDetached(["uwsm-app", "--", "pathfinder", account.mountpoint])
    else setMounted(account, true)
  }

  function openApp() {
    Quickshell.execDetached(["uwsm-app", "--", "pathfinder"])
  }

  function connect() {
    Quickshell.execDetached(["omarchy-launch-floating-terminal-with-presentation", "pathfinder", "cloud", "connect"])
  }

  Timer {
    interval: root.refreshIntervalSec * 1000
    repeat: true
    running: root.autoRefresh
    triggeredOnStart: true
    onTriggered: root.refresh()
  }

  Process {
    id: status
    running: false
    command: []
    stdout: StdioCollector { id: out; waitForEnd: true }
    stderr: StdioCollector { id: err; waitForEnd: true }
    onExited: function(exitCode) {
      var text = String(out.text || "")
      var errText = String(err.text || "")
      if (root.missing(exitCode, errText) && text === "") {
        root.installed = false
      } else {
        root.installed = true
        var parsed = text.length > 1048576 ? { ok: false, error: "" } : Model.parse(text)
        if (parsed.ok && exitCode === 0) {
          root.accounts = parsed.accounts
          root.lastError = ""
          root.loaded = true
          root.checkedAt = new Date()
        } else {
          root.lastError = parsed.error || Model.text(errText.trim()) || "pathfinder exited with " + exitCode
        }
      }
      if (root._again) { root._again = false; Qt.callLater(root.refresh) }
    }
  }

  Process {
    id: action
    running: false
    command: []
    stdout: StdioCollector { id: actionOut; waitForEnd: true }
    stderr: StdioCollector { id: actionErr; waitForEnd: true }
    onExited: function(exitCode) {
      var errText = String(actionErr.text || "")
      if (root.missing(exitCode, errText) && String(actionOut.text || "") === "") {
        root.installed = false
      } else {
        var result = Model.parseResult(actionOut.text)
        if (!result.ok || exitCode !== 0)
          root.actionError = result.error || Model.text(errText.trim()) || "pathfinder exited with " + exitCode
      }
      root.actionRemote = ""
      root.refresh()
    }
  }
}
