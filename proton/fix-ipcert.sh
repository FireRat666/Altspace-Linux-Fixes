#!/usr/bin/env bash
# Lets Altspace join rooms under Proton/Wine when the room server uses an IP-address
# certificate (fixes "SSL CA certificate error" / "UnityTls error code: 7").
#
# It copies ipcert-fix/version.dll next to AltspaceVR.exe and tells the Wine
# prefix to load it for AltspaceVR.exe only. See README.md for why.
#
# Usage:
#   ./fix-ipcert.sh                                 fix every Altspace install it can find
#   ./fix-ipcert.sh <game folder> <prefix>          fix one install
#   ./fix-ipcert.sh --undo [<game folder> <prefix>] remove
# Without arguments it looks in every Wine/Proton prefix Altspace has run in, and takes the
# game folder from that prefix's Player.log (the folder Altspace last ran from).
# Run it again after a game update (the update replaces the game folder).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/lib/find-altspace.sh"
DLL="$HERE/ipcert-fix/version.dll"
UNDO=0
if [ "${1:-}" = "--undo" ]; then UNDO=1; shift; fi
if [ $# -ne 0 ] && [ $# -ne 2 ]; then
    echo "Usage: $0 [--undo] [<game folder> <prefix>]"; exit 2
fi

if pgrep -x AltspaceVR.exe >/dev/null || pgrep -x wineserver >/dev/null; then
    echo "Close Altspace (and any other Wine program) first: Wine rewrites user.reg on exit."
    exit 1
fi

# Remove our override block (the key line plus its lines up to the next blank line).
remove_key() {
    python3 - "$1" <<'EOF'
import sys
path = sys.argv[1]
lines = open(path, encoding='utf-8', errors='surrogateescape').read().split('\n')
out, skip = [], False
for line in lines:
    if line.startswith('[Software\\\\Wine\\\\AppDefaults\\\\AltspaceVR.exe\\\\DllOverrides]'):
        skip = True
        continue
    if skip and line == '':
        skip = False
        continue
    if not skip:
        out.append(line)
open(path, 'w', encoding='utf-8', errors='surrogateescape').write('\n'.join(out))
EOF
}

# fix_one <game folder> <prefix>
fix_one() {
    local game="${1%/}" prefix reg
    prefix="$(altspace_normalize_prefix "$2")"
    reg="$prefix/user.reg"
    [ -f "$game/AltspaceVR.exe" ] || { echo "No AltspaceVR.exe in '$game'. Pass the game folder."; return 1; }
    [ -n "$prefix" ] && [ -f "$reg" ] || { echo "No user.reg in '$2'. Pass the prefix folder (the one holding drive_c)."; return 1; }

    if [ $UNDO = 1 ]; then
        rm -f "$game/version.dll"
        remove_key "$reg"
        echo "Removed the IP certificate fix from $game and $prefix"
        return
    fi
    cp "$DLL" "$game/version.dll"
    [ -f "$reg.bak-ipcert" ] || cp "$reg" "$reg.bak-ipcert"
    remove_key "$reg"
    printf '\n[Software\\\\Wine\\\\AppDefaults\\\\AltspaceVR.exe\\\\DllOverrides] %s\n"version"="native,builtin"\n\n' "$(date +%s)" >> "$reg"
    echo "Installed: $game/version.dll, and a version.dll override for AltspaceVR.exe in $prefix"
}

[ $UNDO = 1 ] || [ -f "$DLL" ] || "$HERE/ipcert-fix/build.sh"

if [ $# -eq 2 ]; then
    fix_one "$1" "$2"
else
    found=0
    while IFS= read -r prefix; do
        game="$(altspace_game_dir "$prefix")"
        if [ -z "$game" ]; then
            echo "Skipping $prefix: couldn't find the game folder from its Player.log."
            continue
        fi
        fix_one "$game" "$prefix" && found=1
    done < <(altspace_prefixes)
    if [ $found = 0 ]; then
        echo "No Altspace install found. Start Altspace once (so it writes Player.log), or run:"
        echo "  $0 <game folder> <prefix>"
        exit 1
    fi
fi
[ $UNDO = 1 ] || echo "Start Altspace as usual."
