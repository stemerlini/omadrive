#!/bin/sh
# Installs the Cloud (OneDrive accounts) bar panel into the Omarchy shell and puts it on the bar.
set -e
src="$(cd "$(dirname "$0")" && pwd)/smerlini.onedrive"
dest="${XDG_CONFIG_HOME:-$HOME/.config}/omarchy/plugins/smerlini.onedrive"
omarchy plugin validate "$src"
mkdir -p "$dest"
cp -r "$src"/. "$dest"/
omarchy-shell shell rescanPlugins >/dev/null 2>&1 || true
sleep 1
omarchy plugin enable smerlini.onedrive --after omarchy.tray
echo "Installed to $dest"
