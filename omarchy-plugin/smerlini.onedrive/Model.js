// Validates what `omadrive --recent` printed before the panel shows any of it.

var MAX_ACCOUNTS = 8
var MAX_FILES = 20
var MAX_TEXT = 300

function text(value) {
  var s = typeof value === "string" ? value : ""
  s = s.replace(/[\u0000-\u001f\u007f]/g, " ")
  return s.length > MAX_TEXT ? s.substring(0, MAX_TEXT - 1) + "…" : s
}

function number(value) {
  var n = Number(value)
  return isFinite(n) && n >= 0 ? n : 0
}

function stamp(value) {
  var s = text(value)
  if (!/^\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(\.\d+)?Z$/.test(s)) return null
  var d = new Date(s)
  return isNaN(d.getTime()) ? null : d
}

// Only https links to Microsoft's OneDrive and SharePoint hosts are opened.
function webUrl(value) {
  var s = text(value)
  var m = s.match(/^https:\/\/([A-Za-z0-9.-]+)(\/|$)/)
  if (!m) return ""
  var host = m[1].toLowerCase()
  var ok = /(^|\.)(sharepoint\.com|onedrive\.live\.com|live\.net|onedrive\.com|1drv\.ms)$/.test(host)
  return ok ? s : ""
}

// A local copy must be inside the account's mirror folder.
function localPath(value, mirror) {
  var s = text(value)
  if (!mirror || mirror.charAt(0) !== "/" || s.indexOf(mirror + "/") !== 0) return ""
  if (/(^|\/)\.\.(\/|$)/.test(s)) return ""
  return s
}

function parse(raw, limit) {
  var data
  try {
    data = JSON.parse(String(raw || ""))
  } catch (e) {
    return { ok: false, accounts: [] }
  }
  var list = data && Array.isArray(data.accounts) ? data.accounts.slice(0, MAX_ACCOUNTS) : []
  var perAccount = Math.max(1, Math.min(MAX_FILES, limit || 8))
  var accounts = []
  for (var i = 0; i < list.length; i++) {
    var a = list[i] || {}
    var mirror = text(a.mirror)
    var files = []
    var raw = Array.isArray(a.files) ? a.files.slice(0, perAccount) : []
    for (var j = 0; j < raw.length; j++) {
      var f = raw[j] || {}
      var path = text(f.path)
      files.push({
        name: text(f.name) || "?",
        folder: path ? path.substring(0, path.lastIndexOf("/")) || "/" : "",
        changed: stamp(f.changed),
        by: text(f.modifiedBy),
        size: number(f.size),
        webUrl: webUrl(f.webUrl),
        local: localPath(f.local, mirror),
        isLocal: f.isLocal === true
      })
    }
    accounts.push({
      id: text(a.id),
      name: text(a.name),
      driveName: text(a.driveName) || "OneDrive",
      active: a.active === true,
      used: number(a.used),
      total: number(a.total),
      error: text(a.error),
      files: files
    })
  }
  return { ok: true, accounts: accounts }
}

function bytes(n, fmt) {
  var units = ["B", "KB", "MB", "GB", "TB"]
  var i = 0
  while (n >= 1024 && i < units.length - 1) { n /= 1024; i++ }
  return fmt(n, i === 0 || n >= 100 ? 0 : 1) + " " + units[i]
}
