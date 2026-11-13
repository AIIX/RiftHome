#!/bin/sh
# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: Apache-2.0
# Puts "Rift Home" on the login screen: /usr/share/wayland-sessions/rift-home.desktop, which needs
# root (run with sudo). Plasma stays the default session. To take it away again: sudo rm
# /usr/share/wayland-sessions/rift-home.desktop
set -eu
HERE=$(dirname "$(readlink -f "$0")")
ROOT=$(readlink -f "$HERE/..")
if [ "$(id -u)" -ne 0 ]; then
	echo "Needs root: sudo $0" >&2
	exit 1
fi
FILE=/usr/share/wayland-sessions/rift-home.desktop
sed "s|@ROOT@|$ROOT|g" "$ROOT/data/rift-home-session.desktop.in" >"$FILE.new"
chmod 644 "$FILE.new"
mv "$FILE.new" "$FILE"
echo "installed $FILE (Exec: $ROOT/scripts/rift-home-session-start)"
