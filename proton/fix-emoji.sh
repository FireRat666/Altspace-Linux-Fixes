#!/usr/bin/env bash
# Makes emoji show in Altspace running under Proton or Wine.
#
# On Windows, Unity draws emoji with the system font "Segoe UI Emoji" from
# C:\Windows\Fonts. Proton/Wine prefixes don't have that font, so every emoji
# shows as a box. This copies a colour emoji font (Twemoji, which Firefox ships)
# into the prefix under that name.
#
# Usage:
#   ./fix-emoji.sh                  fix every prefix Altspace has run in (any launcher)
#   ./fix-emoji.sh /path/to/prefix  fix one prefix (the folder holding drive_c)
#   ./fix-emoji.sh --undo [prefix]  remove the font again
#
# Safe to run again after a game update; the font lives in the prefix, not the game.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/lib/find-altspace.sh"
CACHE="$HERE/seguiemj-v2.ttf"
FONT_NAME="seguiemj.ttf"

find_source_font() {
    for f in "${EMOJI_FONT:-}" \
             /usr/lib/firefox/fonts/TwemojiMozilla.ttf \
             /usr/lib64/firefox/fonts/TwemojiMozilla.ttf \
             /usr/lib/librewolf/fonts/TwemojiMozilla.ttf \
             /usr/lib/firefox-esr/fonts/TwemojiMozilla.ttf \
             /opt/firefox/fonts/TwemojiMozilla.ttf \
             /snap/firefox/current/usr/lib/firefox/fonts/TwemojiMozilla.ttf \
             /var/lib/flatpak/app/org.mozilla.firefox/current/active/files/lib/firefox/fonts/TwemojiMozilla.ttf \
             "$HOME"/.local/share/flatpak/app/org.mozilla.firefox/current/active/files/lib/firefox/fonts/TwemojiMozilla.ttf \
             /usr/share/fonts/*/Twemoji*.ttf \
             "$HOME"/.local/share/fonts/Twemoji*.ttf; do
        [ -n "$f" ] && [ -f "$f" ] && { echo "$f"; return; }
    done
    return 1
}

build_font() {
    [ -f "$CACHE" ] && return
    local src
    if ! src="$(find_source_font)"; then
        echo "No Twemoji font found. Install Firefox, or download TwemojiMozilla.ttf from"
        echo "https://github.com/mozilla/twemoji-colr/releases and run:"
        echo "  EMOJI_FONT=/path/to/TwemojiMozilla.ttf $0"
        exit 1
    fi
    echo "Building Segoe UI Emoji from $src"
    python3 - "$src" "$CACHE" <<'EOF'
import sys
try:
    from fontTools.ttLib import TTFont
except ImportError:
    sys.exit("Needs fontTools: install python-fonttools (or: pip install --user fonttools)")
font = TTFont(sys.argv[1])
if 'COLR' not in font:
    sys.exit("That font has no COLR colour table; use a Twemoji (COLR) font.")
# Twemoji's main glyphs are empty (the picture is only in the COLR colour layers).
# Segoe UI Emoji has a full outline there, and Unity sizes emoji from it, so an
# empty one draws tiny. Make each main glyph a composite of its layers.
from fontTools.ttLib.tables._g_l_y_f import Glyph, GlyphComponent
glyf = font['glyf']
for base, layers in font['COLR'].ColorLayers.items():
    g = glyf[base]
    if g.numberOfContours != 0 or not layers:
        continue
    comp = Glyph()
    comp.numberOfContours = -1
    comp.components = []
    for layer in layers:
        c = GlyphComponent()
        c.glyphName, c.x, c.y, c.flags = layer.name, 0, 0, 0
        comp.components.append(c)
    comp.components[0].flags = 0x4   # ROUND_XY_TO_GRID; first one keeps metrics
    glyf[base] = comp
    comp.recalcBounds(glyf)
font['maxp'].recalc(font) if hasattr(font['maxp'], 'recalc') else None
names = {1: 'Segoe UI Emoji', 2: 'Regular', 4: 'Segoe UI Emoji', 6: 'SegoeUIEmoji',
         16: 'Segoe UI Emoji', 17: 'Regular'}
table = font['name']
table.names = [r for r in table.names if r.nameID not in names]
for nid, text in names.items():
    table.setName(text, nid, 3, 1, 0x409)   # Windows
    table.setName(text, nid, 1, 0, 0)       # Mac
table.setName('Twemoji for Unity v2', 5, 3, 1, 0x409)   # version, marks our own font
font.save(sys.argv[2])
EOF
}

prefixes() {
    if [ $# -gt 0 ]; then printf '%s\n' "$@"; return; fi
    altspace_prefixes
}

UNDO=0
if [ "${1:-}" = "--undo" ]; then UNDO=1; shift; fi

[ $UNDO = 1 ] || build_font

found=0
while IFS= read -r p; do
    [ -z "$p" ] && continue
    [ -d "$p/pfx/drive_c" ] && p="$(readlink -f "$p/pfx")"
    fonts="$p/drive_c/windows/Fonts"
    if [ ! -d "$fonts" ]; then echo "Skipping $p (no drive_c/windows/Fonts)"; continue; fi
    found=1
    if [ $UNDO = 1 ]; then
        rm -f "$fonts/$FONT_NAME" && echo "Removed emoji font from $p"
    elif [ -f "$fonts/$FONT_NAME" ] && ! cmp -s "$CACHE" "$fonts/$FONT_NAME" \
         && ! grep -a -q "Twemoji" "$fonts/$FONT_NAME"; then
        echo "Skipping $p: it already has a different $FONT_NAME (a real Segoe UI Emoji?)"
    else
        rm -f "$fonts/$FONT_NAME"; cp "$CACHE" "$fonts/$FONT_NAME" && echo "Installed emoji font in $p"
    fi
done < <(prefixes "$@")

[ $found = 1 ] || { echo "No Altspace prefix found (start Altspace once first), or pass the prefix folder (the one holding drive_c)."; exit 1; }
[ $UNDO = 1 ] || echo "Done. Restart the game for emoji to show."
