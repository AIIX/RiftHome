#!/bin/sh
# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: Apache-2.0
# Starts Rift Home from the desktop's menu (the "Rift Home" entry) with the headset worn: RiftLinux's runtime started
# for it (riftlinux-run), the user's own app data unless Settings says otherwise, no time limit. Leave it from
# Settings -> Power -> Leave Rift Home. No terminal: what goes wrong is said in a desktop notification. State and logs:
# in ~/.local/state/rift-home (logs/desktop-<time>.log); started from the source tree, in run/ there.
# Usage: rift-home-start [rift-home options] (rift-home-start.sh in the source tree)
set -u
HERE=$(dirname "$(readlink -f "$0")")
if [ -x "$HERE/../build/rift-home" ] && [ -f "$HERE/../CMakeLists.txt" ]; then
	ROOT=$(readlink -f "$HERE/..")
	RUN=$HERE/rift-home-run.sh
	STATE=$ROOT/run/state-frame
	LOGS=$ROOT/run
	ICON=$ROOT/data/rift-home.svg
	set -- --with-profile --state-dir "$STATE" --isolate-xdg "$ROOT/run/xdg" "$@"
else
	RUN=$HERE/rift-home-run
	STATE=${XDG_STATE_HOME:-$HOME/.local/state}/rift-home
	LOGS=$STATE/logs
	ICON=rift-home
fi
say() {
	notify-send --app-name="Rift Home" --icon="$ICON" "Rift Home" "$1" 2>/dev/null || echo "$1" >&2
}
if pgrep -x rift-home >/dev/null 2>&1; then
	say "Rift Home is already running."
	exit 0
fi
# The Rift serves one runtime: SteamVR or another session must be quit first (never stopped from here).
for busy in vrserver vrcompositor monado-service; do
	if pgrep -x "$busy" >/dev/null 2>&1; then
		case "$busy" in
		vr*) say "SteamVR is using the Rift: quit SteamVR, then start Rift Home again." ;;
		*) say "Another VR session is using the Rift (monado-service): end it, then start Rift Home again." ;;
		esac
		exit 1
	fi
done
if ! command -v riftlinux-run >/dev/null 2>&1; then
	say "RiftLinux is not installed (riftlinux-run is missing): Rift Home needs RiftLinux's runtime."
	exit 1
fi
if [ ! -x "$RUN" ]; then
	say "Rift Home is not built or installed ($RUN is missing)."
	exit 1
fi
# The user's own app data (Firefox must start with the user's profile from here). Firefox open on the desktop
# keeps its profile to itself: said before starting.
if pgrep -x firefox >/dev/null 2>&1; then
	say "Firefox is open on the desktop: close it there to use your Firefox profile in Rift Home."
fi
NAME="desktop-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$LOGS"
LOG="$LOGS/$NAME.log"
# Picture quality (Settings -> Display, display.composition: the runtime's picture in percent of the panel's pixels a
# side, 165 standard, up to 200): RiftLinux's XRT_COMPOSITOR_SCRATCH_HEADROOM (percent over the CV1's 1:1 size, 1.3216
# of the panel's) for the runtime started here.
COMPOSITION=$(sed -n 's/.*"composition": *\([0-9][0-9]*\).*/\1/p' "$STATE/settings.json" 2>/dev/null | head -n 1)
if [ "${COMPOSITION:-165}" -gt 165 ] 2>/dev/null; then
	export XRT_COMPOSITOR_SCRATCH_HEADROOM=$((COMPOSITION * 10000 / 13216))
fi
say "Starting: put the headset on. Leave from Settings -> Power."
set -- riftlinux-run "$RUN" --xr --log-dir "$LOGS/$NAME/apps" --quit-after 0 "$@"
if command -v kde-inhibit >/dev/null 2>&1; then
	# Keeps the desktop's screen on and the computer awake for as long as Rift Home runs.
	set -- env QT_QPA_PLATFORM=offscreen kde-inhibit --screenSaver --power env -u QT_QPA_PLATFORM "$@"
fi
"$@" >"$LOG" 2>&1
STATUS=$?
if [ "$STATUS" -ne 0 ]; then
	# The session's own reason when it gave one (the Rift in use, the runtime missing).
	REASON=$(grep -v '^\s*$' "$LOG" | tail -n 1)
	say "Rift Home stopped (status $STATUS): ${REASON:-see $LOG}"
fi
exit "$STATUS"
