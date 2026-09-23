#!/usr/bin/env bash
# Keep raw bench acquisition independent of the invoking terminal's Python packages.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
case "${1:-}" in
  '')
    [[ $# -eq 0 ]] || exit 2
    REQUIREMENTS="$ROOT_DIR/scripts/requirements-bench.txt"
    ;;
  --visual)
    [[ $# -eq 1 ]] || exit 2
    REQUIREMENTS="$ROOT_DIR/scripts/requirements-bench-visual.txt"
    ;;
  *) exit 2 ;;
esac
BENCH_ENV="$ROOT_DIR/.artifacts/bench-runtime/python"
BENCH_PYTHON="$BENCH_ENV/bin/python3"
unset PYTHONHOME PYTHONPATH
export PYTHONNOUSERSITE=1

dependencies_match() {
  "$BENCH_PYTHON" -I - "$REQUIREMENTS" <<'PY'
import importlib.metadata
import sys
try:
    for line in open(sys.argv[1], encoding="utf-8"):
        line = line.strip()
        if line and not line.startswith("#"):
            name, version = line.split("==")
            if importlib.metadata.version(name) != version:
                raise ValueError(name)
except (ImportError, ValueError, importlib.metadata.PackageNotFoundError):
    sys.exit(1)
PY
}

if [[ ! -x "$BENCH_PYTHON" ]]; then
  printf '[bench] preparing the local Python environment...\n' >&2
  python3 -I -m venv "$BENCH_ENV" >&2
fi
if ! dependencies_match; then
  printf '[bench] installing the fixed bench dependencies...\n' >&2
  "$BENCH_PYTHON" -I -m pip --isolated install --disable-pip-version-check \
    --no-input --only-binary=:all: --retries 1 --timeout 30 -r "$REQUIREMENTS" >&2
  dependencies_match
fi
printf '%s\n' "$BENCH_PYTHON"
