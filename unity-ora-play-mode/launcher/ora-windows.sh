#!/bin/bash
# Linux launcher for the Ora browser, used by Unity in place of ora-windows.exe
# (via OraManager.browserFolderOverride, set by the OraLinuxSetup editor script).
# Runs the Linux Electron build with the patched Ora app. When Unity runs inside the
# Unity Hub Flatpak, the browser is started on the host with flatpak-spawn.
# stdin (the TCP secret) and stdout/stderr (logs) pass straight through.
ORA_LINUX="${ORA_LINUX_HOME:-$HOME/.local/share/ora-linux}"
ELECTRON="$ORA_LINUX/electron/electron"
APP="$ORA_LINUX/app"
# The editor's own PID as Unity sees it (inside Flatpak this differs from the host PID).
# The local multiplayer hub reports it as its parent so the creator SDK recognises it.
export ORA_EDITOR_PID="$PPID"
# Frame folder Unity reads (patched SideQuest.Ora.dll): /tmp/ora-frames as Unity sees it.
mkdir -p -m 700 /tmp/ora-frames
export ORA_FRAME_DIR=/tmp/ora-frames

if [ -f /.flatpak-info ]; then
  # The sandbox's /tmp lives on the host under $XDG_RUNTIME_DIR/.flatpak/<app id>/tmp.
  ORA_FRAME_DIR="$XDG_RUNTIME_DIR/.flatpak/$FLATPAK_ID/tmp/ora-frames"
  # --watch-bus: the host browser exits when this launcher does (e.g. Unity kills it).
  exec flatpak-spawn --host --watch-bus --directory="$ORA_LINUX" \
    --env=ORA_EDITOR_PID="$ORA_EDITOR_PID" --env=ORA_FRAME_DIR="$ORA_FRAME_DIR" --env=DISPLAY="$DISPLAY" --env=WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \
    "$ELECTRON" "$APP" "$@"
fi
exec "$ELECTRON" "$APP" "$@"
