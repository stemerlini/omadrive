#!/usr/bin/env node
// omadrive MCP server — exposes OneDrive accounts (rclone remotes, personal and
// work) to agents over the Model Context Protocol. Every tool shells out to
// `omadrive agent <cmd>` with an argument array (no shell), so the same account
// safety the CLI enforces applies here too: operations name their account and
// work files never leave their account.
//
// Transport: newline-delimited JSON-RPC 2.0 over stdio. No dependencies.

import { execFile } from "node:child_process";
import { readFileSync } from "node:fs";

const OMADRIVE = process.env.OMADRIVE_BIN || "omadrive";
const PROTOCOL_VERSION = "2024-11-05";

let pkgVersion = "0.1.0";
try {
  pkgVersion = JSON.parse(readFileSync(new URL("./package.json", import.meta.url), "utf8")).version || pkgVersion;
} catch {
  /* ignore */
}

// Runs `omadrive agent <args...>` and resolves with the parsed JSON object the
// CLI prints. A non-zero exit still carries a JSON {ok:false,error} line.
function runAgent(args) {
  return new Promise((resolve) => {
    execFile(OMADRIVE, ["agent", ...args], { maxBuffer: 8 * 1024 * 1024, timeout: 600000 }, (err, stdout, stderr) => {
      const text = (stdout || "").trim();
      if (text) {
        try {
          resolve(JSON.parse(text.split("\n").pop()));
          return;
        } catch {
          /* fall through to error below */
        }
      }
      if (err && err.code === "ENOENT") {
        resolve({ ok: false, error: "omadrive is not installed on PATH (set OMADRIVE_BIN to the binary)" });
        return;
      }
      resolve({ ok: false, error: (stderr || "").trim() || (err && err.message) || "omadrive produced no output" });
    });
  });
}

// Appends --account when the caller named one.
function withAccount(args, a) {
  return a ? [...args, "--account", String(a)] : args;
}

const accountProp = {
  account: { type: "string", description: "Account name (e.g. onedrive-personal, Personal, SES); defaults to the personal account." },
};

const tools = [
  {
    name: "onedrive_accounts",
    description: "List the connected OneDrive accounts (rclone remotes): id, name, whether it is a work account, and the local folder it is mounted at.",
    inputSchema: { type: "object", properties: {} },
    run: () => runAgent(["accounts"]),
  },
  {
    name: "onedrive_list",
    description: "List the items directly inside a OneDrive folder. Path is from the drive root, e.g. \"/\" or \"/Documents\".",
    inputSchema: { type: "object", properties: { path: { type: "string", description: "Folder path from the drive root." }, ...accountProp }, required: ["path"] },
    run: (a) => runAgent(withAccount(["list", a.path], a.account)),
  },
  {
    name: "onedrive_stat",
    description: "Get metadata for one OneDrive item (type, size, last changed).",
    inputSchema: { type: "object", properties: { path: { type: "string", description: "Item path from the drive root." }, ...accountProp }, required: ["path"] },
    run: (a) => runAgent(withAccount(["stat", a.path], a.account)),
  },
  {
    name: "onedrive_search",
    description: "Search a OneDrive account for files and folders whose name or content matches a query (up to 200 hits).",
    inputSchema: { type: "object", properties: { query: { type: "string", description: "Text to search for." }, ...accountProp }, required: ["query"] },
    run: (a) => runAgent(withAccount(["search", a.query], a.account)),
  },
  {
    name: "onedrive_read",
    description: "Read a small (<1 MB) text file from OneDrive and return its contents inline.",
    inputSchema: { type: "object", properties: { path: { type: "string", description: "File path from the drive root." }, ...accountProp }, required: ["path"] },
    run: (a) => runAgent(withAccount(["cat", a.path], a.account)),
  },
  {
    name: "onedrive_download",
    description: "Download one OneDrive file to a local directory (a temp dir by default) and return the local path.",
    inputSchema: { type: "object", properties: { path: { type: "string", description: "File path from the drive root." }, dest: { type: "string", description: "Local directory to download into. Optional." }, ...accountProp }, required: ["path"] },
    run: (a) => runAgent(withAccount(a.dest ? ["download", a.path, "--dest", a.dest] : ["download", a.path], a.account)),
  },
  {
    name: "onedrive_upload",
    description: "Upload a local file into a OneDrive folder. Refuses to overwrite an existing item, and files in a work account's folder are never carried into another account.",
    inputSchema: { type: "object", properties: { localFile: { type: "string", description: "Absolute path of the local file to upload." }, remoteParent: { type: "string", description: "Destination folder path from the drive root." }, ...accountProp }, required: ["localFile", "remoteParent"] },
    run: (a) => runAgent(withAccount(["upload", a.localFile, a.remoteParent], a.account)),
  },
  {
    name: "onedrive_mkdir",
    description: "Create a folder in OneDrive.",
    inputSchema: { type: "object", properties: { remoteParent: { type: "string", description: "Parent folder path from the drive root." }, name: { type: "string", description: "New folder name." }, ...accountProp }, required: ["remoteParent", "name"] },
    run: (a) => runAgent(withAccount(["mkdir", a.remoteParent, a.name], a.account)),
  },
  {
    name: "onedrive_rename",
    description: "Rename a OneDrive item; refuses when the new name already exists.",
    inputSchema: { type: "object", properties: { path: { type: "string", description: "Item path from the drive root." }, newName: { type: "string", description: "New name (no slashes)." }, ...accountProp }, required: ["path", "newName"] },
    run: (a) => runAgent(withAccount(["rename", a.path, a.newName], a.account)),
  },
  {
    name: "onedrive_claim",
    description: "Claim a OneDrive item so only one machine processes it: moves it on OneDrive into <parent>/.claimed/<hostname>/ (or `as`: a new name there, or a full destination path). Returns {ok:true, path} when this machine won; {ok:false, reason:\"taken\"} when another machine got it first, reason \"pending\" when a mounted file still has changes to upload. Path is from the drive root or inside a mounted folder.",
    inputSchema: { type: "object", properties: { path: { type: "string", description: "Item path." }, as: { type: "string", description: "New name inside .claimed/<host>/, or a full destination path. Optional." }, ...accountProp }, required: ["path"] },
    run: (a) => runAgent(withAccount(a.as ? ["claim", a.path, "--as", a.as] : ["claim", a.path], a.account)),
  },
  {
    name: "onedrive_trash",
    description: "Move a OneDrive item (file or folder) to the OneDrive recycle bin.",
    inputSchema: { type: "object", properties: { path: { type: "string", description: "Item path from the drive root." }, ...accountProp }, required: ["path"] },
    run: (a) => runAgent(withAccount(["trash", a.path], a.account)),
  },
  {
    name: "onedrive_share",
    description: "Create a sharing link for a OneDrive item and return the URL.",
    inputSchema: {
      type: "object",
      properties: {
        path: { type: "string", description: "Item path from the drive root." },
        edit: { type: "boolean", description: "If true the link allows editing, otherwise view only." },
        scope: { type: "string", enum: ["anonymous", "organization"], description: "Link audience; defaults to organization for work accounts and anonymous otherwise." },
        ...accountProp,
      },
      required: ["path"],
    },
    run: (a) => {
      let args = ["share", a.path];
      if (a.edit) args.push("--edit");
      if (a.scope) args.push("--scope", a.scope);
      return runAgent(withAccount(args, a.account));
    },
  },
  {
    name: "onedrive_sync_status",
    description: "State of an item in a mounted OneDrive folder: online (not downloaded), local (on this device), pending (waiting to upload) or folder, with size and modified time. Path is from the drive root or inside the mounted folder.",
    inputSchema: { type: "object", properties: { path: { type: "string", description: "Item path." }, ...accountProp }, required: ["path"] },
    run: (a) => runAgent(withAccount(["status", a.path], a.account)),
  },
  {
    name: "onedrive_pending",
    description: "List the files each mounted OneDrive folder still has to upload.",
    inputSchema: { type: "object", properties: { ...accountProp } },
    run: (a) => runAgent(withAccount(["pending"], a.account)),
  },
];

const toolByName = new Map(tools.map((t) => [t.name, t]));

function send(message) {
  process.stdout.write(JSON.stringify(message) + "\n");
}

function reply(id, result) {
  send({ jsonrpc: "2.0", id, result });
}

function replyError(id, code, message) {
  send({ jsonrpc: "2.0", id, error: { code, message } });
}

async function handle(msg) {
  const { id, method, params } = msg;
  // Notifications have no id and need no reply.
  if (id === undefined || id === null) {
    return;
  }
  switch (method) {
    case "initialize":
      reply(id, {
        protocolVersion: PROTOCOL_VERSION,
        capabilities: { tools: {} },
        serverInfo: { name: "omadrive", version: pkgVersion },
      });
      return;
    case "ping":
      reply(id, {});
      return;
    case "tools/list":
      reply(id, { tools: tools.map(({ name, description, inputSchema }) => ({ name, description, inputSchema })) });
      return;
    case "tools/call": {
      const tool = toolByName.get(params?.name);
      if (!tool) {
        replyError(id, -32602, `Unknown tool: ${params?.name}`);
        return;
      }
      let out;
      try {
        out = await tool.run(params.arguments || {});
      } catch (e) {
        out = { ok: false, error: String((e && e.message) || e) };
      }
      reply(id, {
        content: [{ type: "text", text: JSON.stringify(out) }],
        isError: out && out.ok === false,
      });
      return;
    }
    default:
      replyError(id, -32601, `Method not found: ${method}`);
  }
}

let buffer = "";
let pending = 0;
let stdinEnded = false;
function maybeExit() {
  if (stdinEnded && pending === 0) process.exit(0);
}
process.stdin.setEncoding("utf8");
process.stdin.on("data", (chunk) => {
  buffer += chunk;
  let newline;
  while ((newline = buffer.indexOf("\n")) >= 0) {
    const line = buffer.slice(0, newline).trim();
    buffer = buffer.slice(newline + 1);
    if (!line) continue;
    let msg;
    try {
      msg = JSON.parse(line);
    } catch {
      continue;
    }
    pending++;
    handle(msg)
      .catch((e) => process.stderr.write(`omadrive-mcp: ${e}\n`))
      .finally(() => {
        pending--;
        maybeExit();
      });
  }
});
process.stdin.on("end", () => {
  stdinEnded = true;
  maybeExit();
});
