#!/bin/bash
# Compress all web assets with gzip after build

set -euo pipefail

echo "Compressing web assets..."

# Run from interface directory
cd "$(dirname "$0")/../interface" || exit 1

BUILD_DIR="build"
if [[ ! -d "$BUILD_DIR" ]]; then
    echo "Build directory not found: $BUILD_DIR" >&2
    exit 1
fi

# Keep HTML entry pages uncompressed so clients without gzip support can load
# them without also storing a compressed twin for every route. LittleFS charges
# each tiny twin at least one allocation block; gzip the larger runtime assets.
find "$BUILD_DIR" -type f \( -name "*.js" -o -name "*.css" -o -name "*.json" \) -print0 | while IFS= read -r -d '' file; do
    echo "  Compressing: $file"
    gzip -9 -k -f "$file"  # -9 = best compression, -k = keep original, -f = refresh stale .gz
done

echo "Web assets compressed."
echo "Runtime originals + .gz files are both in $BUILD_DIR; HTML remains uncompressed"
