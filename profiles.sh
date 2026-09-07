#!/usr/bin/env bash
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROFILE_PYTHON="${PROFILE_PYTHON:-$ROOT_DIR/.artifacts/bench/python/bin/python}"
if [[ ! -x "$PROFILE_PYTHON" ]]; then
  PROFILE_PYTHON=python3
fi
exec "$PROFILE_PYTHON" "$ROOT_DIR/scripts/usb_profiles.py" "$@"
