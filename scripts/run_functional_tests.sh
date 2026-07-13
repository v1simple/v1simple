#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

WITH_DEVICE=0
if [[ "${1:-}" == "--with-device" ]]; then
  WITH_DEVICE=1
  shift
fi

if [[ $# -ne 0 ]]; then
  echo "Usage: $0 [--with-device]" >&2
  exit 2
fi

PIO_CMD="${PIO_CMD:-pio}"
if ! command -v "$PIO_CMD" >/dev/null 2>&1; then
  echo "PlatformIO is required but not found in PATH." >&2
  exit 1
fi
source "$ROOT_DIR/scripts/platformio_ca_bundle.sh"
export PIO_CMD SSL_CERT_FILE REQUESTS_CA_BUNDLE

timestamp="$(date +%Y%m%d_%H%M%S)"
OUT_DIR="$ROOT_DIR/.artifacts/test_reports/functional_$timestamp"
mkdir -p "$OUT_DIR"

FILTERS=(
  -f test_wifi_boot_policy
  -f test_wifi_manager
  # Connection/alert scenario coverage so the "functional" gate is not
  # WiFi-only (2026-07-09 review): end-to-end packet-stream parsing,
  # connection-cycle invariants, and display correctness traces.
  -f test_packet_parser_stream
  -f test_connection_cycle_coordinator_invariants
  -f test_display_correctness_trace
)

summarize_json() {
  local env_name="$1"
  local json_path="$2"
  python3 - "$env_name" "$json_path" <<'PY'
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
failure_count = sum(int(s.get("failure_nums", 0)) for s in suites)
error_count = sum(int(s.get("error_nums", 0)) for s in suites)
duration_s = sum(float(s.get("duration", 0.0)) for s in suites)

print(
    f"[{env_name}] suites={suite_count} tests={test_count} "
    f"failures={failure_count} errors={error_count} duration={duration_s:.3f}s"
)
PY
}

run_env() {
  local env_name="$1"
  local json_path="$OUT_DIR/${env_name}.json"
  local junit_path="$OUT_DIR/${env_name}.xml"
  local log_path="$OUT_DIR/${env_name}.log"

  echo "==> Running functional tests in env '${env_name}'"
  "$PIO_CMD" test -e "$env_name" "${FILTERS[@]}" \
    --json-output-path "$json_path" \
    --junit-output-path "$junit_path" | tee "$log_path"

  summarize_json "$env_name" "$json_path"
}

run_env native

if [[ "$WITH_DEVICE" -eq 1 ]]; then
  run_env device
fi

echo "Reports written to: $OUT_DIR"
