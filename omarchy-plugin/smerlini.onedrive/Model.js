// Validates what `pathfinder cloud status` printed before the panel shows any of it.

var MAX_ACCOUNTS = 8
var MAX_TEXT = 300
var MAX_PATH = 1024

function text(value, limit) {
  var max = limit || MAX_TEXT
  var s = typeof value === "string" ? value : ""
  s = s.replace(/[\u0000-\u001f\u007f-\u009f\u200e\u200f\u2028-\u202e\u2066-\u2069]/g, " ")
  return s.length > max ? s.substring(0, max - 1) + "…" : s
}

function number(value) {
  var n = typeof value === "number" ? value : Number(value)
  return isFinite(n) && n >= 0 ? Math.floor(n) : 0
}

// rclone remote names are the identifier passed back to `pathfinder cloud`.
function remote(value) {
  var s = typeof value === "string" ? value : ""
  return s.length > 0 && s.length <= 64 && /^[A-Za-z0-9_.-]+$/.test(s) && s.charAt(0) !== "-" ? s : ""
}

// A mount folder must be absolute and never climb out with "..".
function mountpoint(value) {
  var s = typeof value === "string" ? value : ""
  if (s.length === 0 || s.length > MAX_PATH || s.charAt(0) !== "/") return ""
  if (/[\u0000-\u001f\u007f]/.test(s)) return ""
  if (/(^|\/)\.\.(\/|$)/.test(s)) return ""
  return s
}

function parse(raw) {
  var data
  try {
    data = JSON.parse(String(raw || ""))
  } catch (e) {
    return { ok: false, error: "", accounts: [] }
  }
  if (!data || typeof data !== "object" || Array.isArray(data)) return { ok: false, error: "", accounts: [] }
  if (data.ok !== true) return { ok: false, error: text(data.error), accounts: [] }
  var list = Array.isArray(data.accounts) ? data.accounts : []
  var accounts = []
  var seen = {}
  for (var i = 0; i < list.length && accounts.length < MAX_ACCOUNTS; i++) {
    var a = list[i]
    if (!a || typeof a !== "object") continue
    var id = remote(a.remote)
    if (id === "" || seen[id]) continue
    seen[id] = true
    var work = a.work === true || a.driveType === "business"
    var mounted = a.mounted === true
    var used = number(a.used)
    var total = number(a.total)
    accounts.push({
      remote: id,
      label: text(a.label) || id,
      work: work,
      mountpoint: mountpoint(a.mountpoint),
      configured: a.configured === true,
      mounted: mounted,
      pending: mounted ? number(a.pending) : 0,
      used: total > 0 ? Math.min(used, total) : used,
      total: total,
      error: text(a.error)
    })
  }
  return { ok: true, error: "", accounts: accounts }
}

// Reply to `pathfinder cloud mount|unmount`.
function parseResult(raw) {
  try {
    var data = JSON.parse(String(raw || ""))
    if (data && data.ok === true) return { ok: true, error: "" }
    return { ok: false, error: text(data && data.error) }
  } catch (e) {
    return { ok: false, error: "" }
  }
}

function bytes(n, fmt) {
  var units = ["B", "KB", "MB", "GB", "TB"]
  var i = 0
  while (n >= 1024 && i < units.length - 1) { n /= 1024; i++ }
  return fmt(n, i === 0 || n >= 100 ? 0 : 1) + " " + units[i]
}
