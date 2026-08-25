#!/bin/bash
# Authoritative repo gate used locally and by GitHub workflows.
#
# Gate order: pinned toolchain/workflow checks -> production build contracts ->
# privacy and publication guards -> firmware static analysis -> Python regression
# tests -> host-tool tests -> native tests across every host environment ->
# frontend checks -> production artifact build.
# Privacy guards run their own regression suites inline; a guard is only as
# trustworthy as the proof that it still detects.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

usage() {
  cat <<'EOF'
Usage: scripts/ci-test.sh [--fast] [--help]

  --fast   Static preflight only: toolchain/workflow pins, build contracts, and
           privacy/publication guards. No firmware analysis, native tests, or builds.
  --help   Show this message.
EOF
}

FAST=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --fast)
      FAST=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo -e "${RED}Unknown argument: $1${NC}" >&2
      usage >&2
      exit 2
      ;;
  esac
done

START_TIME=$(date +%s)
PIO_JOBS="${PLATFORMIO_RUN_JOBS:-}"
if [[ -n "$PIO_JOBS" && ! "$PIO_JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo -e "${RED}Invalid PLATFORMIO_RUN_JOBS: $PIO_JOBS${NC}" >&2
  echo "Expected a positive integer." >&2
  exit 2
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

run_v1replay_swift_tests() {
  local swift_driver=(xcrun swift)
  local module_cache="$ROOT_DIR/tools/v1replay/.build/module-cache"

  if [[ -d /Applications/Xcode.app/Contents/Developer ]]; then
    swift_driver=(env DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcrun swift)
  fi

  mkdir -p "$module_cache"
  CLANG_MODULE_CACHE_PATH="$module_cache" \
    SWIFTPM_MODULECACHE_OVERRIDE="$module_cache" \
    "${swift_driver[@]}" test \
    --package-path "$ROOT_DIR/tools/v1replay"
}

run_camera_recorder_checks() {
  local module_cache="$ROOT_DIR/tools/v1replay/.build/camera-module-cache"
  local recorder="$ROOT_DIR/scripts/bench/camera_recorder.swift"
  local xcrun_driver=(xcrun)

  if [[ -d /Applications/Xcode.app/Contents/Developer ]]; then
    xcrun_driver=(env DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer xcrun)
  fi

  mkdir -p "$module_cache"
  "${xcrun_driver[@]}" swiftc -module-cache-path "$module_cache" -typecheck "$recorder"
  "${xcrun_driver[@]}" swift -module-cache-path "$module_cache" "$recorder" --self-test-timing
  "${xcrun_driver[@]}" swift -module-cache-path "$module_cache" "$recorder" --self-test-writer
}

PIO_CMD="${PIO_CMD:-pio}"
if ! command -v "$PIO_CMD" >/dev/null 2>&1; then
  echo -e "${RED}PlatformIO not found in PATH.${NC}" >&2
  exit 1
fi
source "$ROOT_DIR/scripts/platformio_ca_bundle.sh"
export PIO_CMD SSL_CERT_FILE REQUESTS_CA_BUNDLE

echo "============================================"
if [[ "$FAST" -eq 1 ]]; then
  echo "Static Preflight"
else
  echo "Authoritative Local CI Gate"
fi
echo "============================================"

section "Toolchain"
run_step "PlatformIO Core version" python3 scripts/check_platformio_core_version.py --pio "$PIO_CMD"
run_step "Workflow action pin contract" python3 scripts/check_workflow_action_pins.py

section "Build Contracts"
run_step "Empty control-flow body regression suite" python3 scripts/test_check_no_empty_conditionals.py
run_step "Empty control-flow body guard" python3 scripts/check_no_empty_conditionals.py
run_step "Memory headroom regression suite" python3 scripts/test_check_memory_headroom.py
run_step "Build reset regression suite" python3 scripts/test_build_reset.py
run_step "ESP32-S3 framework contract regression suite" python3 scripts/test_verify_esp32s3_framework.py

section "Privacy"
# The guards run first, then the tests that prove the guards still work. A
# scanner that silently stopped detecting something passes its own scan.
run_step "Public commit metadata privacy guard" python3 scripts/check_public_commit_metadata.py --revision=--all
run_step "Public publication-history privacy guard" python3 scripts/check_public_snapshot_privacy.py --all-history
run_step "Snapshot scanner regression suite" python3 scripts/test_check_public_snapshot_privacy.py
run_step "Privacy hook regression suite" python3 scripts/test_public_privacy_hooks.py
run_step "Scanner parity with the internal repository" python3 scripts/test_scanner_parity.py
run_step "v1replay source-only publication guard" python3 tools/v1replay/verify/check_publication_safety.py
run_step "v1replay publication guard regression suite" python3 scripts/test_v1replay_publication_safety.py
run_step "v1replay producer-to-firmware protocol contract" python3 tools/v1replay/verify/verify_protocol.py

if [[ "$FAST" -eq 1 ]]; then
  ELAPSED=$(($(date +%s) - START_TIME))
  echo ""
  echo -e "${GREEN}Static preflight passed in ${ELAPSED}s${NC}"
  exit 0
fi

section "Static Analysis"
run_step "Firmware static analysis" "$PIO_CMD" check -e waveshare-349 --fail-on-defect=medium

section "Python Regression Tests"
# Safety-critical guard regressions already run inline above. Keep the remaining
# script and workflow regressions in the full gate without expanding --fast.
run_step "Camera artifact regression suite" python3 scripts/test_camera_artifacts.py
run_step "Camera preflight regression suite" python3 scripts/test_camera_preflight.py
run_step "Bench window regression suite" python3 scripts/test_bench_window.py
run_step "LittleFS compatibility regression suite" python3 scripts/test_check_littlefs_image_compatibility.py
run_step "Commit metadata regression suite" python3 scripts/test_check_public_commit_metadata.py
run_step "App-only upload offset regression suite" python3 scripts/test_force_app_upload_offset.py
run_step "Identity gate workflow regression suite" python3 scripts/test_identity_gate_workflow.py
run_step "Release preparation regression suite" python3 scripts/test_prepare_release.py
run_step "Release workflow flash contract regression suite" python3 scripts/test_release_workflow_flash_contract.py
run_step "Release license staging regression suite" python3 scripts/test_stage_release_licenses.py

section "Host Tools"
if [[ "$(uname -s)" == "Darwin" ]]; then
  run_step "v1replay Swift package tests" run_v1replay_swift_tests
  run_step "Native camera recorder checks" run_camera_recorder_checks
else
  echo -e "${YELLOW}[skip] v1replay Swift package tests require macOS CoreBluetooth${NC}"
  echo -e "${YELLOW}[skip] Native camera recorder checks require macOS AVFoundation${NC}"
fi

section "Native Tests"
run_step "Native unit tests" python3 scripts/run_native_tests_serial.py
run_step "Native sanitizer unit tests" python3 scripts/run_native_tests_serial.py --env native-sanitized
run_step "Native car-mode unit tests" python3 scripts/run_native_tests_serial.py --env native-car

section "Frontend"
run_step "Frontend dependencies" bash -c 'cd interface && npm ci'
run_step "Frontend dependency audit" bash -c 'cd interface && npm audit --audit-level=high'
run_step "Frontend lint and type checks" bash -c 'cd interface && npm run lint'
run_step "Frontend unit tests" bash -c 'cd interface && npm test'

section "Production Build"
run_step "Production artifact build" ./scripts/build_production_artifacts.sh

END_TIME=$(date +%s)
ELAPSED=$((END_TIME - START_TIME))

echo ""
echo "============================================"
echo -e "${GREEN}All CI checks passed${NC}"
echo "Elapsed: ${ELAPSED}s"
echo "============================================"
