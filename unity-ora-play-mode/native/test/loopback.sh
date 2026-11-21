#!/usr/bin/env bash
# Loopback test of the Linux link: Python stands in for Unity (libora-audio.so through ctypes, the calls
# SideQuest.Ora.dll makes), Node for Electron (ora-link.node, the calls dist/link.js makes).
#   make && test/loopback.sh
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="$HERE/../build"
export ORA_LINK_DIR="$(mktemp -d)"
trap 'rm -rf "$ORA_LINK_DIR"' EXIT
python3 -I "$HERE/unity_side.py" "$BUILD/libora-audio.so" "$BUILD/ora-link.node" "$HERE/host_side.js"
