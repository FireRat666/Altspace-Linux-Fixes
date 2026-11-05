# Finds Altspace installs under Wine/Proton. Sourced by fix-emoji.sh and fix-ipcert.sh.
#
# A prefix counts as an Altspace prefix once the game has run in it: Unity then creates
# drive_c/users/<user>/AppData/LocalLow/SideQuest/AltspaceVR (with Player.log). That works
# the same for every launcher (Steam, Faugus, Lutris, Heroic, Bottles, umu, plain Wine).

# Folders that hold Wine prefixes for the common launchers. Missing ones are skipped.
altspace_search_roots() {
    printf '%s\n' \
        "${WINEPREFIX:-}" \
        "$HOME/.wine" \
        "$HOME/.local/share/wineprefixes" \
        "$HOME/Faugus" \
        "$HOME/Games" \
        "$HOME/.local/share/bottles/bottles" \
        "$HOME/.var/app/com.usebottles.bottles/data/bottles/bottles" \
        "$HOME/.local/share/Steam/steamapps/compatdata" \
        "$HOME/.steam/steam/steamapps/compatdata" \
        "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/compatdata"
}

# Prints the prefix folder (the one holding drive_c) for a path, or nothing.
altspace_normalize_prefix() {
    local p="$1"
    [ -d "$p/pfx/drive_c" ] && p="$p/pfx"
    [ -d "$p/drive_c" ] && readlink -f "$p"
}

# Prints each prefix (one per line) that Altspace has run in.
altspace_prefixes() {
    local root drive_c prefix
    while IFS= read -r root; do
        [ -n "$root" ] && [ -d "$root" ] || continue
        while IFS= read -r drive_c; do
            prefix="$(dirname "$drive_c")"
            compgen -G "$prefix/drive_c/users/*/AppData/LocalLow/SideQuest/AltspaceVR" >/dev/null &&
                readlink -f "$prefix"
        # Don't follow links: every prefix has dosdevices/z: -> /.
        done < <(find "$root/" -maxdepth 5 -name dosdevices -prune -o -type d -name drive_c -prune -print 2>/dev/null)
    done < <(altspace_search_roots) | sort -u
}

# Prints the Linux path of the game folder Altspace last ran from in a prefix, or nothing.
# Unity's Player.log has a line like
#   [Subsystems] Discovering subsystems at path X:/Games/Altspace/AltspaceVR_Data/UnitySubsystems
# and the prefix's dosdevices/x: link says where that drive points.
altspace_game_dir() {
    local prefix="$1" log win drive rest root dir
    log="$(ls -t "$prefix"/drive_c/users/*/AppData/LocalLow/SideQuest/AltspaceVR/Player.log 2>/dev/null | head -1)"
    [ -n "$log" ] || return 0
    win="$(grep -a 'Discovering subsystems at path ' "$log" | tail -1 |
           sed -e 's/.*Discovering subsystems at path //' -e 's/[[:space:]]*$//' -e 's#\\#/#g' -e 's#/AltspaceVR_Data/UnitySubsystems$##')"
    case "$win" in [A-Za-z]:/*) ;; *) return 0 ;; esac
    drive="$(printf '%s' "${win:0:1}" | tr 'A-Z' 'a-z')"
    rest="${win:2}"
    root="$(readlink -f "$prefix/dosdevices/$drive:" 2>/dev/null)" || return 0
    dir="${root%/}$rest"
    [ -f "$dir/AltspaceVR.exe" ] && printf '%s\n' "$dir"
    return 0
}
