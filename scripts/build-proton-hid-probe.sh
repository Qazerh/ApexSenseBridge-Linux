#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
probe_build="$repo_dir/build-hid-probe"
wine_include="${WINE_INCLUDE_DIR:-/usr/include/wine/windows}"
wine_lib="${WINE_PE_LIB_DIR:-/usr/lib/wine/x86_64-windows}"
mkdir -p "$probe_build" "$repo_dir/bin"
# Build a small PE executable with native GCC's Microsoft ABI and Wine's import
# libraries. No cross compiler, CRT implementation, or GUI is needed.
gcc -std=c11 -Wall -Wextra -I"$wine_include" -D_WIN64 -fshort-wchar -mabi=ms \
    -fno-ident -fno-asynchronous-unwind-tables -fno-stack-protector \
    -c "$repo_dir/src/tools/ProtonHidProbe.c" -o "$probe_build/probe.o"
objcopy --remove-section .note.GNU-stack --remove-section .note.gnu.property \
    "$probe_build/probe.o" "$probe_build/probe-link.o"
ld -mi386pep --entry probe_entry --subsystem console --image-base 0x140000000 \
    -o "$repo_dir/bin/ProtonHidProbe.exe" "$probe_build/probe-link.o" \
    "$wine_lib/libsetupapi.a" "$wine_lib/libhid.a" "$wine_lib/libkernel32.a"
