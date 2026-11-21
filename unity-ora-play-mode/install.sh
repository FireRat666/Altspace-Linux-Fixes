#!/usr/bin/env bash
# Sets up the Linux browser for com.sidequest.ora and installs the editor script into a Unity project.
#
#   ./install.sh <unity project> [--no-project-script]
#
# Installs into ${ORA_LINUX_HOME:-~/.local/share/ora-linux}:
#   electron/  official Electron build (version matched to the Ora package), checksum-verified
#   app/       Ora's own Electron app from the package, with our Linux patches applied
#   bin/       the launcher Unity runs in place of ora-windows.exe
#   patcher/   the SideQuest.Ora.dll patcher (run by the editor script)
# and copies unity/OraLinuxSetup.cs to <project>/Assets/Editor/OraLinux/. For Ora 0.2 and later it also builds
# native/ (the Unity <-> browser link): ora-link.node goes into app/dist, libora-audio.so into
# <project>/Assets/Editor/OraLinux/Plugins.
#
# Needs: curl, unzip, sha256sum, node/npx (for @electron/asar), a C compiler and make (Ora 0.2), the Node-API
# headers (/usr/include/node, or node's own), and a Unity editor under ~/Unity/Hub/Editor
# (or UNITY_EDITOR=<.../Editor> pointing at one) for its bundled Mono and Cecil.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${ORA_LINUX_HOME:-$HOME/.local/share/ora-linux}"
PROJECT=""
PROJECT_SCRIPT=1
for arg in "$@"; do
  case "$arg" in
    --no-project-script) PROJECT_SCRIPT=0 ;;
    -h|--help) sed -n '2,15p' "$0"; exit 0 ;;
    *) PROJECT="$arg" ;;
  esac
done
[ -n "$PROJECT" ] || { echo "usage: $0 <unity project> [--no-project-script]" >&2; exit 2; }
PROJECT="$(cd "$PROJECT" && pwd)"
step() { printf '\n==> %s\n' "$*"; }

# --- Find the Ora package -----------------------------------------------------------------------
step "Looking for com.sidequest.ora in $PROJECT"
ORA=""
for d in "$PROJECT/Packages/com.sidequest.ora" "$PROJECT"/Library/PackageCache/com.sidequest.ora@*; do
  [ -f "$d/Plugins/Windows~/resources/app.asar" ] && { ORA="$d"; break; }
done
[ -n "$ORA" ] || { echo "com.sidequest.ora not found (open the project in Unity once so packages resolve)" >&2; exit 1; }
WINDOWS_DIR="$ORA/Plugins/Windows~"
echo "found $ORA"

# --- Electron (same version Ora ships for Windows) ----------------------------------------------
ELECTRON_VERSION="$(grep -aom1 'Electron/[0-9.]*' "$WINDOWS_DIR/ora-windows.exe" | head -1 | cut -d/ -f2)"
[ -n "$ELECTRON_VERSION" ] || { echo "couldn't read the Electron version from ora-windows.exe" >&2; exit 1; }
step "Electron $ELECTRON_VERSION"
if [ "$(cat "$DEST/electron/version" 2>/dev/null)" = "$ELECTRON_VERSION" ]; then
  echo "already installed"
else
  zip="electron-v$ELECTRON_VERSION-linux-x64.zip"
  base="https://github.com/electron/electron/releases/download/v$ELECTRON_VERSION"
  dl="$(mktemp -d)"
  curl -fL --progress-bar -o "$dl/$zip" "$base/$zip"
  curl -fsSL -o "$dl/SHASUMS256.txt" "$base/SHASUMS256.txt"
  (cd "$dl" && grep " \*$zip\$" SHASUMS256.txt | sed 's/ \*/  /' | sha256sum -c -)
  rm -rf "$DEST/electron" && mkdir -p "$DEST/electron"
  unzip -q "$dl/$zip" -d "$DEST/electron"
  rm -rf "$dl"
fi

# --- Ora's app, patched ---------------------------------------------------------------------------
step "Ora app from $(basename "$ORA")"
rm -rf "$DEST/app"
npx --yes @electron/asar extract "$WINDOWS_DIR/resources/app.asar" "$DEST/app"
node "$REPO/app/apply-patches.js" "$DEST/app"

# --- Ora 0.2 link (Linux build of ora-link.node and ora-audio) -------------------------------------
LINK=0
if [ -f "$DEST/app/dist/link.js" ]; then
  LINK=1
  step "Browser link (Ora 0.2)"
  make -s -C "$REPO/native"
  cp "$REPO/native/build/ora-link.node" "$DEST/app/dist/ora-link.node"
  echo "$DEST/app/dist/ora-link.node"
fi

# --- Launcher -----------------------------------------------------------------------------------
step "Launcher"
mkdir -p "$DEST/bin"
launcher="$DEST/bin/ora-windows.exe"
cp "$REPO/launcher/ora-windows.sh" "$launcher"
# Ora refuses a "browser" under 1 MB (it takes it for a Git LFS pointer): pad with comments after the exec lines.
{ printf '\n# Padding below: Ora rejects launchers smaller than 1 MB.\n'
  head -c 1200000 /dev/zero | tr '\0' '#' | fold -w 100 | sed 's/^/#/'; } >> "$launcher"
chmod +x "$launcher"
echo "$launcher"

# --- DLL patcher ----------------------------------------------------------------------------------
step "SideQuest.Ora.dll patcher"
EDITOR="${UNITY_EDITOR:-}"
if [ -z "$EDITOR" ]; then
  version="$(sed -n 's/^m_EditorVersion: //p' "$PROJECT/ProjectSettings/ProjectVersion.txt" 2>/dev/null || true)"
  if [ -n "$version" ] && [ -d "$HOME/Unity/Hub/Editor/$version/Editor" ]; then
    EDITOR="$HOME/Unity/Hub/Editor/$version/Editor"
  else
    EDITOR="$(ls -d "$HOME"/Unity/Hub/Editor/*/Editor 2>/dev/null | sort -V | tail -1)"
  fi
fi
MONO="$EDITOR/Data/MonoBleedingEdge"
CECIL="$EDITOR/Data/Managed/Unity.Cecil.dll"
[ -x "$MONO/bin/mono" ] && [ -f "$CECIL" ] || { echo "no Unity editor found; set UNITY_EDITOR=<path to .../Editor>" >&2; exit 1; }
mkdir -p "$DEST/patcher"
cp "$CECIL" "$DEST/patcher/"
cp "$REPO/patcher/OraLinuxPatcher.cs" "$DEST/patcher/"
MONO_PATH="$MONO/lib/mono/4.5" "$MONO/bin/mono" "$MONO/lib/mono/4.5/csc.exe" -nologo \
  -out:"$DEST/patcher/OraLinuxPatcher.exe" -r:"$DEST/patcher/Unity.Cecil.dll" -r:System.Core.dll \
  "$DEST/patcher/OraLinuxPatcher.cs"
echo "built with $EDITOR"

cp "$REPO/README.md" "$DEST/README.md"

# --- Editor script --------------------------------------------------------------------------------
if [ "$PROJECT_SCRIPT" = 1 ]; then
  step "Editor script"
  mkdir -p "$PROJECT/Assets/Editor/OraLinux"
  cp "$REPO/unity/OraLinuxSetup.cs" "$PROJECT/Assets/Editor/OraLinux/OraLinuxSetup.cs"
  echo "$PROJECT/Assets/Editor/OraLinux/OraLinuxSetup.cs"
  if [ "$LINK" = 1 ]; then
    # Unity's half of the link, as the "ora-audio" plugin, for the Linux editor only.
    mkdir -p "$PROJECT/Assets/Editor/OraLinux/Plugins"
    cp "$REPO/native/build/libora-audio.so" "$PROJECT/Assets/Editor/OraLinux/Plugins/libora-audio.so"
    cp "$REPO/unity/libora-audio.so.meta" "$PROJECT/Assets/Editor/OraLinux/Plugins/libora-audio.so.meta"
    echo "$PROJECT/Assets/Editor/OraLinux/Plugins/libora-audio.so"
  fi
fi

step "Done"
echo "Focus Unity so it recompiles; the editor script patches SideQuest.Ora.dll, then enter Play mode."
