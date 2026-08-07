#!/usr/bin/env bash
#
# Build v1replay.
#
# Uses swiftc directly rather than SwiftPM so the binary can carry an embedded
# Info.plist. macOS wants NSBluetoothAlwaysUsageDescription somewhere in the
# executable before it will hand out Bluetooth access; a bare SwiftPM binary has
# nowhere to put it.
#
# Requires: Xcode command line tools (xcode-select --install). No network.

set -euo pipefail

cd "$(dirname "$0")/.."

python3 verify/check_publication_safety.py

if ! command -v xcrun >/dev/null 2>&1; then
    echo "error: xcrun not found. Run: xcode-select --install" >&2
    exit 1
fi

mkdir -p .build/module-cache

# Prefer the complete Xcode toolchain when it is installed. A macOS update can
# briefly leave the standalone Command Line Tools compiler and SDK at different
# patch builds, which makes even a one-file Swift program fail before compiling
# our sources. xcrun also supplies the matching SDK and Swift runtime paths.
SWIFT_DRIVER=(xcrun swiftc)
if [[ -d /Applications/Xcode.app/Contents/Developer ]]; then
    SWIFT_DRIVER=(env DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcrun swiftc)
fi

CLANG_MODULE_CACHE_PATH=".build/module-cache" "${SWIFT_DRIVER[@]}" -O \
    -module-cache-path ".build/module-cache" \
    -framework CoreBluetooth \
    -Xlinker -sectcreate \
    -Xlinker __TEXT \
    -Xlinker __info_plist \
    -Xlinker "Resources/Info.plist" \
    Sources/v1replay/*.swift \
    -o .build/v1replay

echo "built .build/v1replay"
echo
echo "try:  .build/v1replay demo"
