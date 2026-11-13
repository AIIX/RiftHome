#!/bin/sh
# SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
# SPDX-License-Identifier: Apache-2.0
# Starts Rift Home with its own D-Bus session, so programs started inside it never reach instances already running on
# the desktop (Firefox, Dolphin and other single-instance programs would open their window there instead).
# Headless (default): no window, no input; works from an SSH login. Nested (--backend nested): a window on the
# desktop; from an SSH login the desktop session's display is taken from the user's service manager.
# Usage: rift-home-run [rift-home options] (rift-home-run.sh in the source tree)
HERE=$(dirname "$(readlink -f "$0")")
# The source tree's build, else the installed program next to this command.
if [ -n "${RIFT_HOME_BIN:-}" ]; then
	BIN=$RIFT_HOME_BIN
elif [ -x "$HERE/../build/rift-home" ] && [ -f "$HERE/../CMakeLists.txt" ]; then
	BIN=$HERE/../build/rift-home
else
	BIN=$HERE/rift-home
fi

case " $* " in
*" nested "* | *"--backend=nested"*)
	if [ -z "$WAYLAND_DISPLAY" ]; then
		WAYLAND_DISPLAY=$(systemctl --user show-environment 2>/dev/null | sed -n 's/^WAYLAND_DISPLAY=//p')
		export WAYLAND_DISPLAY
	fi
	;;
*)
	unset WAYLAND_DISPLAY
	;;
esac
# Never an X display: X11 programs would open on the desktop (or, over SSH, on another computer's screen).
unset DISPLAY
# A silent notify-send stand-in in run/bin (used by test scripts) is not for the apps: theirs reach Rift
# Home's notification server.
ROOT=$(readlink -f "$HERE/..")
PATH=$(printf '%s' "$PATH" | tr ':' '\n' | grep -vxF "$ROOT/run/bin" | paste -sd:)
export PATH
# Rift Home's log on stderr even when it is redirected (Qt would send it to the journal).
export QT_FORCE_STDERR_LOGGING=1

# The desktop's own session bus, for the services Rift Home forwards onto its bus (the password store).
export RIFT_HOME_DESKTOP_BUS="${DBUS_SESSION_BUS_ADDRESS:-unix:path=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/bus}"
exec dbus-run-session -- "$BIN" "$@"
