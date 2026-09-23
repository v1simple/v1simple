#!/usr/bin/env bash
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR" || exit 2

ARTIFACT_ROOT="${BENCH_ARTIFACT_ROOT:-$ROOT_DIR/.artifacts/bench}"
BOARD_ID="${BENCH_BOARD_ID:-release}"
DURATION_SECONDS="${BENCH_REPLAY_DURATION_SECONDS:-300}"
POST_UPLOAD_SETTLE_SECONDS="${BENCH_POST_UPLOAD_SETTLE_SECONDS:-90}"
PRESENTATION_API_BASE_URL="http://192.168.35.5"
PIO_CMD="${PIO_CMD:-pio}"
PORT="${DEVICE_PORT:-}"
RUN_REPLAY=0
CAMERA_REQUESTED=0
KU_QUALIFICATION=0
PHOTO_LABEL_QUALIFICATION=0
JUNK_QUALIFICATION=0

usage() {
  printf 'Usage: ./bench.sh --replay --camera [--ku-qualification|--photo-label-qualification|--junk-qualification]\n'
  printf 'Builds and flashes the current firmware, sends the generated replay stimuli, and retains raw synchronized capture.\n'
  printf 'With the DUT in maintenance, the bench joins V1-Simple and captures settings before serial discovery or upload.\n'
}

fail() {
  printf 'COLLECTION_FAILED: %s\n' "$1"
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --replay) RUN_REPLAY=1 ;;
    --camera) CAMERA_REQUESTED=1 ;;
    --ku-qualification) KU_QUALIFICATION=1 ;;
    --photo-label-qualification) PHOTO_LABEL_QUALIFICATION=1 ;;
    --junk-qualification) JUNK_QUALIFICATION=1 ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      usage
      exit 2
      ;;
  esac
  shift
done

[[ "$RUN_REPLAY" -eq 1 && "$CAMERA_REQUESTED" -eq 1 ]] || {
  usage
  exit 2
}
[[ "$DURATION_SECONDS" =~ ^[1-9][0-9]*$ ]] || fail 'duration must be a positive integer'
[[ "$POST_UPLOAD_SETTLE_SECONDS" =~ ^[0-9]+$ ]] || fail 'post-upload settle time must be an integer'
if [[ "$KU_QUALIFICATION" -eq 1 && -z "${BENCH_REPLAY_DURATION_SECONDS+x}" ]]; then
  DURATION_SECONDS=25
fi
if [[ "$PHOTO_LABEL_QUALIFICATION" -eq 1 && -z "${BENCH_REPLAY_DURATION_SECONDS+x}" ]]; then
  DURATION_SECONDS=45
fi
if [[ "$JUNK_QUALIFICATION" -eq 1 && -z "${BENCH_REPLAY_DURATION_SECONDS+x}" ]]; then
  DURATION_SECONDS=17
fi
[[ "$((KU_QUALIFICATION + PHOTO_LABEL_QUALIFICATION + JUNK_QUALIFICATION))" -le 1 ]] \
  || fail 'choose only one focused replay qualification'

BENCH_PYTHON="$("$ROOT_DIR/scripts/bench_python.sh")" || fail 'could not prepare the bench Python environment'
unset PYTHONHOME PYTHONPATH
export PYTHONNOUSERSITE=1

SAFE_BOARD_ID="$(PYTHONPATH="$ROOT_DIR/scripts/bench" "$BENCH_PYTHON" -c \
  'import sys; from artifact_privacy import privacy_safe_identifier; print(privacy_safe_identifier(sys.argv[1], namespace="board"))' \
  "$BOARD_ID" 2>/dev/null)" || fail 'could not create the private-safe board identity'

GIT_SHA="$(git rev-parse HEAD 2>/dev/null)" || fail 'could not identify the source revision'
GIT_SHA_SHORT="$(git rev-parse --short HEAD 2>/dev/null)" || fail 'could not identify the short source revision'
GIT_REF="$(git rev-parse --abbrev-ref HEAD 2>/dev/null)" || fail 'could not identify the source branch'
GIT_STATUS="$(git status --porcelain=v1 --untracked-files=all --ignore-submodules=none 2>/dev/null)" \
  || fail 'could not inspect the source worktree'
[[ -z "$GIT_STATUS" ]] || fail 'source worktree is dirty; raw capture requires an exact source identity'
unset GIT_STATUS

TIMESTAMP="$(date -u +%Y%m%d_%H%M%S)"
RUN_DIR="$ARTIFACT_ROOT/$SAFE_BOARD_ID/runs/${TIMESTAMP}_${GIT_SHA_SHORT}"
if [[ -e "$RUN_DIR" ]]; then
  suffix=2
  while [[ -e "${RUN_DIR}_${suffix}" ]]; do
    suffix=$((suffix + 1))
  done
  RUN_DIR="${RUN_DIR}_${suffix}"
fi
mkdir -p "$RUN_DIR" || fail 'could not create the raw-capture directory'
RUN_LOG="$RUN_DIR/bench.log"
: > "$RUN_LOG" || fail 'could not initialize the raw-capture log'

command -v xcrun >/dev/null 2>&1 || fail 'Xcode command line tools are required to build v1replay'
command -v "$PIO_CMD" >/dev/null 2>&1 || fail 'PlatformIO is required to build and flash the firmware'

if command -v system_profiler >/dev/null 2>&1; then
  CAMERA_NAME="${BENCH_CAMERA_NAME:-Global Shutter Camera}"
  system_profiler SPCameraDataType 2>/dev/null | grep -F -- "$CAMERA_NAME" >/dev/null \
    || fail 'requested camera is unavailable'
fi

printf '[bench] raw run: %s\n' "$RUN_DIR"
printf '[bench] building emulator\n'
build_status=0
"$BENCH_PYTHON" "$ROOT_DIR/scripts/bench/run_logged.py" \
  --stdout "$RUN_DIR/v1replay_build.log" \
  --stderr "$RUN_DIR/v1replay_build.err" \
  --combined "$RUN_LOG" \
  --quiet \
  -- "$ROOT_DIR/tools/v1replay/scripts/build.sh" >/dev/null 2>&1 || build_status=$?
[[ "$build_status" -eq 0 && -x "$ROOT_DIR/tools/v1replay/.build/v1replay" ]] \
  || fail 'v1replay build failed'

REPLAY_DIR="$RUN_DIR/replay"
mkdir -p "$REPLAY_DIR" || fail 'could not create the replay capture directory'

runner_status=0
RUNNER_SCENARIO_ARGS=()
RUNNER_PRESENTATION_ARGS=(--presentation-api-base-url "$PRESENTATION_API_BASE_URL")
if [[ "$KU_QUALIFICATION" -eq 1 ]]; then
  RUNNER_SCENARIO_ARGS+=(--ku-qualification)
fi
if [[ "$PHOTO_LABEL_QUALIFICATION" -eq 1 ]]; then
  RUNNER_SCENARIO_ARGS+=(--photo-label-qualification)
fi
if [[ "$JUNK_QUALIFICATION" -eq 1 ]]; then
  RUNNER_SCENARIO_ARGS+=(--junk-qualification)
fi
"$BENCH_PYTHON" "$ROOT_DIR/scripts/bench/run_logged.py" \
  --stdout "$REPLAY_DIR/run.log" \
  --stderr "$REPLAY_DIR/run.err" \
  --combined "$RUN_LOG" \
  --quiet \
  --terminal-prefix '[bench]' \
  -- "$BENCH_PYTHON" "$ROOT_DIR/scripts/bench/run_window.py" \
    --suite replay \
    --duration-seconds "$DURATION_SECONDS" \
    --out-dir "$REPLAY_DIR" \
    --runner-stdout-log "$REPLAY_DIR/run.log" \
    --runner-stderr-log "$REPLAY_DIR/run.err" \
    --port "$PORT" \
    --board-id "$SAFE_BOARD_ID" \
    --git-sha "$GIT_SHA" \
    --git-ref "$GIT_REF" \
    --git-worktree-clean 1 \
    --post-upload-settle-seconds "$POST_UPLOAD_SETTLE_SECONDS" \
    --replay-executable "$ROOT_DIR/tools/v1replay/.build/v1replay" \
    "${RUNNER_SCENARIO_ARGS[@]}" \
    "${RUNNER_PRESENTATION_ARGS[@]}" \
    --camera || runner_status=$?

[[ "$runner_status" -eq 0 ]] || fail "raw collection did not complete; see $RUN_LOG"
printf 'COMPLETE: %s\n' "$REPLAY_DIR"

# Interpret pixels only after the raw recording and its provenance are final.
# Keep this verdict outside replay/ so the captured evidence stays immutable.
VISUAL_REPORT="$RUN_DIR/visual_fields_result.json"
printf '[bench] reading stable display fields from the recorded video\n'
visual_status=0
VISUAL_PYTHON="$("$ROOT_DIR/scripts/bench_python.sh" --visual)" || visual_status=$?
if [[ "$visual_status" -eq 0 ]]; then
  "$VISUAL_PYTHON" "$ROOT_DIR/scripts/bench/read_replay_fields.py" "$REPLAY_DIR" \
    --output "$VISUAL_REPORT" --progress || visual_status=$?
fi
if [[ ! -s "$VISUAL_REPORT" ]]; then
  printf '{"result":"INCONCLUSIVE","reason":"visual reader unavailable"}\n' > "$VISUAL_REPORT"
fi
visual_result="$("$BENCH_PYTHON" -c \
  'import json, sys; print(json.load(open(sys.argv[1], encoding="utf-8"))["result"])' \
  "$VISUAL_REPORT" 2>/dev/null)" || visual_result=INCONCLUSIVE
case "$visual_result" in
  PASS)
    printf 'VISUAL_FIELDS_PASS: %s\n' "$VISUAL_REPORT"
    ;;
  FAIL)
    printf 'VISUAL_FIELDS_FAIL: %s\n' "$VISUAL_REPORT"
    exit 1
    ;;
  *)
    printf 'VISUAL_FIELDS_INCONCLUSIVE: %s\n' "$VISUAL_REPORT"
    exit 2
    ;;
esac
