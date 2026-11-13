#!/bin/sh
# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: Apache-2.0
# Puts the "Rift Home" entry in the desktop's menu: ~/.local/share/applications, this user only,
# nothing system-wide. To take it away again, delete
# ~/.local/share/applications/org.rifthome.RiftHome.desktop.
set -eu
HERE=$(dirname "$(readlink -f "$0")")
ROOT=$(readlink -f "$HERE/..")
DIR="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
FILE="$DIR/org.rifthome.RiftHome.desktop"
mkdir -p "$DIR"
sed "s|@ROOT@|$ROOT|g" "$ROOT/data/org.rifthome.RiftHome.desktop.in" >"$FILE.new"
mv "$FILE.new" "$FILE"
chmod +x "$ROOT/scripts/rift-home-start.sh"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$DIR" 2>/dev/null || true
echo "installed $FILE"
