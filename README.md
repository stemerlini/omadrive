# Omadrive

Two-pane file manager for Omarchy (C++ / Qt Quick): local folders on the left, OneDrive (through Microsoft Graph) or a second local folder on the right.

Keys: `Tab` switch pane, `Space` preview, `Ins`/`Ctrl+Space` mark, `Ctrl+A` mark all, `Enter`/`→` open, `Backspace`/`←` up, `Home`/`End`/`PgUp`/`PgDn` move, `F5` copy, `F6` move, `F7` new folder, `F2` rename, `F8`/`Delete` trash, `Ctrl+H` hidden, `Ctrl+O` right pane OneDrive/local, `Ctrl+Shift+L` sign out, `?` help, `Q` quit.

Mouse: click selects, `Ctrl`+click toggles a mark, `Shift`+click marks a range, double-click opens, right-click opens the menu, the path bar is clickable. Drag items between panes, onto a folder, or to and from other apps; hold `Shift` while dropping to move.

## OneDrive mirror

OneDrive files download into `~/OneDrive` at the same path (set `"mirror"` in `~/.config/omadrive/auth.json` to change it). Each OneDrive item shows whether it is online only (cloud), available on this device (check), or different from the local copy (arrows). Opening a OneDrive file downloads it to the mirror first. A local copy that is newer than OneDrive's is never replaced; an older one goes to the trash before the new download. Dragging out of OneDrive copies to wherever you drop.

Originals are never overwritten: name clashes get a ` (2)` suffix.

```
bin/build     # build to build/omadrive
bin/test      # Qt Test suite
bin/install   # build and install the package
```
