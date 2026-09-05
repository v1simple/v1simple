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
CAMERA_REQUESTED=0
FLASH=1
COLLECTION_ONLY=0
COLLECTION_ONLY_REASON=""
QUALIFICATION_CAPTURE=0
COUNTER_RESULT="NOT_EVALUATED"
COUNTER_PRINTED=0
ENCOUNTER_RESULT="NOT_EVALUATED"
ENCOUNTER_PRINTED=0
ENCOUNTER_REASON=""

usage() {
  printf 'Usage: ./bench.sh --all|--replay [--camera] [--no-flash] [--qualification-capture]\n'
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
    --camera)
      CAMERA_REQUESTED=1
      ;;
    --no-flash)
      FLASH=0
      ;;
    --qualification-capture)
      QUALIFICATION_CAPTURE=1
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
if [[ "$QUALIFICATION_CAPTURE" -eq 1 \
      && ("$RUN_REPLAY" -ne 1 || "$RUN_ALL" -ne 0 || "$CAMERA_REQUESTED" -ne 1) ]]; then
  fail_usage
fi
[[ "$DURATION_SECONDS" =~ ^[1-9][0-9]*$ ]] || fail_usage
[[ "$REPLAY_DURATION_SECONDS" =~ ^[1-9][0-9]*$ ]] || fail_usage
[[ "$POST_UPLOAD_SETTLE_SECONDS" =~ ^[0-9]+$ ]] || fail_usage
if [[ "$CAMERA_REQUESTED" -eq 1 && "$QUALIFICATION_CAPTURE" -eq 0 ]]; then
  ENCOUNTER_RESULT="INCONCLUSIVE"
  ENCOUNTER_REASON="requested camera evidence is unavailable"
fi

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
  if [[ "$COUNTER_PRINTED" -eq 0 ]]; then
    printf '[bench] sampled live counter: %s\n' "$COUNTER_RESULT"
    COUNTER_PRINTED=1
  fi
  if [[ "$ENCOUNTER_PRINTED" -eq 0 ]]; then
    printf '[bench] visible encounter product: %s' "$ENCOUNTER_RESULT"
    [[ -n "$ENCOUNTER_REASON" ]] && printf ' | %s' "$ENCOUNTER_REASON"
    printf '\n'
    ENCOUNTER_PRINTED=1
  fi
  if ! publish_latest; then
    verdict="FAIL (collection): could not update the latest evidence link"
    status=2
  fi
  printf '%s\n' "$verdict"
  exit "$status"
}

if [[ "$GIT_WORKTREE_CLEAN" -ne 1 ]]; then
  printf 'qualification: source worktree is dirty; exact source identity is unavailable\n' >> "$RUN_LOG"
  finish 'FAIL (qualification): source worktree is dirty; commit or remove relevant changes before bench qualification' 2
fi

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
    or payload.get("qualification_reason")
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
  python3 - "$1" <<'PY'
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
  python3 "$ROOT_DIR/scripts/bench/counter_check.py" \
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
  python3 - "$1" "$2" "$SIGNALLED" <<'PY'
import json
import math
import re
import sys
from pathlib import Path

result, tally, required, requests, selected, decoded, gap, first_id, reason = (
    "INCONCLUSIVE", "counts unavailable", "?", "?", "?", "?", "?", "-", ""
)
try:
    payload = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
    counts, coverage = payload["counts"], payload["coverage"]
    fields, joint = counts["fields"], counts["joint_states"]
    samples, errors = payload["samples"], payload["errors"]
    required, requests = counts["required"], coverage["requests"]
    selected, decoded = coverage["selected_unique_frames"], coverage["unique_frames"]
    allowed = {"MATCH", "DIFFERENCE", "UNRESOLVED", "CONDITIONAL", "PREVIOUS_INPUT_STATE", "TRANSITION_DIFFERENCE"}
    if (payload["kind"] != "sampled_encounter_check"
            or payload["result"] not in ("PASS", "FAIL", "INCONCLUSIVE")
            or not isinstance(fields, dict) or not isinstance(joint, dict)
            or not set(fields).issubset(allowed)
            or any(type(n) is not int or n < 0 for n in [required, requests, selected, decoded, *fields.values(), *joint.values()])
            or required != requests * 7 or sum(fields.values()) != required
            or not 0 <= decoded <= selected <= requests
            or not isinstance(samples, list) or len(samples) != requests
            or not isinstance(errors, list)):
        raise ValueError("invalid encounter result or coverage")
    sample_ids = {str(sample.get("frame_id")) for sample in samples if isinstance(sample, dict)}
    if (len(sample_ids) != len(samples)
            or any(re.fullmatch(r"[A-Za-z0-9_-]+", value) is None for value in sample_ids)):
        raise ValueError("invalid or duplicate encounter sample identity")
    raw_computed = ("FAIL" if fields.get("DIFFERENCE", 0) or joint.get("DIFFERENCE", 0) else
                "INCONCLUSIVE" if errors or not required or decoded != selected
                or fields.get("MATCH", 0) != required
                or any(n for status, n in joint.items() if status not in ("MATCH", "NOT_EVALUATED")) else "PASS")
    if payload.get("raw_frame_result") != raw_computed:
        raise ValueError("raw encounter result disagrees with its required evidence")

    product = payload["primary_judgment"]
    qualification = payload["reader_qualification"]
    if (not isinstance(product, dict)
            or product.get("kind") != "visible_event_presentation"
            or product.get("result") not in ("PASS", "FAIL", "INCONCLUSIVE")
            or not isinstance(product.get("events"), list)
            or not isinstance(product.get("counts"), dict)
            or not isinstance(product.get("execution"), dict)
            or not isinstance(qualification, dict)
            or qualification.get("status") not in ("QUALIFIED", "REJECTED")):
        raise ValueError("visible-event product judgment is malformed")
    contract = product.get("contract", {})
    if (contract.get("id") != "VISIBLE_EVENT_PRESENTATION" or contract.get("version") != 2
            or contract.get("appearance_deadline_ns") != 100_000_000
            or contract.get("appearance_decision_rule") != "first_source_marker_at_or_after_nominal_deadline"
            or contract.get("maximum_appearance_observation_bracket_ns") != 10_000_000
            or contract.get("verification_duration_ns") != 192_000_000
            or contract.get("maximum_source_marker_gap_ns") != 10_000_000
            or contract.get("minimum_post_completion_hold_ns") != 312_000_000
            or contract.get("clock") != "host_monotonic_capture_marker"
            or contract.get("anchor") != "first_complete_target_input_all_accepted_ns"):
        raise ValueError("visible-event product contract is not the supported exact policy")
    event_results = [event.get("result") for event in product["events"] if isinstance(event, dict)]
    if len(event_results) != len(product["events"]) or any(
            value not in ("PASS", "FAIL", "INCONCLUSIVE") for value in event_results):
        raise ValueError("visible-event product events are malformed")
    product_counts = {"required_events": len(event_results),
                      "passed": event_results.count("PASS"),
                      "failed": event_results.count("FAIL"),
                      "inconclusive": event_results.count("INCONCLUSIVE")}
    if product["counts"] != product_counts:
        raise ValueError("visible-event product counts disagree with its events")
    fatal = product["execution"].get("fatal_integrity_errors")
    classification_errors = product["execution"].get("temporal_classification_errors")
    if (not isinstance(fatal, list) or not isinstance(classification_errors, list)
            or any(not isinstance(value, (str, dict)) for value in [*fatal, *classification_errors])):
        raise ValueError("visible-event product integrity errors are malformed")
    if fatal:
        computed, computed_reason = "INCONCLUSIVE", "FATAL_EVIDENCE_INTEGRITY"
    elif not event_results:
        computed, computed_reason = "INCONCLUSIVE", "NO_REQUIRED_EVENTS"
    elif product_counts["failed"]:
        first = next(event for event in product["events"] if event["result"] == "FAIL")
        computed, computed_reason = "FAIL", first.get("reason_code")
    elif product_counts["inconclusive"] or classification_errors:
        first = next((event for event in product["events"] if event["result"] == "INCONCLUSIVE"), None)
        computed, computed_reason = ("INCONCLUSIVE",
                                     first.get("reason_code") if first else "INVALID_TEMPORAL_CLASSIFICATION")
    else:
        computed, computed_reason = "PASS", "ALL_REQUIRED_EVENTS_PASSED"
    if payload["result"] != computed or product["result"] != computed \
            or product.get("reason_code") != computed_reason:
        raise ValueError("visible-event product verdict disagrees with its required evidence")
    if computed in ("PASS", "FAIL") and qualification.get("status") != "QUALIFIED":
        raise ValueError("unqualified reader produced a decisive product verdict")
    if qualification.get("status") != "QUALIFIED" and not any(
            isinstance(value, str) and value.startswith("Reader qualification:") for value in fatal):
        raise ValueError("reader qualification failure is absent from product integrity")
    result = computed
    required = product_counts["required_events"]
    tally = (f"{product_counts['passed']} passed, {product_counts['failed']} failed, "
             f"{product_counts['inconclusive']} inconclusive")
    gaps = [region["maximum_unobserved_gap_seconds"] for region in coverage["regions"]]
    if any(type(value) not in (int, float) or not math.isfinite(value) or value < 0 for value in gaps):
        raise ValueError("invalid observed coverage gaps")
    gap = f"{max(gaps):.3f}s" if gaps else "unavailable"
    if fatal:
        reason = str(fatal[0])
    for event in product["events"] if reason == "" else []:
        if event["result"] == "PASS":
            continue
        reason = f"{event.get('event_id', 'event')}: {event.get('reason_code', event['result'])}"
        point = event.get("first_decisive_marker") or event.get("first_current_correct")
        if isinstance(point, dict) and isinstance(point.get("frame_id"), str):
            first_id = point["frame_id"]
            if first_id not in sample_ids:
                raise ValueError("product evidence references an unknown sample")
        break
    for sample in samples if reason == "" else []:
        checks = sample.get("comparison", {}).get("checks", {})
        issues = [(name, check) for name, check in checks.items() if check.get("status") != "MATCH"]
        joint_check = sample.get("comparison", {}).get("joint_state", {})
        if joint_check.get("status") not in (None, "MATCH", "NOT_EVALUATED"):
            issues.append(("joint display", joint_check))
        if issues:
            name, check = issues[0]
            first_id = str(sample["frame_id"])
            if not re.fullmatch(r"[A-Za-z0-9_-]+", first_id):
                raise ValueError("invalid sample identity")
            reason = f"sample {first_id}, {name}: {check.get('reason') or check.get('status')}"
            break
    if errors:
        reason = str(errors[0])
    if (result, int(sys.argv[2])) not in (("PASS", 0), ("FAIL", 1), ("INCONCLUSIVE", 2)):
        result, reason = "INCONCLUSIVE", f"analysis exit {sys.argv[2]} did not match its retained result"
    if sys.argv[3] == "1" and result == "PASS":
        result, reason = "INCONCLUSIVE", "analysis interrupted; completion was not established"
except (OSError, KeyError, TypeError, ValueError, AttributeError, json.JSONDecodeError):
    result, tally, required, requests, selected, decoded, gap, first_id, reason = (
        "INCONCLUSIVE", "counts unavailable", "?", "?", "?", "?", "?", "-",
        f"analysis result is missing, unreadable or inconsistent (exit {sys.argv[2]}; see bench.log)"
    )
reason = " ".join(reason.split())[:240] or "-"
print("\t".join(map(str, (result, tally, required, requests, selected, decoded, gap, first_id, reason))))
PY
}

run_encounter_check() {
  local replay_dir="$1"
  local encounter_dir="$replay_dir/encounter-check"
  local encounter_status=0
  local tally="counts unavailable" required="?" requests="?" selected="?" decoded="?" gap="?" first_id="-" reason="-"
  if [[ "$SIGNALLED" -eq 1 ]]; then
    ENCOUNTER_REASON="analysis interrupted before the encounter check"
    return
  fi
  printf '[bench] visible encounter product: analyzing exact event windows and raw camera evidence...\n'
  python3 "$ROOT_DIR/scripts/bench/encounter_check.py" \
    --run-dir "$replay_dir" \
    --inspect-transitions \
    --reader-qualification "$ENCOUNTER_QUALIFICATION" \
    --out "$encounter_dir" \
    2>&1 | tee -a "$RUN_LOG" || encounter_status=$?
  IFS=$'\t' read -r ENCOUNTER_RESULT tally required requests selected decoded gap first_id reason \
    < <(read_encounter_result "$encounter_dir/result.json" "$encounter_status" 2>/dev/null)
  case "$ENCOUNTER_RESULT" in
    PASS|FAIL|INCONCLUSIVE) ;;
    *) ENCOUNTER_RESULT="INCONCLUSIVE"; reason="could not read the encounter result; see bench.log" ;;
  esac
  printf 'visible encounter product: result=%s exit=%s qualification=%s\n' \
    "$ENCOUNTER_RESULT" "$encounter_status" "$ENCOUNTER_QUALIFICATION" >> "$RUN_LOG"
  printf '[bench] visible encounter product: %s | %s / %s required visible events\n' \
    "$ENCOUNTER_RESULT" "$tally" "$required"
  printf '[bench] encounter coverage: %s requests | %s selected, %s decoded original frames | largest unobserved gap %s\n' \
    "$requests" "$selected" "$decoded" "$gap"
  [[ "$reason" == "-" ]] || printf '[bench] encounter attention: %s\n' "$reason"
  if [[ -f "$encounter_dir/report.html" ]]; then
    local fragment=""
    [[ "$first_id" == "-" ]] || fragment="#sample=$first_id"
    printf '[bench] encounter report: %s/report.html%s\n' "$encounter_dir" "$fragment"
  else
    printf '[bench] encounter report unavailable; see %s\n' "$RUN_LOG"
  fi
  ENCOUNTER_PRINTED=1
}

write_qualification_capture_record() {
  local replay_dir="$1"
  python3 - "$replay_dir/window_result.json" "$replay_dir/qualification_capture.json" \
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
  if [[ "$QUALIFICATION_CAPTURE" -eq 1 ]]; then
    args+=(--reader-qualification)
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
  case "$ENCOUNTER_RESULT" in
    PASS)
      finish 'PASS (visible encounter product)' 0
      ;;
    FAIL)
      finish 'FAIL (visible encounter product)' 2
      ;;
    INCONCLUSIVE|NOT_EVALUATED)
      finish 'INCONCLUSIVE (visible encounter product)' 1
      ;;
  esac
fi
finish 'PASS' 0
