#!/usr/bin/env bash
# Keep the external reader independent of the invoking terminal's Python packages.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REQUIREMENTS="$ROOT_DIR/scripts/requirements-bench.txt"
BENCH_ENV="$ROOT_DIR/.artifacts/bench/python"
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

# Reject dependency drift before spending a recording on an unusable reader.
# Full reader, OCR, camera and evidence verification still runs in the analyzer.
if [[ $# -gt 0 ]]; then
  "$BENCH_PYTHON" -I - "$1" <<'PY'
import json
import sys
import numpy
import PIL
try:
    with open(sys.argv[1], encoding="utf-8") as stream:
        qualified = json.load(stream)["reader"]["runtime"]
    actual = {"numpy_version": numpy.__version__, "pillow_version": PIL.__version__}
    differences = [f"{key}: qualified {qualified.get(key)!r}, running {value!r}"
                   for key, value in actual.items() if qualified.get(key) != value]
    if differences:
        raise ValueError("; ".join(differences))
except (OSError, ValueError, KeyError, TypeError) as error:
    print(f"[bench] reader environment is not qualified: {error}", file=sys.stderr)
    sys.exit(2)
PY
fi
printf '%s\n' "$BENCH_PYTHON"
