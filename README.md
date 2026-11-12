# Pathfinder

![pathfinder](assets/banner.png)

Tabbed file manager for Omarchy (C++ / Qt Quick), modelled on One Commander, with OneDrive status and actions inside rclone-mounted folders. One window, one pane; `Ctrl+\` opens a second pane beside it for copying, and `pathfinder <folder>` opens a folder. Above the files, folder columns (One Commander's Miller columns) show every folder on the way to the current one; the folders above the current one are thin strips with their names turned 90° (click one to go there), the current folder is a full column of its folders and files that follows the highlight, and the last column looks ahead: the contents of the highlighted folder (click an entry there to go in with it highlighted), or the highlighted file's size, date and OneDrive state. Hide them with `` ` `` or `Ctrl+B`. Each row shows, as in One Commander, an icon and colour for the kind of file, the name with its extension dimmed, the type, the size with a bar against the largest file in the folder (or the number of items in a folder, counted in the background), and how long ago it changed, coloured by how recent (the exact date on hover); hidden files are dimmed and links marked ⇢. Click a column title to sort by it, again to reverse; the status line totals folders, files and size, or the size of the marked items. Colour labels (red, orange, yellow, green, blue, purple, gray) tint a row and colour its name; set them with `Alt+1…7` or the menu's Label entry, clear with `Alt+0`. They are kept on this device in `~/.config/pathfinder/labels.json` and follow renames, moves and trashing done in Pathfinder. Colours and font follow the current Omarchy theme (its whole palette and the monospace font), falling back to Tokyo Night. Tabs, the split and the highlighted files are remembered in `~/.config/pathfinder/tabs.conf`.

Keys follow Windows and One Commander:

| Keys | Action |
| --- | --- |
| `Tab` / `Alt+F` | Switch pane |
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
| `Alt+C` / `Alt+M` | Copy / move to the other pane |
| `F2`, `Delete` | Rename, trash |
| `Alt+1…7` / `Alt+0` | Colour label / remove it |
| `Ctrl+R` / `F5`, `Ctrl+H` | Refresh, hidden files |
| `?`, `Ctrl+Q` | Help, quit |

Mouse: click selects, `Ctrl`+click toggles a mark, `Shift`+click marks a range, double-click opens, right-click opens the menu, middle-click opens a folder in a new tab (or closes a tab), and clicking beside the path lets you type one. Drag items between panes, onto a folder or a tab, or to and from other apps; hold `Shift` while dropping to move.

## OneDrive with rclone

OneDrive accounts are [rclone](https://rclone.org) remotes, each mounted as a folder (`~/OneDrive-Personal`, `~/OneDrive-SES`, …) by the packaged systemd user unit `pathfinder-rclone@<remote>.service`. pathfinder browses a mount like any local folder; inside one, each file shows its state, read from rclone's cache (`~/.cache/rclone/vfsMeta/`): online only (cloud), on this device (check) or waiting to upload (cloud with an arrow). Outside the mounts there are no cloud features.

Right-click in a mount for the cloud actions: **Download to this device** fetches files (whole folders too) into rclone's cache, and **Copy view link** / **Copy edit link** put a sharing link on the clipboard (for a work account, a link for your organisation). rclone frees space itself, dropping the least recently used files once the cache passes 50 GB.

```
pathfinder cloud connect                    # sign in to an account, pick its drive, mount it
pathfinder cloud status                     # accounts, mounts, uploads waiting, storage (JSON)
pathfinder cloud mount <remote> [folder]    # mount (default ~/OneDrive-<Name>) and start at login
pathfinder cloud unmount <remote>
```

`connect` runs `rclone config create onedrive-<name> onedrive`; when rclone lists the drives, pick the one called "OneDrive" (your own files), not a SharePoint library. A work account may need its IT admin to approve rclone first. Each mount is described by `~/.config/pathfinder/cloud/<remote>.env` (`REMOTE=`, `MOUNTPOINT=`) and serves rclone's remote control on `$XDG_RUNTIME_DIR/pathfinder-rclone-<remote>.sock`, which pathfinder uses for the upload queue and storage. The mount folder must be empty, and two accounts' folders cannot overlap.

## Bar panel

`omarchy-plugin/install.sh` adds a cloud icon to the Omarchy bar. Its panel lists each OneDrive account with its storage, whether it is mounted and how many files wait to upload (the icon shows an arrow while any do). Click an account to open its folder in pathfinder, or to mount it when it is not; **Connect** adds an account in a terminal. In the panel `r` refreshes, `o` opens pathfinder, `m` mounts or unmounts and `c` connects. It runs `pathfinder cloud status`.

## Agents (CLI and MCP)

`pathfinder agent <command>` is a headless JSON interface to the same accounts, through rclone, for agents to use:

```
pathfinder agent accounts
pathfinder agent list /Documents [--account <name>]
pathfinder agent stat <path>            pathfinder agent search <query>
pathfinder agent cat <file>             pathfinder agent download <file> [--dest <dir>]
pathfinder agent upload <localFile> <remoteParent>
pathfinder agent mkdir <remoteParent> <name>
pathfinder agent rename <path> <newName>
pathfinder agent trash <path>
pathfinder agent share <path> [--edit] [--scope anonymous|organization]
pathfinder agent status <path>          pathfinder agent pending
pathfinder agent refresh
```

Paths are from the drive root (`/Documents/a.pdf`) or inside a mount folder, which also picks the account; otherwise `--account` names it (`onedrive-ses`, `SES`), defaulting to the personal one. Each command prints one JSON object, `{"ok":true,…}` or `{"ok":false,"error":…}`. Uploads and renames never overwrite, `trash` uses the OneDrive recycle bin, and work files never leave their account: a file from a work account's folder cannot be uploaded to another account, nor a work file downloaded into another account's folder. When the account is mounted, `download` and `cat` read through the mount (so changes not yet uploaded are included) and changes made on OneDrive show in the folder at once. `search` uses OneDrive's own search.

`mcp/pathfinder-mcp.mjs` is a dependency-free MCP (Model Context Protocol) server over stdio that exposes these as tools (`onedrive_list`, `onedrive_read`, `onedrive_upload`, …), for any MCP client. It runs `pathfinder agent` under the hood, so the same rules apply. Register it with GitHub Copilot CLI:

```
copilot mcp add pathfinder --env PATHFINDER_BIN=/usr/bin/pathfinder -- node /usr/lib/pathfinder/mcp/pathfinder-mcp.mjs
```

`PATHFINDER_BIN` points at the pathfinder binary (defaults to `pathfinder` on `PATH`). The server needs `nodejs`.

```
bin/build     # build to build/pathfinder
bin/test      # Qt Test suite
bin/install   # build and install the package
```
