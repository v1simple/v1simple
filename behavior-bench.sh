#!/usr/bin/env bash
# Prepare host dependencies before opening or changing the DUT.
set -euo pipefail
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
"$ROOT_DIR/scripts/bench_python.sh" >/dev/null
"$ROOT_DIR/scripts/bench_python.sh" --visual >/dev/null
BENCH_PYTHON="$("$ROOT_DIR/scripts/bench_python.sh" --browser)"
unset PYTHONHOME PYTHONPATH
export PYTHONNOUSERSITE=1
exec "$BENCH_PYTHON" "$ROOT_DIR/scripts/behavior_bench.py" "$@"
