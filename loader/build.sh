#!/usr/bin/env bash
# Build the loader (proxy dwmapi.dll) and the launcher (FoundationCoop.exe) with a portable Zig toolchain.
# Set ZIG to your zig.exe. Regenerate the dwmapi stubs only if Windows changes the exports:
#   python gen_proxy.py pe_exports.py .
set -e
cd "$(dirname "$0")"
Z=${ZIG:-/e/FoundationCoopTest/tools/zig-x86_64-windows-0.17.1/zig.exe}
mkdir -p build
"$Z" cc -target x86_64-windows-gnu -O2 -shared -o build/dwmapi.dll dwmapi_proxy.c dwmapi_fwd.S dwmapi.def loader.rc -lshell32 -lole32
"$Z" cc -target x86_64-windows-gnu -O2 -municode -Wl,--subsystem,windows -o build/FoundationCoop.exe launcher.c launcher.rc \
    -lws2_32 -liphlpapi -lcomdlg32 -lshell32 -lole32 -lbcrypt -luser32 -lgdi32 -ladvapi32
ls -la build/dwmapi.dll build/FoundationCoop.exe
