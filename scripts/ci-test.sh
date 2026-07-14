#!/bin/bash
# Authoritative repo gate used locally and by GitHub workflows.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

START_TIME=$(date +%s)
PIO_JOBS="${PLATFORMIO_RUN_JOBS:-}"
if [[ -n "$PIO_JOBS" && ! "$PIO_JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo -e "${RED}Invalid PLATFORMIO_RUN_JOBS: $PIO_JOBS${NC}" >&2
  echo "Expected a positive integer." >&2
  exit 2
fi
PIO_BUILD_ARGS=(-e waveshare-349)
if [[ -n "$PIO_JOBS" ]]; then
  PIO_BUILD_ARGS+=(-j "$PIO_JOBS")
fi

section() {
  echo ""
  echo -e "${BLUE}== $1 ==${NC}"
}

run_step() {
  local label="$1"
  shift
  echo -e "${YELLOW}[run] ${label}${NC}"
  "$@"
  echo -e "${GREEN}[pass] ${label}${NC}"
}

run_advisory_step() {
  local label="$1"
  shift
  echo -e "${YELLOW}[advisory] ${label}${NC}"
  if "$@"; then
    echo -e "${GREEN}[pass] ${label}${NC}"
  else
    echo -e "${YELLOW}[warn] ${label} failed (advisory)${NC}"
  fi
}

run_firmware_build_with_memory_log() {
  local memory_dir="$ROOT_DIR/.artifacts/test_reports/memory-headroom"
  mkdir -p "$memory_dir"
  "$PIO_CMD" run "${PIO_BUILD_ARGS[@]}" | tee "$memory_dir/waveshare-349-build.log"
}

PIO_CMD="${PIO_CMD:-pio}"
if ! command -v "$PIO_CMD" >/dev/null 2>&1; then
  echo -e "${RED}PlatformIO not found in PATH.${NC}" >&2
  exit 1
fi
source "$ROOT_DIR/scripts/platformio_ca_bundle.sh"
export PIO_CMD SSL_CERT_FILE REQUESTS_CA_BUNDLE

echo "============================================"
echo "Authoritative Local CI Gate"
echo "============================================"

section "Toolchain"
run_step "PlatformIO Core version" python3 scripts/check_platformio_core_version.py --pio "$PIO_CMD"

section "Semantic Gates"
run_step "Bug pattern scanner" python3 scripts/check_bug_patterns.py
run_step "Bug pattern scanner regression tests" python3 scripts/test_bug_pattern_scanner.py
run_step "LittleFS image compatibility regression tests" python3 scripts/test_check_littlefs_image_compatibility.py
run_step "sdkconfig redefine guard (CONFIG_* -D vs framework header)" python3 scripts/check_sdkconfig_redefines.py
run_step "BLE deletion semantic guard" python3 scripts/check_ble_deletion_contract.py
run_step "Frontend HTTP resilience semantic guard" python3 scripts/check_frontend_http_resilience_contract.py
run_step "BLE hot-path semantic guard" python3 scripts/check_ble_hot_path_semantic_guard.py
run_step "Display flush semantic guard" python3 scripts/check_display_flush_semantic_guard.py
run_step "Display visual verifier synthetic tests" python3 -B scripts/test_display_visual_check.py
run_step "Main loop semantic guard" python3 scripts/check_main_loop_semantic_guard.py
run_step "Module const-correctness semantic guard" python3 scripts/check_module_const_correctness.py
run_step "Extern-escape semantic guard" python3 scripts/check_extern_escape.py
run_step "Header style contract" python3 scripts/check_header_style_contract.py
run_step "Modified font reserved-name contract" python3 scripts/check_modified_font_names.py
run_step "Retired ALP terms" python3 scripts/check_retired_alp_terms.py
run_step "Retired ALP terms regression tests" python3 scripts/test_retired_alp_terms.py
run_step "Stabilization manifest contract" python3 scripts/check_stabilization_manifest.py
run_step "Native linked-source manifest contract" python3 scripts/native_test_source_manifest.py --check
run_step "Native unit tests" python3 scripts/run_native_tests_serial.py
run_step "Native sanitized unit tests" python3 scripts/run_native_tests_serial.py --env native-sanitized
run_step "Native car-mode unit tests" python3 scripts/run_native_tests_serial.py --env native_car
run_step "Native linked-source pilot" python3 scripts/run_native_tests_serial.py --linked-pilot test_alp_event_latch
run_step "Native sanitized linked-source pilot" python3 scripts/run_native_tests_serial.py --env native-sanitized --linked-pilot test_alp_event_latch
run_step "Functional scenarios" ./scripts/run_functional_tests.sh

section "Critical Mutation Gate"
run_step "Tracked critical mutation catalog" ./scripts/mutation_test.sh --critical

section "Perf Scoring Gate"
run_step "Deterministic perf scorer regression tests" python3 scripts/test_perf_scoring.py
run_step "Perf computed metric contract" python3 scripts/check_perf_computed_metric_contract.py
run_step "Shared metric schema regression tests" python3 scripts/test_metric_schema.py
run_step "Hardware manifest scoring regression tests" python3 scripts/test_hardware_run_scoring.py
run_step "Perf CSV import regression tests" python3 scripts/test_perf_csv_import.py
run_step "Soak metrics parser regression tests" python3 scripts/test_soak_parse_metrics.py
run_step "Bench scorer regression tests" python3 scripts/test_bench_score.py
run_step "Device test runner regression tests" python3 scripts/test_run_device_tests_script.py
run_step "OBD/proxy qualification validator regression tests" python3 scripts/test_obd_proxy_qualification.py
run_step "Release evidence manifest regression tests" python3 scripts/test_release_evidence_manifest.py

section "Compatibility Guards"
run_step "WiFi API contracts" python3 scripts/check_wifi_api_contract.py
run_step "Reorder warning contract" python3 scripts/check_reorder_warning_contract.py
run_step "Quiet coordinator contract" python3 scripts/check_quiet_coordinator_contract.py
run_step "Connection cycle invariants contract" python3 scripts/check_connection_cycle_invariants.py
run_step "BLE hot-path snapshot contract" python3 scripts/check_ble_hot_path_contract.py
run_step "Perf CSV column contract" python3 scripts/check_perf_csv_column_contract.py
run_step "Perf schema referential contract" python3 scripts/check_perf_schema_contract.py
run_step "Display flush discipline contract" python3 scripts/check_display_flush_discipline_contract.py
run_step "Dirty flag discipline contract" python3 scripts/check_dirty_flag_discipline.py
run_step "Main loop call-order contract" python3 scripts/check_main_loop_call_order_contract.py
run_step "OBD boot-safety contract" python3 scripts/check_obd_boot_safety_contract.py
run_step "Extern/global usage contract" python3 scripts/check_extern_usage_contract.py
run_step "std::function usage contract" python3 scripts/check_std_function_usage_contract.py
run_step "LittleFS mount/tooling contract" python3 scripts/check_littlefs_mount_contract.py

section "Docs Hygiene"
run_step "Perf SLO doc/json contract" python3 scripts/check_perf_slo_contract.py
run_step "Build instruction contract" python3 scripts/check_build_instruction_contract.py
run_step "Build dist contract" python3 scripts/check_build_dist_contract.py
run_step "Workflow action pin contract" python3 scripts/check_workflow_action_pins.py
run_step "Release workflow flash contract" python3 scripts/check_release_workflow_flash_contract.py
run_step "Web installer page contract" python3 scripts/check_web_installer_page.py --site-dir web-installer --template-only
run_step "API docs source contract" python3 scripts/check_api_doc_sources.py
run_step "V1 protocol docs contract" python3 scripts/check_v1_protocol_docs_contract.py
run_step "ALP protocol docs contract" python3 scripts/check_alp_protocol_docs_contract.py
run_step "Protocol spec tables (doc ↔ fixtures)" python3 scripts/check_protocol_spec_tables.py

section "Frontend"
cd interface
run_step "Frontend dependencies" bash -lc "npm ci --silent 2>/dev/null || npm install --silent"
run_step "Frontend lint/type checks" npm run lint
run_step "Frontend unit tests with coverage" npm run test:coverage
run_step "Frontend build" npm run build
run_step "Frontend deploy" npm run deploy:built
cd "$ROOT_DIR"

section "Frontend Packaging"
run_step "Web asset guardrails" python3 scripts/check_web_asset_budget.py
run_step "Audio asset manifest (source + deployed)" python3 scripts/check_audio_asset_manifest.py

section "Firmware Build"
run_step "Firmware static analysis" ./scripts/pio-check.sh
run_step "Firmware clean" "$PIO_CMD" run "${PIO_BUILD_ARGS[@]}" -t clean
run_step "Firmware build" run_firmware_build_with_memory_log
run_step "Firmware memory headroom" python3 scripts/check_memory_headroom.py \
  --env waveshare-349 \
  --no-build \
  --build-log .artifacts/test_reports/memory-headroom/waveshare-349-build.log \
  --warn-iram-zero
run_step "LittleFS image build" "$PIO_CMD" run "${PIO_BUILD_ARGS[@]}" -t buildfs
run_step "Flash package truth report" python3 scripts/report_flash_package_size.py \
  --max-firmware-bytes 5570560 \
  --expect-littlefs-bytes 2424832
run_step "LittleFS image/runtime compatibility" python3 scripts/check_littlefs_image_compatibility.py \
  --candidate .pio/build/waveshare-349/littlefs.bin

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

# Emit timing artifact for budget checker
TIMING_DIR="$ROOT_DIR/.artifacts/test_reports/ci-test"
mkdir -p "$TIMING_DIR"
echo "{\"elapsed_seconds\": ${ELAPSED}, \"lane\": \"ci-test\"}" > "$TIMING_DIR/timing.json"

section "Budget Check"
run_step "ci-test timing budget" python3 scripts/check_ci_budget.py ci-test "$TIMING_DIR/timing.json"

echo ""
echo -e "${GREEN}All gates passed in ${ELAPSED}s${NC}"

section "Size Report"
echo "waveshare-349:"
"$PIO_CMD" run "${PIO_BUILD_ARGS[@]}" -t size 2>/dev/null | grep -E "(RAM|Flash|used|bytes)"

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

echo ""
echo "============================================"
echo -e "${GREEN}All CI checks passed${NC}"
echo "Elapsed: ${ELAPSED}s"
echo "============================================"
