#!/usr/bin/env bash
# Test-only: build coop.dll (localhost-only co-op prototype for the Foundation test copy) with the portable Zig.
set -e
cd "$(dirname "$0")"
Z=/e/FoundationCoopTest/tools/zig-x86_64-windows-0.17.1/zig.exe
mkdir -p build
"$Z" cc -target x86_64-windows-gnu -O2 -shared -o build/coop.dll coop.c -lws2_32 -lbcrypt
ls -la build/coop.dll
