#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

PIO_CMD="${PIO_CMD:-pio}"
if ! command -v "$PIO_CMD" >/dev/null 2>&1; then
  echo "PlatformIO not found in PATH." >&2
  exit 1
fi
source "$SCRIPT_DIR/platformio_ca_bundle.sh"
export PIO_CMD SSL_CERT_FILE REQUESTS_CA_BUNDLE

# Run PlatformIO static analysis for the waveshare environment.
python3 "$SCRIPT_DIR/check_platformio_core_version.py" --pio "$PIO_CMD"
PIO_BIN_PATH="$(command -v "$PIO_CMD" 2>/dev/null || true)"
if [[ -z "$PIO_BIN_PATH" && "$PIO_CMD" == */* ]]; then
  PIO_BIN_PATH="$PIO_CMD"
fi

python_candidates=()
add_python_candidate() {
  local candidate="$1"
  local existing
  [[ -n "$candidate" ]] || return 0
  if ((${#python_candidates[@]} > 0)); then
    for existing in "${python_candidates[@]}"; do
      [[ "$existing" == "$candidate" ]] && return 0
    done
  fi
  python_candidates+=("$candidate")
}

if [[ -n "$PIO_BIN_PATH" ]]; then
  add_python_candidate "$(dirname "$PIO_BIN_PATH")/python"
  if IFS= read -r pio_shebang < "$PIO_BIN_PATH" 2>/dev/null; then
    if [[ "$pio_shebang" == '#!'*python* ]]; then
      shebang_cmd="${pio_shebang#\#!}"
      read -r shebang_first shebang_second _ <<< "$shebang_cmd"
      if [[ "$shebang_first" == "/usr/bin/env" || "$shebang_first" == "env" ]]; then
        add_python_candidate "$shebang_second"
      else
        add_python_candidate "$shebang_first"
      fi
    fi
  fi
fi
add_python_candidate "python3"

PIO_PYTHON=""
for candidate in "${python_candidates[@]}"; do
  if [[ "$candidate" == */* ]]; then
    [[ -x "$candidate" ]] || continue
  elif ! command -v "$candidate" >/dev/null 2>&1; then
    continue
  fi
  if "$candidate" -c "import platformio" >/dev/null 2>&1; then
    PIO_PYTHON="$candidate"
    break
  fi
done
if [[ -z "$PIO_PYTHON" ]]; then
  echo "Unable to find the Python interpreter that owns PlatformIO Core." >&2
  exit 1
fi
"$PIO_PYTHON" "$SCRIPT_DIR/bootstrap_platformio_check_tool.py"
"$PIO_CMD" check -e waveshare-349
