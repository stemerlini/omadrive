# Omadrive

Tabbed file manager for Omarchy (C++ / Qt Quick) for local folders and OneDrive (through Microsoft Graph). One window, one pane; `Tab` flips between this device and OneDrive, and `F3` opens a second pane beside it for copying. Tabs, the split and the highlighted files are remembered in `~/.config/omadrive/tabs.conf`.

Keys: `Tab` local ⇄ OneDrive (with two panes: switch pane), `F3`/`Ctrl+\` one or two panes, `Ctrl+L` this pane local ⇄ OneDrive, `Ctrl+1…9` go to tab, `Ctrl+Tab`/`Ctrl+Shift+Tab` next/previous tab, `Ctrl+T` new tab here, `Ctrl+W` close tab, `Ctrl+C`/`Ctrl+X`/`Ctrl+V` copy, cut and paste across tabs, `Space` preview, `Ins`/`Ctrl+Space` mark, `Ctrl+A` mark all, `Enter`/`→` open, `Backspace`/`←` up, `Home`/`End`/`PgUp`/`PgDn` move, `F5` copy, `F6` move (to the other pane; with one pane they open the second), `F7` new folder, `F2` rename, `F8`/`Delete` trash, `Ctrl+H` hidden, `Ctrl+Shift+L` sign out, `?` help, `Q` quit.

Mouse: click selects, `Ctrl`+click toggles a mark, `Shift`+click marks a range, double-click opens, right-click opens the menu, middle-click closes a tab, the path bar is clickable. Drag items between panes, onto a folder or a tab, or to and from other apps; hold `Shift` while dropping to move.

## OneDrive mirror

OneDrive files download into `~/OneDrive` at the same path (set `"mirror"` in `~/.config/omadrive/auth.json` to change it). Each OneDrive item shows whether it is online only (cloud), available on this device (check), or different from the local copy (arrows). Opening a OneDrive file downloads it to the mirror first. A local copy that is newer than OneDrive's is never replaced; an older one goes to the trash before the new download. Dragging out of OneDrive copies to wherever you drop.

Originals are never overwritten: name clashes get a ` (2)` suffix.

```
bin/build     # build to build/omadrive
bin/test      # Qt Test suite
bin/install   # build and install the package
```
