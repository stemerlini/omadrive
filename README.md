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
| `Ctrl+Shift+L`, `?`, `Ctrl+Q` | Sign out, help, quit |

Mouse: click selects, `Ctrl`+click toggles a mark, `Shift`+click marks a range, double-click opens, right-click opens the menu, middle-click opens a folder in a new tab (or closes a tab), and clicking beside the path lets you type one. Drag items between panes, onto a folder or a tab, or to and from other apps; hold `Shift` while dropping to move.

## OneDrive mirror

OneDrive files download into `~/OneDrive` at the same path (set `"mirror"` in `~/.config/omadrive/auth.json` to change it). Each OneDrive item shows whether it is online only (cloud), available on this device (check), or different from the local copy (arrows). Opening a OneDrive file downloads it to the mirror first. A local copy that is newer than OneDrive's is never replaced; an older one goes to the trash before the new download. Dragging out of OneDrive copies to wherever you drop.

Originals are never overwritten: name clashes get a ` (2)` suffix.

```
bin/build     # build to build/omadrive
bin/test      # Qt Test suite
bin/install   # build and install the package
```
