#!/usr/bin/env bash
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

BOARD_ID="${BENCH_BOARD_ID:-release}"
ARTIFACT_ROOT="${BENCH_ARTIFACT_ROOT:-$ROOT_DIR/.artifacts/bench}"
BASELINE_ROOT="${BENCH_BASELINE_ROOT:-$ROOT_DIR/.artifacts/bench_baselines}"
DURATION_SECONDS="${BENCH_DURATION_SECONDS:-300}"
REPLAY_DURATION_SECONDS="${BENCH_REPLAY_DURATION_SECONDS:-300}"
POST_UPLOAD_SETTLE_SECONDS="${BENCH_POST_UPLOAD_SETTLE_SECONDS:-90}"
PORT="${DEVICE_PORT:-}"
RUN_CORE=0
RUN_DISPLAY=0
RUN_REPLAY=0
SELECTED=0
UPLOAD=1
SKIP_WEB=0
CAPTURE_CAMERA=0
HOSTED_INVESTIGATOR=0
FROM_CSV=""
SEGMENT="last"
USE_BASELINE=1
PROMOTE_BASELINE=0
COMPARE_TO=()
BLINK_PROFILE="scenario"
BLINK_PROFILE_SET=0
SCENARIO=""
LEGACY_BLINK_ARROW=0
WRAPPER_SHUTDOWN_GRACE_SECONDS=75

usage() {
  cat <<'EOF'
Usage: ./bench.sh [options]

Runs the bench evidence suite in one artifact directory and emits one verdict.
No OBD/proxy coverage and no release-qualification language. Core camera video
is diagnostic evidence, display camera video is exercise evidence, and neither
can affect the verdict. Only deterministic replay is mechanically graded
against its independently recorded same-window display log.
Optional promoted baselines are local comparison aids only.
The managed v1replay peripheral emulates the V1 for every live suite.

Options:
  --all                   Run core, display, and deterministic V1 replay.
  --core                  Run only the core window.
  --display               Run only the display window.
  --replay                Run only the deterministic V1 replay window.
  --blink-profile PROFILE Priority-arrow stimulus for replay: scenario (default),
                          steady (negative control), or stress (always blink).
  --blink-arrow           Legacy alias for --blink-profile stress.
  --scenario PATH         Pass an external replay scenario through to v1replay.
  --camera                Capture every live window; gate only replay camera evidence.
  --hosted-investigator   Investigate with hosted gpt-5.6-sol after scoring.
                          This explicitly sends investigation inputs and prepared
                          camera sheets to the hosted model; the bench verdict remains unchanged.
  --duration-seconds N    Window duration (default: 300).
  --replay-duration-seconds N
                          Replay metrics window duration (default: 300).
  --post-upload-settle-seconds N
                          Unscored SD settle time before the first post-upload window (default: 90).
  --board-id ID           Board id label for artifacts (default: release).
  --artifact-root PATH    Artifact root (default: .artifacts/bench).
  --baseline-root PATH    Baseline root (default: .artifacts/bench_baselines).
  --compare-to PATH       Explicit baseline manifest for regression comparison.
                          Repeat for a multi-run baseline window.
  --no-baseline           Do not compare to a promoted baseline.
  --promote-baseline      After a PASS, promote core/display as baselines for
                          future matching runs. Does not compare the current run;
                          replay baselines are not promoted.
  --port PATH             USB serial port. Defaults to auto-detect.
  --no-upload             Do not build/upload before live collection.
  --skip-web              Pass --skip-web when uploading.
  --from-csv PATH         Score an existing perf CSV instead of collecting live.
                          Requires --core or --display.
  --segment VALUE         Perf CSV segment selector for --from-csv/import (default: last).
  -h, --help              Show this help.

Exit codes:
  0 PASS, 1 WARN, 2 FAIL, 3 EVIDENCE_FAILED, COLLECTION_FAILED, or usage/setup failure.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --all)
      RUN_CORE=1; RUN_DISPLAY=1; RUN_REPLAY=1; SELECTED=1; shift ;;
    --core)
      RUN_CORE=1; SELECTED=1; shift ;;
    --display)
      RUN_DISPLAY=1; SELECTED=1; shift ;;
    --replay)
      RUN_REPLAY=1; SELECTED=1; shift ;;
    --blink-profile)
      [[ $# -lt 2 ]] && { echo "Missing value for --blink-profile" >&2; exit 3; }
      BLINK_PROFILE="$2"; BLINK_PROFILE_SET=1; shift 2 ;;
    --blink-arrow)
      LEGACY_BLINK_ARROW=1; shift ;;
    --scenario)
      [[ $# -lt 2 ]] && { echo "Missing value for --scenario" >&2; exit 3; }
      SCENARIO="$2"; shift 2 ;;
    --camera)
      CAPTURE_CAMERA=1; shift ;;
    --hosted-investigator)
      HOSTED_INVESTIGATOR=1; shift ;;
    --duration-seconds)
      [[ $# -lt 2 ]] && { echo "Missing value for --duration-seconds" >&2; exit 3; }
      DURATION_SECONDS="$2"; shift 2 ;;
    --replay-duration-seconds)
      [[ $# -lt 2 ]] && { echo "Missing value for --replay-duration-seconds" >&2; exit 3; }
      REPLAY_DURATION_SECONDS="$2"; shift 2 ;;
    --post-upload-settle-seconds)
      [[ $# -lt 2 ]] && { echo "Missing value for --post-upload-settle-seconds" >&2; exit 3; }
      POST_UPLOAD_SETTLE_SECONDS="$2"; shift 2 ;;
    --board-id)
      [[ $# -lt 2 ]] && { echo "Missing value for --board-id" >&2; exit 3; }
      BOARD_ID="$2"; shift 2 ;;
    --artifact-root)
      [[ $# -lt 2 ]] && { echo "Missing value for --artifact-root" >&2; exit 3; }
      ARTIFACT_ROOT="$2"; shift 2 ;;
    --baseline-root)
      [[ $# -lt 2 ]] && { echo "Missing value for --baseline-root" >&2; exit 3; }
      BASELINE_ROOT="$2"; shift 2 ;;
    --compare-to)
      [[ $# -lt 2 ]] && { echo "Missing value for --compare-to" >&2; exit 3; }
      COMPARE_TO+=("$2"); shift 2 ;;
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

if [[ "$LEGACY_BLINK_ARROW" -eq 1 ]]; then
  if [[ "$BLINK_PROFILE_SET" -eq 1 ]]; then
    echo "Use either --blink-profile or --blink-arrow, not both" >&2
    exit 3
  fi
  BLINK_PROFILE="stress"
  BLINK_PROFILE_SET=1
fi

case "$BLINK_PROFILE" in
  scenario|steady|stress) ;;
  *)
    echo "Invalid --blink-profile value '$BLINK_PROFILE' (use scenario, steady, or stress)" >&2
    exit 3 ;;
esac

if [[ "$BLINK_PROFILE_SET" -eq 1 && "$RUN_REPLAY" -ne 1 ]]; then
  echo "--blink-profile requires a selected replay suite" >&2
  exit 3
fi
if [[ -n "$SCENARIO" && "$RUN_REPLAY" -ne 1 ]]; then
  echo "--scenario requires a selected replay suite" >&2
  exit 3
fi

if [[ "$USE_BASELINE" -eq 0 && "${#COMPARE_TO[@]}" -gt 0 ]]; then
  echo "Use either --no-baseline or --compare-to, not both" >&2
  exit 3
fi

if [[ "$PROMOTE_BASELINE" -eq 1 && "$RUN_CORE" -ne 1 && "$RUN_DISPLAY" -ne 1 ]]; then
  echo "--promote-baseline requires core and/or display; replay baselines are not promoted" >&2
  exit 3
fi

if ! [[ "$DURATION_SECONDS" =~ ^[0-9]+$ ]] || [[ "$DURATION_SECONDS" -lt 1 ]]; then
  echo "Invalid --duration-seconds value '$DURATION_SECONDS'" >&2
  exit 3
fi
if ! [[ "$REPLAY_DURATION_SECONDS" =~ ^[0-9]+$ ]] || [[ "$REPLAY_DURATION_SECONDS" -lt 1 ]]; then
  echo "Invalid --replay-duration-seconds value '$REPLAY_DURATION_SECONDS'" >&2
  exit 3
fi
if ! [[ "$POST_UPLOAD_SETTLE_SECONDS" =~ ^[0-9]+$ ]]; then
  echo "Invalid --post-upload-settle-seconds value '$POST_UPLOAD_SETTLE_SECONDS'" >&2
  exit 3
fi

if [[ -n "$FROM_CSV" ]]; then
  selected_count=$((RUN_CORE + RUN_DISPLAY + RUN_REPLAY))
  if [[ "$selected_count" -ne 1 || "$RUN_REPLAY" -eq 1 ]]; then
    echo "--from-csv requires exactly one suite: pass --core or --display" >&2
    exit 3
  fi
  if [[ "$CAPTURE_CAMERA" -eq 1 ]]; then
    echo "--camera cannot be used with --from-csv" >&2
    exit 3
  fi
fi

SAFE_BOARD_ID="$(PYTHONPATH="$ROOT_DIR/scripts/bench" python3 -c \
  'import sys; from artifact_privacy import privacy_safe_identifier; print(privacy_safe_identifier(sys.argv[1], namespace="board"))' \
  "$BOARD_ID")" || {
  echo "Unable to create the private-safe board identity" >&2
  exit 3
}

GIT_SHA="$(git rev-parse HEAD 2>/dev/null || echo unknown)"
GIT_SHA_SHORT="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"
GIT_REF="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
GIT_WORKTREE_CLEAN=1
[[ -n "$(git status --porcelain 2>/dev/null)" ]] && GIT_WORKTREE_CLEAN=0
TIMESTAMP="$(date -u +%Y%m%d_%H%M%S)"
RUN_DIR="$ARTIFACT_ROOT/$SAFE_BOARD_ID/runs/${TIMESTAMP}_${GIT_SHA_SHORT}"
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
[[ "$RUN_REPLAY" -eq 1 ]] && suites+=(replay)
V1REPLAY_EXECUTABLE="$ROOT_DIR/tools/v1replay/.build/v1replay"

echo "==========================================" | tee -a "$RUN_LOG"
echo " bench" | tee -a "$RUN_LOG"
echo "  board:      $SAFE_BOARD_ID" | tee -a "$RUN_LOG"
echo "  suites:     ${suites[*]}" | tee -a "$RUN_LOG"
echo "  duration:   ${DURATION_SECONDS}s" | tee -a "$RUN_LOG"
[[ "$RUN_REPLAY" -eq 1 ]] && echo "  replay:     ${REPLAY_DURATION_SECONDS}s metrics window" | tee -a "$RUN_LOG"
[[ "$RUN_REPLAY" -eq 1 ]] && echo "  blink:      $BLINK_PROFILE priority-arrow profile" | tee -a "$RUN_LOG"
if [[ -n "$FROM_CSV" ]]; then
  echo "  V1 source:  recorded CSV (no live emulator)" | tee -a "$RUN_LOG"
else
  echo "  V1 source:  managed LightBlue-compatible emulator" | tee -a "$RUN_LOG"
fi
if [[ -z "$FROM_CSV" && "$UPLOAD" -eq 1 ]]; then
  echo "  post-upload: ${POST_UPLOAD_SETTLE_SECONDS}s unscored SD settle" | tee -a "$RUN_LOG"
fi
if [[ "$CAPTURE_CAMERA" -eq 1 ]]; then
  echo "  camera:     role is fixed per suite" | tee -a "$RUN_LOG"
  [[ "$RUN_CORE" -eq 1 ]] && echo "              core = diagnostic capture (not gated)" | tee -a "$RUN_LOG"
  [[ "$RUN_DISPLAY" -eq 1 ]] && echo "              display = preview exercise (not gated)" | tee -a "$RUN_LOG"
  [[ "$RUN_REPLAY" -eq 1 ]] && echo "              replay = same-window log validator (gated)" | tee -a "$RUN_LOG"
else
  echo "  camera:     disabled" | tee -a "$RUN_LOG"
fi
if [[ "${#COMPARE_TO[@]}" -gt 0 ]]; then
  echo "  baseline:   explicit ${#COMPARE_TO[@]}-run comparison window" | tee -a "$RUN_LOG"
elif [[ "$USE_BASELINE" -eq 1 ]]; then
  echo "  baseline:   board/product/hardware-scoring/suite/scenario identity (if present)" | tee -a "$RUN_LOG"
else
  echo "  baseline:   disabled" | tee -a "$RUN_LOG"
fi
[[ "$PROMOTE_BASELINE" -eq 1 ]] \
  && echo "  promote:    core/display on PASS for future matching runs" | tee -a "$RUN_LOG"
echo "  obd/proxy:  not part of bench gate" | tee -a "$RUN_LOG"
echo "  git clean:  $GIT_WORKTREE_CLEAN" | tee -a "$RUN_LOG"
echo "  artifacts:  current run (see the latest link after completion)" | tee -a "$RUN_LOG"
echo "==========================================" | tee -a "$RUN_LOG"
echo | tee -a "$RUN_LOG"

first_live=1
CURRENT_PID=""

cleanup_current_process() {
  if [[ -n "$CURRENT_PID" ]] && kill -0 "$CURRENT_PID" 2>/dev/null; then
    kill -TERM "$CURRENT_PID" 2>/dev/null || true
    local shutdown_deadline=$((SECONDS + WRAPPER_SHUTDOWN_GRACE_SECONDS))
    while (( SECONDS < shutdown_deadline )); do
      kill -0 "$CURRENT_PID" 2>/dev/null || break
      sleep 1
    done
    if kill -0 "$CURRENT_PID" 2>/dev/null; then
      kill -KILL "$CURRENT_PID" 2>/dev/null || true
    fi
    wait "$CURRENT_PID" 2>/dev/null || true
  fi
  CURRENT_PID=""
}

handle_signal() {
  local status="$1"
  cleanup_current_process
  exit "$status"
}

trap cleanup_current_process EXIT
trap 'handle_signal 130' INT
trap 'handle_signal 143' TERM HUP

v1replay_build_status=0
if [[ -z "$FROM_CSV" ]]; then
  echo "==> v1replay_build" | tee -a "$RUN_LOG"
  python3 "$ROOT_DIR/scripts/bench/run_logged.py" \
    --stdout "$RUN_DIR/v1replay_build.log" \
    --stderr "$RUN_DIR/v1replay_build.err" \
    --combined "$RUN_LOG" \
    -- "$ROOT_DIR/tools/v1replay/scripts/build.sh" &
  CURRENT_PID=$!
  wait "$CURRENT_PID" || v1replay_build_status=$?
  CURRENT_PID=""
  echo "==> v1replay_build exit=$v1replay_build_status" | tee -a "$RUN_LOG"
  echo | tee -a "$RUN_LOG"
fi

for suite in "${suites[@]}"; do
  step_dir="$RUN_DIR/$suite"
  mkdir -p "$step_dir"
  if [[ "$v1replay_build_status" -ne 0 ]]; then
    continue
  fi
  suite_duration="$DURATION_SECONDS"
  if [[ "$suite" == "replay" ]]; then
    suite_duration="$REPLAY_DURATION_SECONDS"
  fi
  identity_manifest="$step_dir/identity.json"
  args=(
    python3 "$ROOT_DIR/scripts/bench/run_window.py"
    --suite "$suite"
    --duration-seconds "$suite_duration"
    --out-dir "$step_dir"
    --board-id "$SAFE_BOARD_ID"
    --git-sha "$GIT_SHA"
    --git-ref "$GIT_REF"
    --git-worktree-clean "$GIT_WORKTREE_CLEAN"
    --identity-manifest "$identity_manifest"
    --segment "$SEGMENT"
    --post-upload-settle-seconds "$POST_UPLOAD_SETTLE_SECONDS"
    --runner-stdout-log "$step_dir/run.log"
    --runner-stderr-log "$step_dir/run.err"
  )
  if [[ -z "$FROM_CSV" ]]; then
    args+=(--replay-executable "$V1REPLAY_EXECUTABLE")
  fi
  [[ "$suite" == "replay" ]] && args+=(--blink-profile "$BLINK_PROFILE")
  [[ "$suite" == "replay" && -n "$SCENARIO" ]] && args+=(--scenario "$SCENARIO")
  [[ "$CAPTURE_CAMERA" -eq 1 ]] && args+=(--camera)
  [[ -n "$PORT" ]] && args+=(--port "$PORT")
  [[ "$USE_BASELINE" -eq 1 && "${#COMPARE_TO[@]}" -eq 0 && "$suite" != "replay" ]] \
    && args+=(--baseline-root "$BASELINE_ROOT")
  for compare_to in "${COMPARE_TO[@]}"; do
    args+=(--compare-to "$compare_to")
  done
  if [[ -n "$FROM_CSV" ]]; then
    args+=(--from-csv "$FROM_CSV")
  elif [[ "$UPLOAD" -eq 1 && "$first_live" -eq 1 ]]; then
    args+=(--upload)
    [[ "$SKIP_WEB" -eq 1 ]] && args+=(--skip-web)
    first_live=0
  fi

  echo "==> bench_$suite" | tee -a "$RUN_LOG"
  status=0
  python3 "$ROOT_DIR/scripts/bench/run_logged.py" \
    --stdout "$step_dir/run.log" \
    --stderr "$step_dir/run.err" \
    --combined "$RUN_LOG" \
    -- "${args[@]}" &
  CURRENT_PID=$!
  wait "$CURRENT_PID" || status=$?
  CURRENT_PID=""
  echo "==> bench_$suite exit=$status" | tee -a "$RUN_LOG"
  echo | tee -a "$RUN_LOG"
done

if [[ -L "$ARTIFACT_ROOT/$SAFE_BOARD_ID/latest" ]]; then
  rm "$ARTIFACT_ROOT/$SAFE_BOARD_ID/latest"
elif [[ -e "$ARTIFACT_ROOT/$SAFE_BOARD_ID/latest" ]]; then
  echo "Refusing to replace a non-symlink latest artifact path for $SAFE_BOARD_ID" >&2
  exit 3
fi
ln -s "runs/$(basename "$RUN_DIR")" "$ARTIFACT_ROOT/$SAFE_BOARD_ID/latest"

score_args=(python3 "$ROOT_DIR/tools/bench_score.py" --run-dir "$RUN_DIR")
for suite in "${suites[@]}"; do
  score_args+=(--suite "$suite")
  [[ "$CAPTURE_CAMERA" -eq 1 && "$suite" == "replay" ]] && score_args+=(--camera-suite "$suite")
done
score_status=0
python3 "$ROOT_DIR/scripts/bench/run_logged.py" \
  --stdout "$RUN_DIR/score.log" \
  --stderr "$RUN_DIR/score.err" \
  --combined "$RUN_LOG" \
  -- "${score_args[@]}" || score_status=$?

if [[ "$PROMOTE_BASELINE" -eq 1 ]]; then
  if [[ "$score_status" -eq 0 ]]; then
    for suite in "${suites[@]}"; do
      [[ "$suite" == "replay" ]] && continue
      identity_manifest="$RUN_DIR/$suite/identity.json"
      baseline_dir="$(python3 "$ROOT_DIR/scripts/bench/bench_identity.py" baseline-dir \
        --identity "$identity_manifest" \
        --baseline-root "$BASELINE_ROOT" \
        --board-id "$SAFE_BOARD_ID")"
      mkdir -p "$baseline_dir"
      cp "$identity_manifest" "$baseline_dir/identity.json"
      cp "$RUN_DIR/$suite/manifest.json" "$baseline_dir/manifest.json"
      cp "$RUN_DIR/$suite/metrics.ndjson" "$baseline_dir/metrics.ndjson"
      cp "$RUN_DIR/$suite/scoring.json" "$baseline_dir/scoring.json"
      cp "$RUN_DIR/$suite/import_diagnostics.json" "$baseline_dir/import_diagnostics.json"
      product_fingerprint="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["product_fingerprint"])' "$identity_manifest")"
      grader_fingerprint="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["grader_fingerprint"])' "$identity_manifest")"
      hardware_scoring_fingerprint="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["hardware_scoring_fingerprint"])' "$identity_manifest")"
      scenario_fingerprint="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["scenario_fingerprint"])' "$identity_manifest")"
      cat > "$baseline_dir/baseline_metadata.json" <<EOF
{
  "schema_version": 3,
  "promoted_from": "runs/$(basename "$RUN_DIR")/$suite",
  "promoted_at_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "board_id": "$SAFE_BOARD_ID",
  "suite": "$suite",
  "product_fingerprint": "$product_fingerprint",
  "grader_fingerprint": "$grader_fingerprint",
  "hardware_scoring_fingerprint": "$hardware_scoring_fingerprint",
  "scenario_fingerprint": "$scenario_fingerprint",
  "git_sha": "$GIT_SHA",
  "git_ref": "$GIT_REF"
}
EOF
    done
    echo "Promoted future-run core/display baselines for this board." | tee -a "$RUN_LOG"
    echo "Current manifests retain the baseline comparison available when they were scored." | tee -a "$RUN_LOG"
  else
    echo "Baseline promotion skipped: bench result was not PASS (exit=$score_status)" | tee -a "$RUN_LOG"
  fi
fi

echo "Latest artifacts: selected artifact root / $SAFE_BOARD_ID / latest" | tee -a "$RUN_LOG"
echo "==> bench investigation (non-gating; bench exit remains $score_status)" | tee -a "$RUN_LOG"
investigation_status=0
investigator_args=(python3 "$ROOT_DIR/tools/bench_investigate.py" "$RUN_DIR")
if [[ "$HOSTED_INVESTIGATOR" -eq 1 ]]; then
  investigator_args+=(
    --hosted
    --model gpt-5.6-sol
  )
else
  investigator_args+=(
    --local-provider "${BENCH_INVESTIGATOR_LOCAL_PROVIDER:-ollama}"
    --model "${BENCH_INVESTIGATOR_MODEL:-qwen3-vl:8b}"
  )
fi
"${investigator_args[@]}" || investigation_status=$?
if [[ "$investigation_status" -ne 0 ]]; then
  echo "Bench investigation backend failed (exit=$investigation_status); bench exit remains $score_status" >&2
fi
exit "$score_status"
