#!/usr/bin/env bash
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

ARTIFACT_ROOT="${BENCH_ARTIFACT_ROOT:-$ROOT_DIR/.artifacts/bench}"
BOARD_ID="${BENCH_BOARD_ID:-release}"
DURATION_SECONDS="${BENCH_DURATION_SECONDS:-300}"
REPLAY_DURATION_SECONDS="${BENCH_REPLAY_DURATION_SECONDS:-300}"
POST_UPLOAD_SETTLE_SECONDS="${BENCH_POST_UPLOAD_SETTLE_SECONDS:-90}"
PIO_CMD="${PIO_CMD:-pio}"
PORT="${DEVICE_PORT:-}"
RUN_ALL=0
RUN_REPLAY=0
CAMERA_REQUESTED=0
FLASH=1

usage() {
  printf 'Usage: ./bench.sh --all|--replay [--camera] [--no-flash]\n'
}

fail_usage() {
  printf 'FAIL (collection): usage: ./bench.sh --all|--replay [--camera] [--no-flash]\n'
  exit 2
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --all)
      RUN_ALL=1
      ;;
    --replay)
      RUN_REPLAY=1
      ;;
    --camera)
      CAMERA_REQUESTED=1
      ;;
    --no-flash)
      FLASH=0
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      fail_usage
      ;;
  esac
  shift
done

[[ $((RUN_ALL + RUN_REPLAY)) -eq 1 ]] || fail_usage
[[ "$DURATION_SECONDS" =~ ^[1-9][0-9]*$ ]] || fail_usage
[[ "$REPLAY_DURATION_SECONDS" =~ ^[1-9][0-9]*$ ]] || fail_usage
[[ "$POST_UPLOAD_SETTLE_SECONDS" =~ ^[0-9]+$ ]] || fail_usage

SAFE_BOARD_ID="$(PYTHONPATH="$ROOT_DIR/scripts/bench" python3 -c \
  'import sys; from artifact_privacy import privacy_safe_identifier; print(privacy_safe_identifier(sys.argv[1], namespace="board"))' \
  "$BOARD_ID" 2>/dev/null)" || {
  printf 'FAIL (collection): could not create the private-safe board identity\n'
  exit 2
}

GIT_SHA="$(git rev-parse HEAD 2>/dev/null || printf unknown)"
GIT_SHA_SHORT="$(git rev-parse --short HEAD 2>/dev/null || printf unknown)"
GIT_REF="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || printf unknown)"
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
if ! mkdir -p "$RUN_DIR" 2>/dev/null; then
  printf 'FAIL (collection): could not create the bench run directory\n'
  exit 2
fi
RUN_LOG="$RUN_DIR/bench.log"
if ! : > "$RUN_LOG" 2>/dev/null; then
  printf 'FAIL (collection): could not initialize the bench run log\n'
  exit 2
fi

printf '[bench] run started — evidence: %s\n' "$RUN_DIR"
printf '[bench] detail streams to bench.log in that directory; the verdict prints here last\n'

publish_latest() {
  local board_root="$ARTIFACT_ROOT/$SAFE_BOARD_ID"
  local latest="$board_root/latest"
  mkdir -p "$board_root" 2>/dev/null || return 1
  if [[ -L "$latest" ]]; then
    unlink "$latest" 2>/dev/null || return 1
  elif [[ -e "$latest" ]]; then
    return 1
  fi
  ln -s "runs/$(basename "$RUN_DIR")" "$latest" 2>/dev/null
}

finish() {
  local verdict="$1"
  local status="$2"
  if ! publish_latest; then
    verdict="FAIL (collection): could not update the latest evidence link"
    status=2
  fi
  printf '%s\n' "$verdict"
  exit "$status"
}

SIGNALLED=0
handle_signal() {
  SIGNALLED=1
}
trap handle_signal INT TERM HUP

detect_usb_port() {
  if [[ -n "$PORT" ]]; then
    [[ -e "$PORT" ]] || return 1
    printf '%s\n' "$PORT"
    return 0
  fi

  shopt -s nullglob
  local candidates=(
    /dev/cu.usbmodem*
    /dev/tty.usbmodem*
    /dev/ttyACM*
    /dev/ttyUSB*
    /dev/cu.usbserial*
    /dev/cu.SLAB_USBtoUART*
    /dev/tty.SLAB_USBtoUART*
  )
  shopt -u nullglob
  if [[ ${#candidates[@]} -gt 0 ]]; then
    printf '%s\n' "${candidates[0]}"
    return 0
  fi

  command -v "$PIO_CMD" >/dev/null 2>&1 || return 1
  "$PIO_CMD" device list 2>/dev/null \
    | awk '/^\/dev\// && /usbmodem|ttyACM|ttyUSB|usbserial|SLAB_USBtoUART/ {print $1; exit}'
}

PORT="$(detect_usb_port || true)"
if [[ -z "$PORT" ]]; then
  printf 'board: missing\n' >> "$RUN_LOG"
  finish 'PASS-PARTIAL (skipped: board missing)' 1
fi

CAMERA_ENABLED=0
if [[ "$CAMERA_REQUESTED" -eq 1 ]]; then
  CAMERA_ENABLED=1
  if command -v system_profiler >/dev/null 2>&1; then
    CAMERA_NAME="${BENCH_CAMERA_NAME:-Global Shutter Camera}"
    if ! system_profiler SPCameraDataType 2>/dev/null \
      | grep -F -- "$CAMERA_NAME" >/dev/null; then
      CAMERA_ENABLED=0
    fi
  fi
fi

if ! command -v xcrun >/dev/null 2>&1; then
  finish 'FAIL (emulator): Xcode command line tools are required to build v1replay' 2
fi
if [[ "$FLASH" -eq 1 ]] && ! command -v "$PIO_CMD" >/dev/null 2>&1; then
  finish 'FAIL (collection): PlatformIO is required to build and flash the firmware' 2
fi

printf '[bench] building v1replay emulator...\n'
printf 'v1replay build: started\n' >> "$RUN_LOG"
build_status=0
python3 "$ROOT_DIR/scripts/bench/run_logged.py" \
  --stdout "$RUN_DIR/v1replay_build.log" \
  --stderr "$RUN_DIR/v1replay_build.err" \
  --combined "$RUN_LOG" \
  --quiet \
  -- "$ROOT_DIR/tools/v1replay/scripts/build.sh" >/dev/null 2>&1 || build_status=$?
printf 'v1replay build: exit=%s\n' "$build_status" >> "$RUN_LOG"
if [[ "$SIGNALLED" -eq 1 ]]; then
  finish 'FAIL (collection): interrupted' 2
fi
if [[ "$build_status" -ne 0 || ! -x "$ROOT_DIR/tools/v1replay/.build/v1replay" ]]; then
  finish 'FAIL (emulator): v1replay build failed' 2
fi

read_window_result() {
  python3 - "$1" <<'PY'
import json
import re
import sys
from pathlib import Path

try:
    payload = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError):
    payload = {}

result = str(payload.get("result") or "COLLECTION_FAILED")
kind = str(payload.get("failure_kind") or "none")
message = str(
    payload.get("error")
    or "leg did not produce a readable result"
)
message = re.sub(r"^FAIL \([^)]*\):\s*", "", message)
message = " ".join(message.split())[:512]
print("\t".join((result, kind, message)))
PY
}

print_window_summary() {
  python3 - "$1" "$2" "$ROOT_DIR" <<'PY'
import json
import sys
from pathlib import Path

result_path = Path(sys.argv[1]).resolve()
suite = sys.argv[2]

payload = json.loads(result_path.read_text(encoding="utf-8"))
completion = payload.get("completion") or {}
duration = completion.get("duration_seconds")
serial_lines = completion.get("serial_lines_observed")
if isinstance(duration, (int, float)) and isinstance(serial_lines, int):
    print(
        f"[bench] {suite} external window: {duration:g}s"
        f" | serial lines observed {serial_lines}"
    )

camera = payload.get("camera")
if isinstance(camera, dict) and camera.get("result") == "CAPTURED":
    stats = camera.get("recorder_stats") or {}
    probe = camera.get("video_probe") or {}
    frames = stats.get("frames_appended")
    fps = probe.get("average_frame_rate")
    capture_drops = stats.get("capture_drops")
    writer_drops = stats.get("writer_backpressure_drops")
    if all(isinstance(value, (int, float)) for value in (frames, fps, capture_drops, writer_drops)):
        print(
            f"[bench] {suite} camera: {int(frames):,} frames @ {float(fps):.1f}fps"
            f" | capture drops {int(capture_drops)} | writer drops {int(writer_drops)}"
        )
PY
}

V1REPLAY_EXECUTABLE="$ROOT_DIR/tools/v1replay/.build/v1replay"
SUITES=(core display replay)
[[ "$RUN_REPLAY" -eq 1 ]] && SUITES=(replay)
first_suite=1
for suite in "${SUITES[@]}"; do
  step_dir="$RUN_DIR/$suite"
  mkdir -p "$step_dir" 2>/dev/null || finish "FAIL (collection): $suite evidence directory could not be created" 2
  suite_duration="$DURATION_SECONDS"
  [[ "$suite" == "replay" ]] && suite_duration="$REPLAY_DURATION_SECONDS"
  args=(
    python3 "$ROOT_DIR/scripts/bench/run_window.py"
    --suite "$suite"
    --duration-seconds "$suite_duration"
    --out-dir "$step_dir"
    --runner-stdout-log "$step_dir/run.log"
    --runner-stderr-log "$step_dir/run.err"
    --port "$PORT"
    --board-id "$SAFE_BOARD_ID"
    --git-sha "$GIT_SHA"
    --git-ref "$GIT_REF"
    --git-worktree-clean "$GIT_WORKTREE_CLEAN"
    --post-upload-settle-seconds "$POST_UPLOAD_SETTLE_SECONDS"
    --replay-executable "$V1REPLAY_EXECUTABLE"
  )
  if [[ "$CAMERA_ENABLED" -eq 1 ]]; then
    args+=(--camera)
  fi
  if [[ "$FLASH" -eq 1 && "$first_suite" -eq 1 ]]; then
    args+=(--upload)
  fi

  leg_note=""
  [[ "$FLASH" -eq 1 && "$first_suite" -eq 1 ]] && leg_note=" (firmware build + flash + ${POST_UPLOAD_SETTLE_SECONDS}s settle first)"
  printf '[bench] %s leg: %ss collection%s\n' "$suite" "$suite_duration" "$leg_note"
  printf '%s: started\n' "$suite" >> "$RUN_LOG"
  runner_status=0
  python3 "$ROOT_DIR/scripts/bench/run_logged.py" \
    --stdout "$step_dir/run.log" \
    --stderr "$step_dir/run.err" \
    --combined "$RUN_LOG" \
    --quiet \
    -- "${args[@]}" >/dev/null 2>&1 || runner_status=$?
  printf '%s: exit=%s\n' "$suite" "$runner_status" >> "$RUN_LOG"
  first_suite=0

  if [[ "$SIGNALLED" -eq 1 || "$runner_status" -eq 130 ]]; then
    finish 'FAIL (collection): interrupted' 2
  fi

  result=""
  failure_kind=""
  reason=""
  IFS=$'\t' read -r result failure_kind reason \
    < <(read_window_result "$step_dir/window_result.json" 2>/dev/null)
  if [[ "$result" == "PASS" && "$runner_status" -eq 0 ]]; then
    if ! print_window_summary "$step_dir/window_result.json" "$suite"; then
      printf '%s: external evidence summary unavailable\n' "$suite" >> "$RUN_LOG"
    fi
    continue
  fi

  classification="collection"
  if [[ "$failure_kind" == camera_* ]]; then
    classification="provisional"
  elif [[ "$reason" == *"V1 emulator"* || "$reason" == *"v1replay"* ]]; then
    classification="emulator"
  elif [[ "$result" == "FAIL" || "$runner_status" -eq 2 ]]; then
    classification="semantic"
  fi
  [[ -n "$reason" ]] || reason="leg failed"
  finish "FAIL ($classification): $suite: $reason" 2
done

if [[ "$CAMERA_REQUESTED" -eq 1 && "$CAMERA_ENABLED" -eq 0 ]]; then
  finish 'PASS-PARTIAL (skipped: camera unplugged)' 1
fi
finish 'PASS' 0
