#!/usr/bin/env bash
set -uo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

ARTIFACT_ROOT="${BENCH_ARTIFACT_ROOT:-$ROOT_DIR/.artifacts/bench}"
ENCOUNTER_QUALIFICATION="${BENCH_ENCOUNTER_QUALIFICATION:-$ARTIFACT_ROOT/qualification/encounter-reader.json}"
BOARD_ID="${BENCH_BOARD_ID:-release}"
DURATION_SECONDS="${BENCH_DURATION_SECONDS:-300}"
REPLAY_DURATION_SECONDS="${BENCH_REPLAY_DURATION_SECONDS:-300}"
POST_UPLOAD_SETTLE_SECONDS="${BENCH_POST_UPLOAD_SETTLE_SECONDS:-90}"
PIO_CMD="${PIO_CMD:-pio}"
PORT="${DEVICE_PORT:-}"
RUN_ALL=0
RUN_REPLAY=0
ANALYZE_RECORDING=""
COMPARE_TO=""
REUSE_READINGS=""
ANALYSIS_RANGES=()
CAMERA_REQUESTED=0
FLASH=1
RESIDENT_RECORDING=""
RESIDENT_IMAGE=""
COLLECTION_ONLY=0
COLLECTION_ONLY_REASON=""
QUALIFICATION_CAPTURE=0
PERSISTENCE_COVERAGE=0
READER_WORKERS="${BENCH_READER_WORKERS:-4}"
COUNTER_RESULT="NOT_EVALUATED"
COUNTER_PRINTED=0
ENCOUNTER_RESULT="NOT_EVALUATED"
ENCOUNTER_PRINTED=0
ENCOUNTER_REASON=""

usage() {
  printf 'Usage: ./bench.sh --all|--replay [--camera] [--no-flash] [--compare-to RESULT_JSON] [--qualification-capture|--persistence-coverage]\n'
  printf '       ./bench.sh --analyze-recording DIR [--range START:END ...] [--compare-to RESULT_JSON] [--reuse-readings RESULT_JSON]\n'
  printf '       --no-flash may use --resident-recording DIR --resident-image FILE to verify a prior uploaded image\n'
  printf '       --reader-workers 1..8 selects bounded offline frame readers\n'
}

fail_usage() {
  printf 'FAIL (collection): usage: ./bench.sh --all|--replay [--camera] [--no-flash] [--qualification-capture]\n'
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
    --analyze-recording)
      [[ $# -ge 2 && -z "$ANALYZE_RECORDING" && -n "$2" ]] || fail_usage
      ANALYZE_RECORDING="$2"
      shift
      ;;
    --range)
      [[ $# -ge 2 && -n "$2" ]] || fail_usage
      ANALYSIS_RANGES+=(--range "$2")
      shift
      ;;
    --compare-to)
      [[ $# -ge 2 && -z "$COMPARE_TO" && -n "$2" ]] || fail_usage
      COMPARE_TO="$2"
      shift
      ;;
    --reuse-readings)
      [[ $# -ge 2 && -z "$REUSE_READINGS" && -n "$2" ]] || fail_usage
      REUSE_READINGS="$2"
      shift
      ;;
    --camera)
      CAMERA_REQUESTED=1
      ;;
    --no-flash)
      FLASH=0
      ;;
    --resident-recording)
      [[ $# -ge 2 && -z "$RESIDENT_RECORDING" && -n "$2" ]] || fail_usage
      RESIDENT_RECORDING="$2"
      shift
      ;;
    --resident-image)
      [[ $# -ge 2 && -z "$RESIDENT_IMAGE" && -n "$2" ]] || fail_usage
      RESIDENT_IMAGE="$2"
      shift
      ;;
    --qualification-capture)
      QUALIFICATION_CAPTURE=1
      ;;
    --persistence-coverage)
      PERSISTENCE_COVERAGE=1
      ;;
    --reader-workers)
      [[ $# -ge 2 && -n "$2" ]] || fail_usage
      READER_WORKERS="$2"
      shift
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

[[ -z "$REUSE_READINGS" || -n "$ANALYZE_RECORDING" ]] || fail_usage
[[ "$READER_WORKERS" =~ ^[1-8]$ ]] || fail_usage
if [[ "$PERSISTENCE_COVERAGE" -eq 1 ]]; then
  [[ "$RUN_REPLAY" -eq 1 && "$RUN_ALL" -eq 0 && "$CAMERA_REQUESTED" -eq 1 \
     && "$QUALIFICATION_CAPTURE" -eq 0 && -z "$ANALYZE_RECORDING" ]] || fail_usage
  # Leave connection/handshake time around the complete 64-second input sequence.
  REPLAY_DURATION_SECONDS=90
fi

if [[ -n "$RESIDENT_RECORDING" || -n "$RESIDENT_IMAGE" ]]; then
  [[ -n "$RESIDENT_RECORDING" && -n "$RESIDENT_IMAGE" && "$FLASH" -eq 0 \
     && -z "$ANALYZE_RECORDING" ]] || fail_usage
fi

if [[ -n "$ANALYZE_RECORDING" ]]; then
  [[ $((RUN_ALL + RUN_REPLAY + CAMERA_REQUESTED + QUALIFICATION_CAPTURE)) -eq 0 && "$FLASH" -eq 1 ]] || fail_usage
else
  [[ $((RUN_ALL + RUN_REPLAY)) -eq 1 && ${#ANALYSIS_RANGES[@]} -eq 0 ]] || fail_usage
fi
if [[ "$QUALIFICATION_CAPTURE" -eq 1 \
      && ("$RUN_REPLAY" -ne 1 || "$RUN_ALL" -ne 0 || "$CAMERA_REQUESTED" -ne 1) ]]; then
  fail_usage
fi
if [[ -n "$COMPARE_TO" ]]; then
  [[ "$QUALIFICATION_CAPTURE" -eq 0 && ( -n "$ANALYZE_RECORDING" || "$CAMERA_REQUESTED" -eq 1 ) ]] || fail_usage
fi
[[ "$DURATION_SECONDS" =~ ^[1-9][0-9]*$ ]] || fail_usage
[[ "$REPLAY_DURATION_SECONDS" =~ ^[1-9][0-9]*$ ]] || fail_usage
[[ "$POST_UPLOAD_SETTLE_SECONDS" =~ ^[0-9]+$ ]] || fail_usage
if [[ "$CAMERA_REQUESTED" -eq 1 && "$QUALIFICATION_CAPTURE" -eq 0 ]]; then
  ENCOUNTER_RESULT="MEASUREMENT_INCOMPLETE"
  ENCOUNTER_REASON="requested camera evidence is unavailable"
fi

runtime_args=()
if [[ -n "$ANALYZE_RECORDING" || ( "$CAMERA_REQUESTED" -eq 1 && "$QUALIFICATION_CAPTURE" -eq 0 ) ]]; then
  runtime_args+=("$ENCOUNTER_QUALIFICATION")
fi
BENCH_PYTHON="$("$ROOT_DIR/scripts/bench_python.sh" "${runtime_args[@]}")" || {
  printf 'MEASUREMENT_INCOMPLETE (reader environment)\n'
  exit 2
}
unset PYTHONHOME PYTHONPATH
export PYTHONNOUSERSITE=1

SAFE_BOARD_ID="$(PYTHONPATH="$ROOT_DIR/scripts/bench" "$BENCH_PYTHON" -c \
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
  if [[ "$COUNTER_PRINTED" -eq 0 ]]; then
    printf '[bench] sampled live counter: %s\n' "$COUNTER_RESULT"
    COUNTER_PRINTED=1
  fi
  if [[ "$ENCOUNTER_PRINTED" -eq 0 ]]; then
    printf '[bench] visual behavior: %s' "$ENCOUNTER_RESULT"
    [[ -n "$ENCOUNTER_REASON" ]] && printf ' | %s' "$ENCOUNTER_REASON"
    printf '\n'
    ENCOUNTER_PRINTED=1
  elif [[ "$verdict" != "$ENCOUNTER_RESULT"* ]]; then
    printf '[bench] visual behavior: %s\n' "$ENCOUNTER_RESULT"
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


read_window_result() {
  "$BENCH_PYTHON" - "$1" <<'PY'
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
    or payload.get("qualification_reason")
    or "leg did not produce a readable result"
)
message = re.sub(r"^FAIL \([^)]*\):\s*", "", message)
message = " ".join(message.split())[:512]
print("\t".join((result, kind, message)))
PY
}

print_window_summary() {
  "$BENCH_PYTHON" - "$1" "$2" "$ROOT_DIR" <<'PY'
import json
import sys
from pathlib import Path

result_path = Path(sys.argv[1]).resolve()
suite = sys.argv[2]

payload = json.loads(result_path.read_text(encoding="utf-8"))
window_result = payload.get("result")
completion = payload.get("completion") or {}
duration = completion.get("duration_seconds")
serial_lines = completion.get("serial_lines_observed")
if isinstance(window_result, str) and isinstance(duration, (int, float)) and isinstance(serial_lines, int):
    print(
        f"[bench] {suite} collection: {window_result} | {duration:g}s"
        f" | serial lines observed {serial_lines}"
    )

identity = payload.get("runtime_identity") or {}
qualification = payload.get("runtime_qualification") or {}
runtime_git = identity.get("git_sha")
runtime_image = identity.get("image_id")
mode = qualification.get("mode")
status = qualification.get("status")
if all(isinstance(value, str) and value for value in (runtime_git, runtime_image, mode, status)):
    print(
        f"[bench] {suite} runtime: git {runtime_git} | image {runtime_image}"
        f" | {mode} {status}"
    )

delivery = (payload.get("emulator") or {}).get("notification_delivery") or {}
if all(type(delivery.get(key)) is int for key in ("delivered", "requested", "dropped", "skipped")):
    print(
        f"[bench] {suite} host input acceptance: {delivery['delivered']} / {delivery['requested']} packets"
        f" | dropped {delivery['dropped']} | skipped {delivery['skipped']} | DUT receipt not observed"
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
            f"[bench] {suite} camera integrity: CAPTURED | {int(frames):,} frames @ {float(fps):.1f}fps"
            f" | capture drops {int(capture_drops)} | writer drops {int(writer_drops)}"
        )
PY
}

read_counter_result() {
  "$BENCH_PYTHON" - "$1" <<'PY'
import json
import sys
from pathlib import Path

try:
    payload = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    result = payload["result"]
    counts = payload["counts"]
    values = [counts[name] for name in ("matched", "mismatched", "unresolved", "required")]
    if result not in ("PASS", "FAIL", "INCONCLUSIVE") or any(type(value) is not int or value < 0 for value in values):
        raise ValueError("invalid counter result")
except (OSError, KeyError, TypeError, ValueError, json.JSONDecodeError):
    result, values = "INCONCLUSIVE", [0, 0, 0, 0]
print("\t".join([result, *(str(value) for value in values)]))
PY
}

run_counter_check() {
  local replay_dir="$1"
  local counter_dir="$replay_dir/counter-check"
  local counter_status=0
  local matched=0
  local mismatched=0
  local unresolved=0
  local required=0

  printf '[bench] sampled live counter: analyzing replay camera evidence...\n'
  "$BENCH_PYTHON" "$ROOT_DIR/scripts/bench/counter_check.py" \
    --run-dir "$replay_dir" \
    --auto \
    --out "$counter_dir" \
    >> "$RUN_LOG" 2>&1 || counter_status=$?
  IFS=$'\t' read -r COUNTER_RESULT matched mismatched unresolved required \
    < <(read_counter_result "$counter_dir/result.json" 2>/dev/null)
  case "$COUNTER_RESULT:$counter_status" in
    PASS:0|FAIL:1|INCONCLUSIVE:2) ;;
    *) COUNTER_RESULT="INCONCLUSIVE" ;;
  esac
  printf 'sampled live counter: result=%s exit=%s\n' "$COUNTER_RESULT" "$counter_status" >> "$RUN_LOG"
  printf '[bench] sampled live counter: %s | %s matched, %s mismatched, %s unresolved / %s required\n' \
    "$COUNTER_RESULT" "$matched" "$mismatched" "$unresolved" "$required"
  COUNTER_PRINTED=1
}

read_encounter_result() {
  "$BENCH_PYTHON" - "$1" "$2" "$SIGNALLED" <<'PYRESULT'
import json
import math
import re
import sys
from pathlib import Path

result, counts, first_id, reason = "MEASUREMENT_INCOMPLETE", ["?"] * 8, "-", ""
try:
    payload = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    summary, events, errors = payload["summary"], payload["events"], payload["errors"]
    qualification = payload["reader_qualification"]
    evidence = payload["evidence"]
    identity = evidence.get("runtime_identity", {}) if isinstance(evidence, dict) else {}
    if (payload.get("schema_version") != 1 or payload.get("kind") != "firmware_visual_behavior"
            or not isinstance(summary, dict) or not isinstance(events, list)
            or not isinstance(errors, list) or any(not isinstance(e, str) for e in errors)
            or not isinstance(qualification, dict) or qualification.get("status") not in ("QUALIFIED", "REJECTED")
            or not isinstance(identity, dict) or any(not isinstance(identity.get(k), str) or not identity[k]
                                                     for k in ("git_sha", "image_id"))):
        raise ValueError("unsupported visual behavior summary")
    names = ("events", "targets_observed", "events_with_findings", "findings",
             "unresolved_frames", "unresolved_field_observations", "read_frames", "available_frames")
    if any(type(summary.get(key)) is not int or summary[key] < 0 for key in (*names, "events_without_complete_target")):
        raise ValueError("visual behavior counts are malformed")
    expected = dict.fromkeys(names, 0)
    expected["events"] = len(events)
    ids, coverage_complete, missing_blink_phases = set(), True, 0
    for event in events:
        eid, observation, findings, coverage = event["event_id"], event["observation"], event["findings"], event["coverage"]
        if (not isinstance(eid, str) or not re.fullmatch(r"[A-Za-z0-9_-]+", eid) or eid in ids
                or not isinstance(observation, dict) or type(observation.get("target_observed")) is not bool
                or not isinstance(findings, list) or any(not isinstance(f, dict) or not f.get("field")
                                                       or not f.get("kind") for f in findings)
                or not isinstance(coverage, dict)):
            raise ValueError("visual behavior event is malformed or duplicated")
        ids.add(eid)
        phases = event.get("phase_observation", {})
        required_phases, observed_phases = phases.get("required_phase_ids", []), phases.get("observed_phase_ids", [])
        if (not isinstance(required_phases, list) or not isinstance(observed_phases, list)
                or any(not isinstance(p, str) for p in required_phases + observed_phases)
                or not set(observed_phases) <= set(required_phases)):
            raise ValueError("blink phase observations are malformed")
        event_missing_phases = len(required_phases) > 1 and set(required_phases) != set(observed_phases)
        missing_blink_phases += int(event_missing_phases)
        fields = observation["fields"]
        if not isinstance(fields, dict) or set(fields) != {
                "counter_glyph", "primary_frequency", "active_bands", "main_arrows",
                "main_bars", "secondary", "muted_badge"}:
            raise ValueError("visual field measurements are missing")
        unknown_fields = 0
        for field in fields.values():
            values = field["counts"]
            if not isinstance(values, dict) or any(type(values.get(k)) is not int or values[k] < 0
                    for k in ("matching_frames", "different_frames", "unresolved_frames")):
                raise ValueError("visual field counts are malformed")
            unknown_fields += values["unresolved_frames"]
        available, read, unresolved = coverage["available_recorded_frames"], coverage["read_recorded_frames"], event["unresolved_frames"]
        unrecorded = coverage["unrecorded_source_frames"]
        if (any(type(n) is not int or n < 0 for n in (available, read, unresolved, unrecorded))
                or read > available or unresolved > available
                or type(coverage.get("complete_recorded_frame_coverage")) is not bool
                or coverage["complete_recorded_frame_coverage"] != (available > 0 and read == available)):
            raise ValueError("visual event coverage is inconsistent")
        coverage_complete &= coverage["complete_recorded_frame_coverage"] and unrecorded == 0
        expected["targets_observed"] += int(observation["target_observed"])
        expected["events_with_findings"] += int(bool(findings))
        expected["findings"] += len(findings)
        expected["unresolved_frames"] += unresolved
        expected["unresolved_field_observations"] += unknown_fields
        expected["read_frames"] += read
        expected["available_frames"] += available
        if first_id == "-" and (findings or not observation["target_observed"] or event_missing_phases):
            first_id = eid
            reason = (f"{eid}: {findings[0].get('reason') or findings[0]['kind']}" if findings
                      else f"{eid}: required blink phases were not observed" if event_missing_phases
                      else f"{eid}: complete target was not observed")
    missing = len(events) - expected["targets_observed"]
    if any(summary[key] != value for key, value in expected.items()) or summary["events_without_complete_target"] != missing:
        raise ValueError("visual behavior counts disagree with event evidence")
    if summary.get("events_with_unobserved_blink_phases", 0) != missing_blink_phases:
        raise ValueError("blink phase count disagrees with event evidence")
    computed = ("MEASUREMENT_INCOMPLETE" if errors or qualification["status"] != "QUALIFIED" else
                "DIFFERENCES_FOUND" if expected["findings"] else
                "MEASUREMENT_INCOMPLETE" if not events or missing or missing_blink_phases or not coverage_complete else
                "NO_DIFFERENCES_OBSERVED")
    if "persistence" in payload:
        from scripts.bench.encounter_persistence import measure_persistence_behavior, persistence_result
        configuration = evidence.get("configuration", {})
        if configuration.get("status") != "verified":
            raise ValueError("persistence configuration is unverified")
        measurement = measure_persistence_behavior(events, configuration.get("settings", {}))
        if payload["persistence"] != measurement:
            raise ValueError("persistence result disagrees with retained observations")
        computed = persistence_result(events, errors, qualification, measurement)
        ordinary_attention = (first_id, reason)
        first_id, reason = "-", measurement.get("reason", "-")
        for case in measurement.get("cases", []):
            if case["findings"] or case.get("missing_stages") or not case.get("stage_order_observed"):
                first_id = case["event_id"]
                reason = (f"{first_id}: {case['findings'][0]['reason']}" if case["findings"] else
                          f"{first_id}: persistence stages not fully observed: {', '.join(case.get('missing_stages', []))}")
                break
        if first_id == "-" and computed != "NO_DIFFERENCES_OBSERVED":
            original_event = next((event for event in events if event["event_id"] == ordinary_attention[0]), None)
            if original_event and (original_event["findings"] or original_event.get("wire_rows")):
                first_id, reason = ordinary_attention
    if payload.get("result") != computed:
        raise ValueError("visual behavior result disagrees with event evidence")
    result, counts = computed, [expected[name] for name in names]
    if errors:
        reason = errors[0]
    elif qualification["status"] != "QUALIFIED":
        reason = "reader qualification was rejected"
    if (result, int(sys.argv[2])) not in (("NO_DIFFERENCES_OBSERVED", 0), ("DIFFERENCES_FOUND", 1), ("MEASUREMENT_INCOMPLETE", 2)):
        result, reason = "MEASUREMENT_INCOMPLETE", f"analysis exit {sys.argv[2]} did not match its retained result"
    if sys.argv[3] == "1":
        result, reason = "MEASUREMENT_INCOMPLETE", "analysis interrupted; completion was not established"
except (OSError, KeyError, TypeError, ValueError, AttributeError) as exc:
    detail = str(exc) if type(exc) is ValueError else "missing or malformed evidence"
    result, counts, first_id, reason = (
        "MEASUREMENT_INCOMPLETE", ["?"] * 8, "-",
        f"analysis result rejected: {detail} (exit {sys.argv[2]}; see bench.log)"
    )
reason = " ".join(reason.split())[:240] or "-"
print("\t".join(map(str, (result, *counts, first_id, reason))))
PYRESULT
}

run_encounter_check() {
  local replay_dir="$1"
  local encounter_dir="${2:-$replay_dir/encounter-check}"
  local encounter_status=0
  local events="?" targets="?" affected="?" findings="?" unresolved="?" unresolved_fields="?" read_frames="?" available="?" first_id="-" reason="-"
  local comparison_args=()
  [[ -z "$COMPARE_TO" ]] || comparison_args+=(--compare-to "$COMPARE_TO")
  [[ -z "$REUSE_READINGS" ]] || comparison_args+=(--reuse-readings "$REUSE_READINGS")
  if [[ "$SIGNALLED" -eq 1 ]]; then
    ENCOUNTER_REASON="analysis interrupted before the visual behavior check"
    return
  fi
  printf '[bench] visual behavior: comparing controlled input with recorded display observations...\n'
  "$BENCH_PYTHON" "$ROOT_DIR/scripts/bench/encounter_check.py" \
    --run-dir "$replay_dir" \
    --observe-behavior \
    --reader-workers "$READER_WORKERS" \
    --reader-qualification "$ENCOUNTER_QUALIFICATION" \
    --out "$encounter_dir" \
    "${ANALYSIS_RANGES[@]}" "${comparison_args[@]}" \
    2>&1 | tee -a "$RUN_LOG" | awk '
      /^Read [0-9]+\/[0-9]+ original event frames$/ {
        split($2, count, "/"); percent = int(100 * count[1] / count[2]);
        if (count[1] == 1 || percent >= next_percent || count[1] == count[2]) {
          printf "[bench] display analysis: %d%% (%s frames read)\n", percent, $2;
          fflush(); next_percent = (int(percent / 10) + 1) * 10;
        }
      }' || encounter_status=$?
  IFS=$'\t' read -r ENCOUNTER_RESULT events targets affected findings unresolved unresolved_fields read_frames available first_id reason \
    < <(read_encounter_result "$encounter_dir/result.json" "$encounter_status" 2>/dev/null)
  case "$ENCOUNTER_RESULT" in
    NO_DIFFERENCES_OBSERVED|DIFFERENCES_FOUND|MEASUREMENT_INCOMPLETE) ;;
    *) ENCOUNTER_RESULT="MEASUREMENT_INCOMPLETE"; reason="could not read the visual behavior result; see bench.log" ;;
  esac
  printf 'visual behavior: result=%s exit=%s qualification=%s\n' \
    "$ENCOUNTER_RESULT" "$encounter_status" "$ENCOUNTER_QUALIFICATION" >> "$RUN_LOG"
  printf '[bench] display: target observed %s/%s events | findings %s | affected events %s\n' \
    "$targets" "$events" "$findings" "$affected"
  printf '[bench] coverage: %s/%s recorded frames read | %s frames with unresolved comparisons (%s field comparisons)\n' \
    "$read_frames" "$available" "$unresolved" "$unresolved_fields"
  printf '[bench] limits: unresolved comparisons remain unknown; observed targets do not grade every frame.\n'
  "$BENCH_PYTHON" - "$encounter_dir/result.json" <<'PERSISTENCE_SUMMARY'
import json, sys
from pathlib import Path
try:
    measured = json.loads(Path(sys.argv[1]).read_text()).get("persistence")
    if measured:
        summary = measured.get("summary", {})
        print(f"[bench] persistence: {measured['result']} | "
              f"{summary.get('complete_sequences', 0)}/{summary.get('cases', 0)} required sequences observed | "
              f"{summary.get('findings', 0)} sequence findings")
        print("[bench] persistence uses ordered display stages; the fixed-target totals above are supporting comparisons.")
except (OSError, ValueError, KeyError, TypeError):
    pass  # The authoritative validator above already rejects absent or malformed results.
PERSISTENCE_SUMMARY
  [[ "$reason" == "-" ]] || printf '[bench] attention: %s\n' "$reason"
  if [[ -f "$encounter_dir/report.html" ]]; then
    local fragment=""
    [[ "$first_id" == "-" ]] || fragment="#event=$first_id"
    printf '[bench] visual report: %s/report.html%s\n' "$encounter_dir" "$fragment"
  else
    printf '[bench] visual report unavailable; see %s\n' "$RUN_LOG"
  fi
  ENCOUNTER_PRINTED=1
}

finish_encounter_product() {
  local label="${1:-visual behavior}"
  case "$ENCOUNTER_RESULT" in
    NO_DIFFERENCES_OBSERVED) finish "NO_DIFFERENCES_OBSERVED ($label)" 0 ;;
    DIFFERENCES_FOUND) finish "DIFFERENCES_FOUND ($label)" 1 ;;
    *) finish "MEASUREMENT_INCOMPLETE ($label)" 2 ;;
  esac
}

write_qualification_capture_record() {
  local replay_dir="$1"
  "$BENCH_PYTHON" - "$replay_dir/window_result.json" "$replay_dir/qualification_capture.json" \
    "$GIT_SHA" "$ROOT_DIR/bench.sh" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

window_path = Path(sys.argv[1])
record_path = Path(sys.argv[2])
git_sha = sys.argv[3]
bench_path = Path(sys.argv[4])
window = json.loads(window_path.read_text(encoding="utf-8"))
if window.get("result") != "PASS" or (window.get("camera") or {}).get("result") != "CAPTURED":
    raise SystemExit("qualified blind capture record requires a passing collection with captured camera evidence")
if record_path.exists():
    raise SystemExit("blind capture record already exists")
for forbidden in ("counter-check", "encounter-check"):
    if (record_path.parent / forbidden).exists():
        raise SystemExit(f"pixel analyzer output already exists: {forbidden}")

digest = lambda path: hashlib.sha256(path.read_bytes()).hexdigest()
record = {
    "schema_version": 1,
    "kind": "blind_visible_reader_qualification_capture",
    "capture_mode": "--qualification-capture",
    "source_git_sha": git_sha,
    "bench_source_sha256": digest(bench_path),
    "collection": {
        "result": "PASS",
        "camera_result": "CAPTURED",
        "window_result": window_path.name,
        "window_result_sha256": digest(window_path),
    },
    "pixel_analysis": {
        "status": "WITHHELD_BY_CAPTURE_MODE",
        "executed": [],
        "disabled": ["counter_check", "encounter_check"],
        "analyzer_outputs_present": False,
    },
    "visible_product_eligible": False,
}
record_path.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n", encoding="utf-8")
PY
}

if [[ -n "$ANALYZE_RECORDING" ]]; then
  printf '[bench] OFFLINE recorded firmware analysis; no current DUT evaluation or new physical qualification\n'
  printf 'analysis_mode: OFFLINE_RECORDED_FIRMWARE\nanalysis_git_sha: %s\nanalysis_worktree_clean: %s\n' \
    "$GIT_SHA" "$GIT_WORKTREE_CLEAN" >> "$RUN_LOG"
  print_window_summary "$ANALYZE_RECORDING/window_result.json" 'OFFLINE recorded' 2>/dev/null || true
  run_encounter_check "$ANALYZE_RECORDING" "$RUN_DIR/encounter-check"
  finish_encounter_product 'OFFLINE recorded visual behavior'
fi

if [[ "$GIT_WORKTREE_CLEAN" -ne 1 ]]; then
  printf 'qualification: source worktree is dirty; exact source identity is unavailable\n' >> "$RUN_LOG"
  finish 'FAIL (qualification): source worktree is dirty; commit or remove relevant changes before bench qualification' 2
fi

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
"$BENCH_PYTHON" "$ROOT_DIR/scripts/bench/run_logged.py" \
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
    "$BENCH_PYTHON" "$ROOT_DIR/scripts/bench/run_window.py"
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
  if [[ "$QUALIFICATION_CAPTURE" -eq 1 ]]; then
    args+=(--reader-qualification)
  fi
  if [[ "$PERSISTENCE_COVERAGE" -eq 1 ]]; then
    args+=(--persistence-coverage)
  fi
  if [[ "$FLASH" -eq 1 && "$first_suite" -eq 1 ]]; then
    args+=(--upload)
  fi
  if [[ -n "$RESIDENT_RECORDING" ]]; then
    args+=(--resident-recording "$RESIDENT_RECORDING" --resident-image "$RESIDENT_IMAGE")
  fi

  leg_note=""
  [[ "$FLASH" -eq 1 && "$first_suite" -eq 1 ]] && leg_note=" (firmware build + flash + ${POST_UPLOAD_SETTLE_SECONDS}s settle first)"
  printf '[bench] %s leg: %ss collection%s\n' "$suite" "$suite_duration" "$leg_note"
  printf '%s: started\n' "$suite" >> "$RUN_LOG"
  runner_status=0
  "$BENCH_PYTHON" "$ROOT_DIR/scripts/bench/run_logged.py" \
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
    if [[ "$suite" == "replay" && "$CAMERA_ENABLED" -eq 1 ]]; then
      if [[ "$QUALIFICATION_CAPTURE" -eq 1 ]]; then
        if ! write_qualification_capture_record "$step_dir" >> "$RUN_LOG" 2>&1; then
          finish 'FAIL (qualification capture): could not retain the blind-capture record' 2
        fi
        printf '[bench] qualification capture: camera pixels retained unread; automatic pixel readers disabled\n'
      else
        run_counter_check "$step_dir"
        run_encounter_check "$step_dir"
      fi
    fi
    continue
  fi
  if [[ "$result" == "COLLECTION_ONLY" && "$runner_status" -eq 1 ]]; then
    if ! print_window_summary "$step_dir/window_result.json" "$suite"; then
      printf '%s: external evidence summary unavailable\n' "$suite" >> "$RUN_LOG"
    fi
    COLLECTION_ONLY=1
    COLLECTION_ONLY_REASON="$reason"
    if [[ "$suite" == "replay" && "$CAMERA_ENABLED" -eq 1 \
          && "$QUALIFICATION_CAPTURE" -eq 0 ]]; then
      run_counter_check "$step_dir"
      run_encounter_check "$step_dir"
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

if [[ "$COLLECTION_ONLY" -eq 1 ]]; then
  finish "COLLECTION-ONLY (unqualified: $COLLECTION_ONLY_REASON)" 1
fi
if [[ "$CAMERA_REQUESTED" -eq 1 && "$CAMERA_ENABLED" -eq 0 ]]; then
  if [[ "$QUALIFICATION_CAPTURE" -eq 1 ]]; then
    finish 'FAIL (qualification capture): requested camera is unavailable' 2
  fi
  finish 'PASS-PARTIAL (skipped: camera unplugged)' 1
fi
if [[ "$QUALIFICATION_CAPTURE" -eq 1 ]]; then
  finish 'QUALIFICATION-CAPTURED (pixels withheld; visible product NOT_EVALUATED)' 0
fi
if [[ "$CAMERA_REQUESTED" -eq 1 ]]; then
  finish_encounter_product
fi
finish 'PASS' 0
