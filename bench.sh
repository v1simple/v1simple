#!/usr/bin/env bash
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

BOARD_ID="${BENCH_BOARD_ID:-release}"
ARTIFACT_ROOT="${BENCH_ARTIFACT_ROOT:-$ROOT_DIR/.artifacts/bench}"
BASELINE_ROOT="${BENCH_BASELINE_ROOT:-$ROOT_DIR/.artifacts/bench_baselines}"
DURATION_SECONDS="${BENCH_DURATION_SECONDS:-300}"
PORT="${DEVICE_PORT:-}"
RUN_CORE=0
RUN_DISPLAY=0
SELECTED=0
UPLOAD=1
SKIP_WEB=0
FROM_CSV=""
SEGMENT="last"
USE_BASELINE=1
PROMOTE_BASELINE=0

usage() {
  cat <<'EOF'
Usage: ./bench.sh [options]

Runs the bench evidence suite: core and display SD/serial windows only.
No OBD/proxy coverage and no release-qualification language. Optional promoted
baselines are local comparison aids only.

Options:
  --core                  Run only the core window.
  --display               Run only the display window.
  --duration-seconds N    Window duration (default: 300).
  --board-id ID           Board id label for artifacts (default: release).
  --artifact-root PATH    Artifact root (default: .artifacts/bench).
  --baseline-root PATH    Baseline root (default: .artifacts/bench_baselines).
  --no-baseline           Do not compare to a promoted baseline.
  --promote-baseline      After a PASS, promote this run as the future baseline.
  --port PATH             USB serial port. Defaults to auto-detect.
  --no-upload             Do not build/upload before live collection.
  --skip-web              Pass --skip-web when uploading.
  --from-csv PATH         Score an existing perf CSV instead of collecting live.
                          Requires --core or --display.
  --segment VALUE         Perf CSV segment selector for --from-csv/import (default: last).
  -h, --help              Show this help.

Exit codes:
  0 PASS, 1 WARN, 2 FAIL, 3 COLLECTION_FAILED or usage/setup failure.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --core)
      RUN_CORE=1; SELECTED=1; shift ;;
    --display)
      RUN_DISPLAY=1; SELECTED=1; shift ;;
    --duration-seconds)
      [[ $# -lt 2 ]] && { echo "Missing value for --duration-seconds" >&2; exit 3; }
      DURATION_SECONDS="$2"; shift 2 ;;
    --board-id)
      [[ $# -lt 2 ]] && { echo "Missing value for --board-id" >&2; exit 3; }
      BOARD_ID="$2"; shift 2 ;;
    --artifact-root)
      [[ $# -lt 2 ]] && { echo "Missing value for --artifact-root" >&2; exit 3; }
      ARTIFACT_ROOT="$2"; shift 2 ;;
    --baseline-root)
      [[ $# -lt 2 ]] && { echo "Missing value for --baseline-root" >&2; exit 3; }
      BASELINE_ROOT="$2"; shift 2 ;;
    --no-baseline)
      USE_BASELINE=0; shift ;;
    --promote-baseline)
      PROMOTE_BASELINE=1; shift ;;
    --port)
      [[ $# -lt 2 ]] && { echo "Missing value for --port" >&2; exit 3; }
      PORT="$2"; shift 2 ;;
    --no-upload)
      UPLOAD=0; shift ;;
    --skip-web)
      SKIP_WEB=1; shift ;;
    --from-csv)
      [[ $# -lt 2 ]] && { echo "Missing value for --from-csv" >&2; exit 3; }
      FROM_CSV="$2"; UPLOAD=0; shift 2 ;;
    --segment)
      [[ $# -lt 2 ]] && { echo "Missing value for --segment" >&2; exit 3; }
      SEGMENT="$2"; shift 2 ;;
    -h|--help)
      usage; exit 0 ;;
    *)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 3 ;;
  esac
done

if [[ "$SELECTED" -eq 0 ]]; then
  RUN_CORE=1
  RUN_DISPLAY=1
fi

if ! [[ "$DURATION_SECONDS" =~ ^[0-9]+$ ]] || [[ "$DURATION_SECONDS" -lt 1 ]]; then
  echo "Invalid --duration-seconds value '$DURATION_SECONDS'" >&2
  exit 3
fi

if [[ -n "$FROM_CSV" && "$RUN_CORE" -eq 1 && "$RUN_DISPLAY" -eq 1 ]]; then
  echo "--from-csv requires exactly one suite: pass --core or --display" >&2
  exit 3
fi

GIT_SHA="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
GIT_REF="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
TIMESTAMP="$(date -u +%Y%m%d_%H%M%S)"
RUN_DIR="$ARTIFACT_ROOT/$BOARD_ID/runs/${TIMESTAMP}_${GIT_SHA}"
if [[ -e "$RUN_DIR" ]]; then
  suffix=2
  while [[ -e "${RUN_DIR}_${suffix}" ]]; do
    suffix=$((suffix + 1))
  done
  RUN_DIR="${RUN_DIR}_${suffix}"
fi
mkdir -p "$RUN_DIR"
RUN_LOG="$RUN_DIR/bench.log"
: > "$RUN_LOG"

suites=()
[[ "$RUN_CORE" -eq 1 ]] && suites+=(core)
[[ "$RUN_DISPLAY" -eq 1 ]] && suites+=(display)

echo "==========================================" | tee -a "$RUN_LOG"
echo " bench" | tee -a "$RUN_LOG"
echo "  board:      $BOARD_ID" | tee -a "$RUN_LOG"
echo "  suites:     ${suites[*]}" | tee -a "$RUN_LOG"
echo "  duration:   ${DURATION_SECONDS}s" | tee -a "$RUN_LOG"
if [[ "$USE_BASELINE" -eq 1 ]]; then
  echo "  baseline:   $BASELINE_ROOT/$BOARD_ID (if present)" | tee -a "$RUN_LOG"
else
  echo "  baseline:   disabled" | tee -a "$RUN_LOG"
fi
[[ "$PROMOTE_BASELINE" -eq 1 ]] && echo "  promote:    on PASS" | tee -a "$RUN_LOG"
echo "  obd/proxy:  not part of bench gate" | tee -a "$RUN_LOG"
echo "  artifacts:  $RUN_DIR" | tee -a "$RUN_LOG"
echo "==========================================" | tee -a "$RUN_LOG"
echo | tee -a "$RUN_LOG"

first_live=1
for suite in "${suites[@]}"; do
  step_dir="$RUN_DIR/$suite"
  mkdir -p "$step_dir"
  args=(
    python3 "$ROOT_DIR/scripts/bench/run_window.py"
    --suite "$suite"
    --duration-seconds "$DURATION_SECONDS"
    --out-dir "$step_dir"
    --board-id "$BOARD_ID"
    --git-sha "$GIT_SHA"
    --git-ref "$GIT_REF"
    --segment "$SEGMENT"
  )
  [[ -n "$PORT" ]] && args+=(--port "$PORT")
  baseline_manifest="$BASELINE_ROOT/$BOARD_ID/$suite/manifest.json"
  if [[ "$USE_BASELINE" -eq 1 && -f "$baseline_manifest" ]]; then
    args+=(--compare-to "$baseline_manifest")
  fi
  if [[ -n "$FROM_CSV" ]]; then
    args+=(--from-csv "$FROM_CSV")
  elif [[ "$UPLOAD" -eq 1 && "$first_live" -eq 1 ]]; then
    args+=(--upload)
    [[ "$SKIP_WEB" -eq 1 ]] && args+=(--skip-web)
    first_live=0
  fi

  echo "==> bench_$suite" | tee -a "$RUN_LOG"
  status=0
  "${args[@]}" > >(tee "$step_dir/run.log" | tee -a "$RUN_LOG") 2> >(tee "$step_dir/run.err" >&2 | tee -a "$RUN_LOG" >&2) || status=$?
  echo "==> bench_$suite exit=$status" | tee -a "$RUN_LOG"
  echo | tee -a "$RUN_LOG"
done

rm -rf "$ARTIFACT_ROOT/$BOARD_ID/latest"
ln -s "runs/$(basename "$RUN_DIR")" "$ARTIFACT_ROOT/$BOARD_ID/latest"

score_args=(python3 "$ROOT_DIR/tools/bench_score.py" --run-dir "$RUN_DIR")
for suite in "${suites[@]}"; do
  score_args+=(--suite "$suite")
done
"${score_args[@]}" | tee -a "$RUN_LOG"
score_status=${PIPESTATUS[0]}

if [[ "$PROMOTE_BASELINE" -eq 1 ]]; then
  if [[ "$score_status" -eq 0 ]]; then
    for suite in "${suites[@]}"; do
      baseline_dir="$BASELINE_ROOT/$BOARD_ID/$suite"
      mkdir -p "$baseline_dir"
      cp "$RUN_DIR/$suite/manifest.json" "$baseline_dir/manifest.json"
      cp "$RUN_DIR/$suite/metrics.ndjson" "$baseline_dir/metrics.ndjson"
      cp "$RUN_DIR/$suite/scoring.json" "$baseline_dir/scoring.json"
      cp "$RUN_DIR/$suite/csv_scorecard.json" "$baseline_dir/csv_scorecard.json"
      cp "$RUN_DIR/$suite/import_diagnostics.json" "$baseline_dir/import_diagnostics.json"
      cat > "$baseline_dir/baseline_metadata.json" <<EOF
{
  "schema_version": 1,
  "promoted_from": "$RUN_DIR/$suite",
  "promoted_at_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "board_id": "$BOARD_ID",
  "suite": "$suite",
  "git_sha": "$GIT_SHA",
  "git_ref": "$GIT_REF"
}
EOF
    done
    echo "Promoted bench baseline: $BASELINE_ROOT/$BOARD_ID" | tee -a "$RUN_LOG"
  else
    echo "Baseline promotion skipped: bench result was not PASS (exit=$score_status)" | tee -a "$RUN_LOG"
  fi
fi

echo "Latest artifacts: $ARTIFACT_ROOT/$BOARD_ID/latest" | tee -a "$RUN_LOG"
exit "$score_status"
