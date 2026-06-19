#!/usr/bin/env bash
#
# run_device_tests.sh — Run the device test suite on connected ESP32-S3.
#
# Usage:
#   ./scripts/run_device_tests.sh              # All device-only suites (safe set)
#   ./scripts/run_device_tests.sh --quick      # Core suites only (boot + heap safe)
#   ./scripts/run_device_tests.sh --full       # Device + shared native suites (safe set)
#   ./scripts/run_device_tests.sh --stress     # Include stress suites
#
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

MODE="device"
RUN_STRESS=0
SUITE_COOLDOWN_SECONDS="${DEVICE_SUITE_COOLDOWN_SECONDS:-5}"
COMPARE_TO_MANIFESTS=()
OUT_DIR_OVERRIDE=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --quick)
      MODE="quick"
      ;;
    --full)
      MODE="full"
      ;;
    --stress)
      RUN_STRESS=1
      ;;
    --cooldown-seconds)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --cooldown-seconds" >&2
        exit 2
      fi
      SUITE_COOLDOWN_SECONDS="$2"
      shift
      ;;
    --compare-to)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --compare-to" >&2
        exit 2
      fi
      COMPARE_TO_MANIFESTS+=("$2")
      shift
      ;;
    --out-dir)
      if [[ $# -lt 2 ]]; then
        echo "Missing value for --out-dir" >&2
        exit 2
      fi
      OUT_DIR_OVERRIDE="$2"
      shift
      ;;
    -h|--help)
      echo "Usage: $0 [--quick | --full] [--stress] [--cooldown-seconds N] [--compare-to PATH ...] [--out-dir PATH]"
      echo ""
      echo "Modes:"
      echo "  (default)  Run all device-only test suites (safe set)"
      echo "  --quick    Run core suites only (boot + heap safe)"
      echo "  --full     Run device suites PLUS shared native suites (safe set)"
      echo "Options:"
      echo "  --stress   Include stress suites (can destabilize fragile hardware)"
      echo "  --cooldown-seconds N"
      echo "             Wait N seconds between suites (default: ${DEVICE_SUITE_COOLDOWN_SECONDS:-5})"
      echo "  --compare-to PATH"
      echo "             Compare this run against a prior manifest.json (repeat for a baseline window)"
      echo "  --out-dir PATH"
      echo "             Write all run artifacts to PATH"
      echo ""
      echo "Environment:"
      echo "  DEVICE_SUITE_COOLDOWN_SECONDS"
      echo "             Default cooldown if --cooldown-seconds is not provided"
      exit 0
      ;;
    *)
      echo "Usage: $0 [--quick | --full] [--stress] [--cooldown-seconds N] [--compare-to PATH ...] [--out-dir PATH]" >&2
      echo "" >&2
      echo "Unknown option: $1" >&2
      exit 2
      ;;
  esac
  shift
done

if ! [[ "$SUITE_COOLDOWN_SECONDS" =~ ^[0-9]+$ ]]; then
  echo "Invalid cooldown value '$SUITE_COOLDOWN_SECONDS' (expected integer seconds)." >&2
  exit 2
fi

PIO_CMD="${PIO_CMD:-pio}"
if ! command -v "$PIO_CMD" >/dev/null 2>&1; then
  echo "PlatformIO is required but not found in PATH." >&2
  exit 1
fi
source "$ROOT_DIR/scripts/platformio_ca_bundle.sh"
export PIO_CMD SSL_CERT_FILE REQUESTS_CA_BUNDLE

PIO_TEST_VERBOSITY="${PIO_TEST_VERBOSITY:--vvv}"

# If DEVICE_PORT is set, keep using that exact path.
TEST_PORT="${DEVICE_PORT:-}"
PORT_LOCKED=0
if [[ -n "$TEST_PORT" ]]; then
  PORT_LOCKED=1
fi

detect_usb_port() {
  local detected=""

  shopt -s nullglob
  local usb_ports=(
    /dev/cu.usbmodem*
    /dev/tty.usbmodem*
    /dev/ttyACM*
    /dev/ttyUSB*
    /dev/cu.usbserial*
    /dev/cu.SLAB_USBtoUART*
    /dev/tty.SLAB_USBtoUART*
  )
  shopt -u nullglob

  if [[ ${#usb_ports[@]} -gt 0 ]]; then
    detected="${usb_ports[0]}"
  else
    detected="$("$PIO_CMD" device list | awk '/^\/dev\// {print $1}' \
      | grep -E 'usbmodem|ttyACM|ttyUSB|usbserial|SLAB_USBtoUART' \
      | head -n1 || true)"
  fi

  if [[ -n "$detected" ]]; then
    echo "$detected"
    return 0
  fi

  return 1
}

wait_for_port() {
  local port="$1"
  local timeout_s="${2:-20}"
  local elapsed=0

  while (( elapsed < timeout_s )); do
    if [[ -e "$port" ]]; then
      return 0
    fi
    sleep 1
    elapsed=$((elapsed + 1))
  done

  return 1
}

resolve_test_port() {
  if [[ "$PORT_LOCKED" -eq 1 ]]; then
    echo "$TEST_PORT"
    return 0
  fi

  detect_usb_port || return 1
}

ensure_port_unlocked() {
  local port="$1"

  if ! command -v lsof >/dev/null 2>&1; then
    return 0
  fi

  local pids
  pids="$(lsof -t "$port" 2>/dev/null | tr '\n' ' ' | xargs || true)"
  if [[ -n "$pids" ]]; then
    echo "Serial port '$port' is currently in use by another process." >&2
    echo "Close the serial monitor (or kill the process) and retry." >&2
    echo "Port owner(s):" >&2
    # shellcheck disable=SC2086
    ps -o pid=,command= -p $pids >&2 || true
    return 1
  fi

  return 0
}

if [[ -z "$TEST_PORT" ]]; then
  TEST_PORT="$(detect_usb_port || true)"
fi

if [[ -z "$TEST_PORT" ]]; then
  echo "No USB serial device detected for hardware tests." >&2
  echo "Connect the ESP32-S3 in bootloader mode and retry." >&2
  echo "Or set DEVICE_PORT explicitly, for example:" >&2
  echo "  DEVICE_PORT=/dev/cu.usbmodemXXXX ./scripts/run_device_tests.sh --quick" >&2
  exit 1
fi

if [[ "$PORT_LOCKED" -eq 1 ]]; then
  echo "==> Using fixed device port: $TEST_PORT"
else
  echo "==> Auto-detected initial device port: $TEST_PORT"
fi

timestamp="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="$OUT_DIR_OVERRIDE"
if [[ -z "$OUT_DIR" ]]; then
  OUT_DIR="$ROOT_DIR/.artifacts/test_reports/device_$timestamp"
fi
mkdir -p "$OUT_DIR"
MAIN_LOG="$OUT_DIR/device.log"
METRICS_NDJSON="$OUT_DIR/metrics.ndjson"
MANIFEST_JSON="$OUT_DIR/manifest.json"
SCORING_JSON="$OUT_DIR/scoring.json"
SUMMARY_MD="$OUT_DIR/summary.md"
SUITE_INDEX_TSV="$OUT_DIR/suite_index.tsv"
: > "$MAIN_LOG"
: > "$METRICS_NDJSON"
printf "suite\tstatus\tjson\txml\tlog\tmetric_count\n" > "$SUITE_INDEX_TSV"

GIT_SHA="$(git rev-parse --short=7 HEAD 2>/dev/null || echo unknown)"
GIT_REF="$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)"
RUN_ID="device_${timestamp}_${GIT_SHA}"
BOARD_ID="${DEVICE_BOARD_ID:-unknown}"
LANE="device-tests"
STRESS_CLASS="core"
if [[ "$RUN_STRESS" -eq 1 ]]; then
  STRESS_CLASS="stress"
fi

# ─── Define suite groups ─────────────────────────────────────────────

# Core device suites (boot must be first — validates board is alive)
CORE_SUITES=(
  test_device_boot
  test_device_heap
)

# Memory & concurrency suites
MEMORY_SUITES=(
  test_device_psram
  test_device_freertos
  test_device_event_bus
)

# Dependent system suites
DEPENDENT_SUITES=(
  test_device_nvs
  test_device_battery
  test_device_coexistence
)

# Stress suites (run only with --stress)
STRESS_SUITES=(
  test_device_heap_stress
)

# Shared native suites that compile on device (self-contained, have setup/loop,
# no mock headers, no ../../src/modules/ includes).
# NOTE: These are not yet validated on hardware. Run individually first before
# adding to automated suite. Use: pio test -e device -f test_<name>
# SHARED_SUITES=(
#   test_battery_manager
#   test_ble_client
#   test_display
#   test_packet_parser
#   test_settings
#   test_wifi_manager
# )

# ─── Build suite list ────────────────────────────────────────────────

SUITES=()
case "$MODE" in
  quick)
    SUITES=("${CORE_SUITES[@]}")
    echo "==> Quick mode: core suites only (safe)"
    ;;
  device)
    SUITES=("${CORE_SUITES[@]}" "${MEMORY_SUITES[@]}" "${DEPENDENT_SUITES[@]}")
    echo "==> Device mode: all device-only suites (safe)"
    ;;
  full)
    # All device suites (same as default for now — shared native suites need
    # individual validation before adding to automated run)
    SUITES=("${CORE_SUITES[@]}" "${MEMORY_SUITES[@]}" "${DEPENDENT_SUITES[@]}")
    echo "==> Full mode: all device-compatible suites (safe)"
    ;;
esac

if [[ "$RUN_STRESS" -eq 1 ]]; then
  SUITES=("${SUITES[@]}" "${STRESS_SUITES[@]}")
  echo "==> Stress mode enabled: adding stress suites"
fi

echo "==> Planned suite order:"
for suite in "${SUITES[@]}"; do
  echo "    - $suite"
done
echo "==> Inter-suite cooldown: ${SUITE_COOLDOWN_SECONDS}s"

# ─── Summarize JSON results ──────────────────────────────────────────

summarize_json() {
  local json_path="$1"
  python3 - "device" "$json_path" <<'PY'
import json
import sys

env_name = sys.argv[1]
json_path = sys.argv[2]

with open(json_path, "r", encoding="utf-8") as f:
    data = json.load(f)

suites = [
    s for s in data.get("test_suites", [])
    if s.get("env_name") == env_name and s.get("status") != "SKIPPED"
]

suite_count = len(suites)
test_count = sum(int(s.get("testcase_nums", 0)) for s in suites)
pass_count = sum(int(s.get("pass_nums", 0)) for s in suites)
failure_count = sum(int(s.get("failure_nums", 0)) for s in suites)
error_count = sum(int(s.get("error_nums", 0)) for s in suites)
duration_s = sum(float(s.get("duration", 0.0)) for s in suites)

# Infra errors (serial disconnect, timeout) with all assertions passing = PASS
if failure_count > 0:
    status = "FAIL"
elif error_count > 0 and pass_count == test_count and test_count > 0:
    status = "PASS"
elif error_count > 0:
    status = "FAIL"
else:
    status = "PASS"

print(f"\n{'='*60}")
print(f"  Device Test Summary: {status}")
print(f"  Suites: {suite_count}  Tests: {test_count}"
      f"  Failures: {failure_count}  Errors: {error_count}")
print(f"  Duration: {duration_s:.3f}s")
print(f"{'='*60}\n")

if failure_count > 0:
    for s in suites:
        if int(s.get("failure_nums", 0)) > 0:
            print(f"  FAILED: {s.get('test_suite_name', 'unknown')}")
    print()
    sys.exit(1)

if error_count > 0:
    for s in suites:
        if int(s.get("error_nums", 0)) > 0:
            print(f"  INFRA ERROR: {s.get('test_suite_name', 'unknown')} (all assertions passed)")
    print()
PY
}

extract_suite_metrics() {
  local suite="$1"
  local suite_log="$2"
  local metric_count

  if ! metric_count="$(python3 "$ROOT_DIR/tools/extract_device_metrics.py" \
      "$suite_log" "$METRICS_NDJSON" \
      --run-id "$RUN_ID" \
      --git-sha "$GIT_SHA" \
      --suite "$suite" 2>>"$MAIN_LOG")"; then
    echo "Failed to extract structured metrics from suite '$suite'." >&2
    return 1
  fi

  echo "${metric_count:-0}"
}

write_manifest() {
  local base_result="$1"
  python3 - "$MANIFEST_JSON" "$SUITE_INDEX_TSV" "$RUN_ID" "$GIT_SHA" "$GIT_REF" "$BOARD_ID" "$STRESS_CLASS" "$base_result" <<'PY'
import csv
import json
import sys
from pathlib import Path

manifest_path = Path(sys.argv[1])
index_path = Path(sys.argv[2])
run_id = sys.argv[3]
git_sha = sys.argv[4]
git_ref = sys.argv[5]
board_id = sys.argv[6]
stress_class = sys.argv[7]
base_result = sys.argv[8]

suite_rows = []
tracks = []
with index_path.open("r", encoding="utf-8") as handle:
    reader = csv.DictReader(handle, delimiter="\t")
    for row in reader:
        suite_rows.append(row)
        if row.get("suite"):
            tracks.append(row["suite"])

payload = {
    "schema_version": 1,
    "run_id": run_id,
    "timestamp_utc": __import__("datetime").datetime.utcnow().replace(microsecond=0).isoformat() + "Z",
    "git_sha": git_sha,
    "git_ref": git_ref,
    "run_kind": "device_suite",
    "board_id": board_id,
    "env": "device",
    "lane": "device-tests",
    "suite_or_profile": "device_suite_collection",
    "stress_class": stress_class,
    "result": base_result,
    "base_result": base_result,
    "metrics_file": "metrics.ndjson",
    "scoring_file": "scoring.json",
    "tracks": tracks,
    "suite_results": suite_rows,
}

manifest_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
PY
}

score_manifest() {
  local scorer_status=0
  local compare_args=()
  local compare_to=""

  if [[ "${#COMPARE_TO_MANIFESTS[@]}" -gt 0 ]]; then
    for compare_to in "${COMPARE_TO_MANIFESTS[@]}"; do
      compare_args+=(--compare-to "$compare_to")
    done
  fi

  set +e
  python3 "$ROOT_DIR/tools/score_hardware_run.py" \
    "$MANIFEST_JSON" \
    --catalog "$ROOT_DIR/tools/hardware_metric_catalog.json" \
    ${compare_args[@]+"${compare_args[@]}"} \
    --json > "$SCORING_JSON"
  scorer_status=$?
  set -e

  if [[ "$scorer_status" -le 2 ]]; then
    set +e
    python3 "$ROOT_DIR/tools/score_hardware_run.py" \
      "$MANIFEST_JSON" \
      --catalog "$ROOT_DIR/tools/hardware_metric_catalog.json" \
      ${compare_args[@]+"${compare_args[@]}"} > "$SUMMARY_MD"
    set -e

    python3 - "$MANIFEST_JSON" "$SCORING_JSON" <<'PY'
import json
import sys
from pathlib import Path

manifest_path = Path(sys.argv[1])
scoring_path = Path(sys.argv[2])
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
scoring = json.loads(scoring_path.read_text(encoding="utf-8"))
manifest["result"] = scoring["result"]
manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
PY
  else
    cat > "$SUMMARY_MD" <<EOF
# Hardware Run Score

- Result: **ERROR**
- Scoring failed before a comparison summary could be generated.
EOF
  fi

  return "$scorer_status"
}

# ─── Run tests ────────────────────────────────────────────────────────

run_suite() {
  local suite="$1"
  local index="$2"
  local total="$3"
  local run_port

  # After a previous test, give USB CDC time to stabilize before re-detecting
  if (( index > 1 )) && (( SUITE_COOLDOWN_SECONDS > 0 )); then
    echo "    Waiting ${SUITE_COOLDOWN_SECONDS}s for USB CDC port to stabilize..."
    sleep "$SUITE_COOLDOWN_SECONDS"
  fi

  # Re-detect port each time (USB CDC may change name after reset)
  run_port="$(resolve_test_port || true)"
  if [[ -z "$run_port" ]]; then
    # Port not found immediately — wait up to 15s for it to reappear
    echo "    Port not found, waiting up to 15s for USB CDC re-enumeration..."
    local waited=0
    while (( waited < 15 )); do
      sleep 1
      waited=$((waited + 1))
      run_port="$(detect_usb_port || true)"
      if [[ -n "$run_port" ]]; then
        break
      fi
    done
    if [[ -z "$run_port" ]]; then
      echo "Unable to locate a USB serial device before suite '$suite'." >&2
      echo "Device may need manual reset (BOOT+RESET buttons)." >&2
      return 1
    fi
  fi

  if ! wait_for_port "$run_port" 20; then
    echo "Timed out waiting for port '$run_port' before suite '$suite'." >&2
    return 1
  fi

  if ! ensure_port_unlocked "$run_port"; then
    return 1
  fi

  echo ""
  echo "==> [$index/$total] Running $suite on $run_port"

  local suite_json="$OUT_DIR/${suite}.json"
  local suite_xml="$OUT_DIR/${suite}.xml"
  local suite_log="$OUT_DIR/${suite}.log"
  local suite_status="PASS"
  local metric_count="0"

  set +e
  "$PIO_CMD" test -e device -f "$suite" \
    --upload-port "$run_port" \
    --test-port "$run_port" \
    --json-output-path "$suite_json" \
    --junit-output-path "$suite_xml" "$PIO_TEST_VERBOSITY" | tee "$suite_log"
  local cmd_status=${PIPESTATUS[0]}
  set -e

  cat "$suite_log" >> "$MAIN_LOG"
  metric_count="$(extract_suite_metrics "$suite" "$suite_log")" || return 1

  if [[ $cmd_status -ne 0 ]]; then
    # Check JSON: if all test assertions passed, this is an infra error
    # (e.g. serial disconnect during teardown), not a real test failure.
    local tests_all_passed
    tests_all_passed="$(python3 -c "
import json, sys
try:
    with open(sys.argv[1]) as f:
        data = json.load(f)
    suites = [s for s in data.get('test_suites', []) if s.get('env_name') == 'device' and s.get('status') != 'SKIPPED']
    total = sum(int(s.get('testcase_nums', 0)) for s in suites)
    fails = sum(int(s.get('failure_nums', 0)) for s in suites)
    passes = sum(int(s.get('pass_nums', 0)) for s in suites)
    print('yes' if total > 0 and fails == 0 and passes == total else 'no')
except Exception:
    print('no')
" "$suite_json" 2>/dev/null || echo "no")"

    if [[ "$tests_all_passed" != "yes" ]]; then
      suite_status="FAIL"
      printf "%s\t%s\t%s\t%s\t%s\t%s\n" \
        "$suite" "$suite_status" "$suite_json" "$suite_xml" "$suite_log" "$metric_count" >> "$SUITE_INDEX_TSV"
      echo "" >&2
      echo "Suite '$suite' failed (exit $cmd_status)." >&2
      echo "Last 40 log lines:" >&2
      tail -n 40 "$suite_log" >&2 || true
      return "$cmd_status"
    fi
    echo "" >&2
    echo "Warning: Suite '$suite' exited $cmd_status but all assertions passed (infra error)." >&2
  fi

  printf "%s\t%s\t%s\t%s\t%s\t%s\n" \
    "$suite" "$suite_status" "$suite_json" "$suite_xml" "$suite_log" "$metric_count" >> "$SUITE_INDEX_TSV"

  summarize_json "$suite_json"
}

failed_suite=""
total_suites="${#SUITES[@]}"
suite_index=1

for suite in "${SUITES[@]}"; do
  if ! run_suite "$suite" "$suite_index" "$total_suites"; then
    failed_suite="$suite"
    break
  fi
  suite_index=$((suite_index + 1))
done

base_result="PASS"
if [[ -n "$failed_suite" ]]; then
  base_result="FAIL"
fi
write_manifest "$base_result"
scorer_exit=0
set +e
score_manifest
scorer_exit=$?
set -e

if [[ -n "$failed_suite" ]]; then
  echo "" >&2
  echo "Device test run stopped at: $failed_suite" >&2
  echo "Reports written to: $OUT_DIR"
  echo "Manifest: $MANIFEST_JSON"
  echo "Summary: $SUMMARY_MD"
  exit 1
fi

echo ""
echo "All requested device suites passed."
echo "Reports written to: $OUT_DIR"
echo "Manifest: $MANIFEST_JSON"
echo "Summary: $SUMMARY_MD"

if [[ "$scorer_exit" -ge 2 ]]; then
  exit 1
fi
