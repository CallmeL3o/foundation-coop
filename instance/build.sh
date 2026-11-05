#!/usr/bin/env bash
# Build launch.exe (starts the game with folderhook.dll + coop.dll injected) and folderhook.dll (per-instance
# Documents/LocalAppData redirect) with a portable Zig toolchain. Set ZIG to your zig.exe.
set -e
cd "$(dirname "$0")"
Z=${ZIG:-/e/FoundationCoopTest/tools/zig-x86_64-windows-0.17.1/zig.exe}
mkdir -p build
"$Z" cc -target x86_64-windows-gnu -O2 -municode -o build/launch.exe launch.c
"$Z" cc -target x86_64-windows-gnu -O2 -shared -o build/folderhook.dll folderhook.c -lshell32 -lole32
ls -la build
