# Omadrive

![omadrive](assets/banner.png)

Tabbed file manager for Omarchy (C++ / Qt Quick) for local folders and OneDrive (through Microsoft Graph), modelled on One Commander. One window, one pane; `Tab` flips between this device and OneDrive, and `Ctrl+\` opens a second pane beside it for copying. Above the files, folder columns (One Commander's Miller columns) show every folder on the way to the current one; the folders above the current one are thin strips with their names turned 90° (click one to go there), and the current folder is a full column of its folders and files that follows the highlight. Hide them with `` ` `` or `Ctrl+B`. Tabs, the split and the highlighted files are remembered in `~/.config/omadrive/tabs.conf`.

Keys follow Windows and One Commander:

| Keys | Action |
| --- | --- |
| `Tab` | Local ⇄ OneDrive (with two panes: switch pane, also `Alt+F`) |
| `Ctrl+\` | One or two panes |
| `Ctrl+T` / `Ctrl+W` / `Ctrl+Tab` / `Ctrl+1…9` | New tab here / close / next / go to tab |
| `Ctrl+N` | New file or folder: folder, text, Markdown, shell script, empty file |
| `Ctrl+Shift+N` | New window |
| `Enter`, `Backspace` / `Alt+↑`, `Alt+←` / `Alt+→` | Open, parent folder, back / forward |
| `Ctrl+L` / `Alt+D` | Type a path |
| `` ` `` / `Ctrl+B` | Show or hide the folder columns |
| `Space` | Preview |
| `Ctrl+Space` / `Ins`, `Ctrl+A`, `Esc` | Mark, select all, clear marks |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Copy, cut, paste (across tabs too) |
| `Ctrl+Shift+C` | Copy path |
| `Alt+C` / `Alt+M` | Copy / move to the other pane (from OneDrive: into the mirror) |
| `F2`, `Delete` | Rename, trash |
| `Ctrl+R` / `F5`, `Ctrl+H` | Refresh, hidden files |
| `Ctrl+Shift+A` | Switch OneDrive account |
| `Ctrl+Shift+L`, `?`, `Ctrl+Q` | Sign out of this account, help, quit |

Mouse: click selects, `Ctrl`+click toggles a mark, `Shift`+click marks a range, double-click opens, right-click opens the menu, middle-click opens a folder in a new tab (or closes a tab), and clicking beside the path lets you type one. Drag items between panes, onto a folder or a tab, or to and from other apps; hold `Shift` while dropping to move.

## Accounts

Add a personal and a work or school account from the round picture at the top right; it shows the active account, and the others sit beside it, one click (or `Ctrl+Shift+A`) to switch. Every account stays signed in, its sign-in kept in the system keyring, and each tab remembers its folder per account. The list lives in `~/.config/omadrive/accounts.json`. A work account may need its IT admin to approve the app first.

Every OneDrive operation is tied to the account its files were picked in: switching account while a copy, a cut, a drag or a dialog is pending never sends it to the other drive, and copying straight from one account to another is refused. Deleting from OneDrive, or moving files out of it, always asks first and names the drive (for example "OneDrive - SES"). Files can go into a work account from anywhere, but files from a work account (its OneDrive or its local folder) never go to another account or that account's folder; copying them to an ordinary local folder is still allowed.

## OneDrive mirror

OneDrive files download into a mirror folder at the same path: `~/OneDrive-Personal` for a personal account (or `"mirror"` in `~/.config/omadrive/auth.json`), and `~/OneDrive-<domain>` for a work account, named after its email domain (`~/OneDrive-SES` for an `@ses.com` account). A `~/OneDrive` folder from earlier versions is renamed to `~/OneDrive-Personal`. Each OneDrive item shows whether it is online only (cloud), available on this device (check), or different from the local copy (arrows). Opening a OneDrive file downloads it to the mirror first. A local copy that is newer than OneDrive's is never replaced; an older one goes to the trash before the new download. Dragging out of OneDrive copies to wherever you drop.

Right-click a OneDrive item for its cloud actions: **Always keep on this device** downloads it to the mirror; **Free up space** removes mirror copies that match OneDrive (changed files are kept); **Share** copies a view or edit link to the clipboard (for a work account, a link for your organisation or for anyone), or invites people by email; **Open on the web** opens it in OneDrive online.

Originals are never overwritten: name clashes get a ` (2)` suffix.

## Bar panel

`omarchy-plugin/install.sh` adds a OneDrive icon to the Omarchy bar, beside the tray. Its panel lists the files most recently changed in each account, with the storage used; a tick marks files on this device, which open from the mirror, and the rest open on the web. Click opens the panel, right-click checks now, middle-click opens omadrive; in the panel `r` checks and `o` opens omadrive. It runs `omadrive --recent N`, which prints the same as JSON; it reads OneDrive's change feed, so the first check reads the whole drive (about half a minute), and later ones only what changed, cached in `~/.cache/omadrive/recent/`. It checks when opened and every five minutes, which its bar settings can turn off.

## Agents (CLI and MCP)

`omadrive agent <command>` is a headless, JSON interface over the same accounts, sign-in and safety as the app, for agents to use:

```
omadrive agent accounts
omadrive agent list /Documents [--account <id|email>]
omadrive agent stat <path>            omadrive agent search <query>
omadrive agent cat <file>             omadrive agent download <file> [--dest <dir>]
omadrive agent upload <localFile> <remoteParent>
omadrive agent mkdir <remoteParent> <name>
omadrive agent rename <path> <newName>
omadrive agent trash <path>
omadrive agent share <path> [--edit] [--scope anonymous|organization]
```

Each command names its account (the active one by default) and prints one JSON object, `{"ok":true,…}` or `{"ok":false,"error":…}`. The account safety is enforced here, so it holds for every caller:

- Work files never leave their account: a file from a work OneDrive (its drive or its mirror folder) cannot be uploaded into, or downloaded into the mirror of, another account.
- **A work (e.g. SES) account may write only in your own personal OneDrive, never in a SharePoint shared library mounted into it.** `upload`, `mkdir`, `rename` and `trash` check the target's drive first and refuse anything on a SharePoint site. Reading a shared library is still allowed.

`mcp/omadrive-mcp.mjs` is a dependency-free MCP (Model Context Protocol) server over stdio that exposes these as tools (`onedrive_list`, `onedrive_read`, `onedrive_upload`, …), for any MCP client. It runs `omadrive agent` under the hood, so the same rules apply. Register it with GitHub Copilot CLI:

```
copilot mcp add omadrive --env OMADRIVE_BIN=/usr/bin/omadrive -- node /usr/lib/omadrive/mcp/omadrive-mcp.mjs
```

`OMADRIVE_BIN` points at the omadrive binary (defaults to `omadrive` on `PATH`). The server needs `nodejs`.

```
bin/build     # build to build/omadrive
bin/test      # Qt Test suite
bin/install   # build and install the package
```
