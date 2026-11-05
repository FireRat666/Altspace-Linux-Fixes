#!/usr/bin/env bash
# Builds version.dll with clang + lld (no Windows SDK or mingw gcc needed).
set -euo pipefail
cd "$(dirname "$0")"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
printf 'LIBRARY kernel32.dll\nEXPORTS\nGetModuleHandleW\nLoadLibraryW\nGetProcAddress\nVirtualProtect\nGetSystemDirectoryW\nOutputDebugStringA\n' > "$tmp/kernel32.def"
llvm-dlltool -m i386:x86-64 -d "$tmp/kernel32.def" -l "$tmp/kernel32.lib"
clang --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fno-stack-protector \
      -fno-asynchronous-unwind-tables -c version.c -o "$tmp/version.o"
lld-link /dll /nodefaultlib /entry:DllMain /def:version.def /out:version.dll \
         "$tmp/version.o" "$tmp/kernel32.lib"
rm -f version.lib
echo "Built $(pwd)/version.dll"
