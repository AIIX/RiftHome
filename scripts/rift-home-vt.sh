#!/bin/sh
# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: Apache-2.0
# Rift Home as its own session on a spare console for the first tries: from a text login on
# Ctrl+Alt+F3 while Plasma stays on its own console. Rift Home drives the monitor (its status page) and input, lends the
# headset to RiftLinux's runtime, which it starts itself (Wayland direct mode only), and shows the windows in the headset.
# Leave: the monitor's Leave button or Ctrl+Alt+Backspace (each twice), or Settings -> Power in the headset; Ctrl+Alt+F1
# or F2 goes back to Plasma meanwhile. Apps use the user's own settings and profiles unless Settings -> Power says
# otherwise.
# Log: ~/.local/state/rift-home/logs/vt-<time>.log (run/ in the source tree).
# Usage: rift-home-console [rift-home options] (rift-home-vt.sh in the source tree)
set -u
HERE=$(dirname "$(readlink -f "$0")")
# Installed: the commands next to this one, state and logs in ~/.local/state/rift-home. From the source tree: its
# build, and the state, apps' own folder and logs in run/ there.
if [ -x "$HERE/../build/rift-home" ] && [ -f "$HERE/../CMakeLists.txt" ]; then
	ROOT=$(readlink -f "$HERE/..")
	RUN=$HERE/rift-home-run.sh
	LOGS=$ROOT/run
	set -- --state-dir "$ROOT/run/state-frame" --isolate-xdg "$ROOT/run/xdg" "$@"
else
	RUN=$HERE/rift-home-run
	LOGS=${XDG_STATE_HOME:-$HOME/.local/state}/rift-home/logs
fi
if [ -n "${WAYLAND_DISPLAY:-}" ] || [ -n "${DISPLAY:-}" ]; then
	echo "Start this from a text console (Ctrl+Alt+F3 and log in), not inside a desktop." >&2
	exit 1
fi
# One runtime on the Rift: SteamVR or a runtime started in Plasma must be quit there first (never stopped from here).
for busy in vrserver vrcompositor monado-service; do
	if pgrep -x "$busy" >/dev/null 2>&1; then
		echo "$busy is running (SteamVR or a RiftLinux runtime in Plasma): quit it there first." >&2
		exit 1
	fi
done
if pgrep -x rift-home >/dev/null 2>&1; then
	echo "Rift Home is already running." >&2
	exit 1
fi
NAME="vt-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$LOGS"
export QT_FORCE_STDERR_LOGGING=1
# KDE apps read their settings as on the desktop (Rift Home's longer double-click time among them).
export XDG_CURRENT_DESKTOP="${XDG_CURRENT_DESKTOP:-KDE}"
echo "Rift Home on this console: log $LOGS/$NAME.log"
"$RUN" --backend drm --xr --start-runtime --log-dir "$LOGS/$NAME/apps" "$@" >"$LOGS/$NAME.log" 2>&1
STATUS=$?
echo "Rift Home ended (status $STATUS). The last lines of its log:"
tail -n 15 "$LOGS/$NAME.log"
exit "$STATUS"
