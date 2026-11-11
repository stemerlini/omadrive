# Omadrive

![omadrive](assets/banner.png)

Tabbed file manager for Omarchy (C++ / Qt Quick), modelled on One Commander, with OneDrive status and actions inside rclone-mounted folders. One window, one pane; `Ctrl+\` opens a second pane beside it for copying, and `omadrive <folder>` opens a folder. Above the files, folder columns (One Commander's Miller columns) show every folder on the way to the current one; the folders above the current one are thin strips with their names turned 90° (click one to go there), and the current folder is a full column of its folders and files that follows the highlight. Hide them with `` ` `` or `Ctrl+B`. Tabs, the split and the highlighted files are remembered in `~/.config/omadrive/tabs.conf`.

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
| `Ctrl+R` / `F5`, `Ctrl+H` | Refresh, hidden files |
| `?`, `Ctrl+Q` | Help, quit |

Mouse: click selects, `Ctrl`+click toggles a mark, `Shift`+click marks a range, double-click opens, right-click opens the menu, middle-click opens a folder in a new tab (or closes a tab), and clicking beside the path lets you type one. Drag items between panes, onto a folder or a tab, or to and from other apps; hold `Shift` while dropping to move.

## OneDrive with rclone

OneDrive accounts are [rclone](https://rclone.org) remotes, each mounted as a folder (`~/OneDrive-Personal`, `~/OneDrive-SES`, …) by the packaged systemd user unit `omadrive-rclone@<remote>.service`. omadrive browses a mount like any local folder; inside one, each file shows its state, read from rclone's cache (`~/.cache/rclone/vfsMeta/`): online only (cloud), on this device (check) or waiting to upload (cloud with an arrow). Outside the mounts there are no cloud features.

Right-click in a mount for the cloud actions: **Download to this device** fetches files (whole folders too) into rclone's cache, and **Copy view link** / **Copy edit link** put a sharing link on the clipboard (for a work account, a link for your organisation). rclone frees space itself, dropping the least recently used files once the cache passes 50 GB.

```
omadrive cloud connect                    # sign in to an account, pick its drive, mount it
omadrive cloud status                     # accounts, mounts, uploads waiting, storage (JSON)
omadrive cloud mount <remote> [folder]    # mount (default ~/OneDrive-<Name>) and start at login
omadrive cloud unmount <remote>
```

`connect` runs `rclone config create onedrive-<name> onedrive`; when rclone lists the drives, pick the one called "OneDrive" (your own files), not a SharePoint library. A work account may need its IT admin to approve rclone first. Each mount is described by `~/.config/omadrive/cloud/<remote>.env` (`REMOTE=`, `MOUNTPOINT=`) and serves rclone's remote control on `$XDG_RUNTIME_DIR/omadrive-rclone-<remote>.sock`, which omadrive uses for the upload queue and storage. The mount folder must be empty, and two accounts' folders cannot overlap.

## Bar panel

`omarchy-plugin/install.sh` adds a cloud icon to the Omarchy bar. Its panel lists each OneDrive account with its storage, whether it is mounted and how many files wait to upload (the icon shows an arrow while any do). Click an account to open its folder in omadrive, or to mount it when it is not; **Connect** adds an account in a terminal. In the panel `r` refreshes, `o` opens omadrive, `m` mounts or unmounts and `c` connects. It runs `omadrive cloud status`.

## Agents (CLI and MCP)

`omadrive agent <command>` is a headless JSON interface to the same accounts, through rclone, for agents to use:

```
omadrive agent accounts
omadrive agent list /Documents [--account <name>]
omadrive agent stat <path>            omadrive agent search <query>
omadrive agent cat <file>             omadrive agent download <file> [--dest <dir>]
omadrive agent upload <localFile> <remoteParent>
omadrive agent mkdir <remoteParent> <name>
omadrive agent rename <path> <newName>
omadrive agent trash <path>
omadrive agent share <path> [--edit] [--scope anonymous|organization]
omadrive agent status <path>          omadrive agent pending
omadrive agent refresh
```

Paths are from the drive root (`/Documents/a.pdf`) or inside a mount folder, which also picks the account; otherwise `--account` names it (`onedrive-ses`, `SES`), defaulting to the personal one. Each command prints one JSON object, `{"ok":true,…}` or `{"ok":false,"error":…}`. Uploads and renames never overwrite, `trash` uses the OneDrive recycle bin, and work files never leave their account: a file from a work account's folder cannot be uploaded to another account, nor a work file downloaded into another account's folder. When the account is mounted, `download` and `cat` read through the mount (so changes not yet uploaded are included) and changes made on OneDrive show in the folder at once. `search` uses OneDrive's own search.

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
