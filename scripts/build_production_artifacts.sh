#!/usr/bin/env bash
# Build and validate the production firmware and LittleFS inputs.
#
# Callers install the pinned npm and Python dependencies first. This script is
# intentionally shared by full CI and Release so both paths package artifacts
# through the same frontend, firmware, memory, size, and filesystem checks.

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

START_TIME="$(date +%s)"
PIO_CMD="${PIO_CMD:-pio}"
PIO_JOBS="${PLATFORMIO_RUN_JOBS:-}"
PIO_ENV="${PIO_ENV:-waveshare-349}"
PIO_BUILD_ARGS=(-e "$PIO_ENV")
RELEASE_SOURCE_SHA=""
if [[ -n "${V1_RELEASE_VERSION:-}" ]]; then
  RELEASE_SOURCE_SHA="$(python3 scripts/stage_release_source.py --check-source)"
fi

if [[ -n "$PIO_JOBS" && ! "$PIO_JOBS" =~ ^[1-9][0-9]*$ ]]; then
  echo -e "${RED}Invalid PLATFORMIO_RUN_JOBS: $PIO_JOBS${NC}" >&2
  echo "Expected a positive integer." >&2
  exit 2
fi
if [[ -n "$PIO_JOBS" ]]; then
  PIO_BUILD_ARGS+=(-j "$PIO_JOBS")
fi

if ! command -v "$PIO_CMD" >/dev/null 2>&1; then
  echo -e "${RED}PlatformIO not found: $PIO_CMD${NC}" >&2
  exit 1
fi
if ! command -v npm >/dev/null 2>&1; then
  echo -e "${RED}npm not found in PATH.${NC}" >&2
  exit 1
fi
if [[ ! -d "$ROOT_DIR/interface/node_modules" ]]; then
  echo -e "${RED}Frontend dependencies are not installed.${NC}" >&2
  echo "Run 'npm ci' in interface/ before building production artifacts." >&2
  exit 1
fi

source "$ROOT_DIR/scripts/platformio_ca_bundle.sh"
export PIO_CMD PIO_ENV SSL_CERT_FILE REQUESTS_CA_BUNDLE

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

run_firmware_build_with_memory_log() {
  local memory_dir="$ROOT_DIR/.artifacts/test_reports/memory-headroom"
  mkdir -p "$memory_dir"
  "$PIO_CMD" run "${PIO_BUILD_ARGS[@]}" 2>&1 \
    | tee "$memory_dir/$PIO_ENV-build.log"
}

echo "============================================"
echo "Production Artifact Build"
echo "============================================"

section "Toolchain"
run_step "PlatformIO Core version" \
  python3 scripts/check_platformio_core_version.py --pio "$PIO_CMD"
run_step "PlatformIO project dependencies" \
  "$PIO_CMD" pkg install -e "$PIO_ENV"

BUILD_DIR="$(python3 scripts/build_context.py --pio "$PIO_CMD" --env "$PIO_ENV" --get build_dir)"
PARTITION_TABLE="$(python3 scripts/build_context.py --pio "$PIO_CMD" --env "$PIO_ENV" --get partitions)"

section "Frontend"
(
  cd interface
  run_step "Frontend build" npm run build
  run_step "Frontend deploy" npm run deploy:built
)

section "Frontend Packaging"
run_step "Audio asset manifest (source + deployed)" \
  python3 scripts/check_audio_asset_manifest.py

section "Firmware and Filesystem"
run_step "Firmware clean" "$PIO_CMD" run "${PIO_BUILD_ARGS[@]}" -t clean
run_step "Firmware build" run_firmware_build_with_memory_log
run_step "Firmware contains the current web interface" \
  python3 scripts/generate_web_bundle.py verify-firmware "$BUILD_DIR/firmware.bin"
run_step "Firmware memory headroom" python3 scripts/check_memory_headroom.py \
  --pio "$PIO_CMD" --env "$PIO_ENV" \
  --no-build \
  --build-log ".artifacts/test_reports/memory-headroom/$PIO_ENV-build.log" \
  --warn-diram-zero
run_step "LittleFS image build" "$PIO_CMD" run "${PIO_BUILD_ARGS[@]}" -t buildfs
run_step "Flash package truth report" python3 scripts/report_flash_package_size.py \
  --build-dir "$BUILD_DIR" --partition-table "$PARTITION_TABLE" \
  --max-firmware-bytes 5570560 \
  --expect-littlefs-bytes 2424832
run_step "LittleFS image/runtime compatibility" \
  python3 scripts/check_littlefs_image_compatibility.py \
  --pio "$PIO_CMD" --env "$PIO_ENV" --partition-table "$PARTITION_TABLE" \
  --candidate "$BUILD_DIR/littlefs.bin"
if [[ -n "$RELEASE_SOURCE_SHA" ]]; then
  ARDUINO_SOURCE_DIR="$(python3 scripts/build_context.py --pio "$PIO_CMD" --env "$PIO_ENV" --get arduino_dir)"
  run_step "Record release source identity" python3 scripts/stage_release_source.py \
    --build-dir "$BUILD_DIR" --arduino-source-dir "$ARDUINO_SOURCE_DIR" \
    --source-sha "$RELEASE_SOURCE_SHA" --tag "v$V1_RELEASE_VERSION"
fi
END_TIME="$(date +%s)"
ELAPSED="$((END_TIME - START_TIME))"

echo ""
echo "============================================"
echo -e "${GREEN}Production artifacts passed in ${ELAPSED}s${NC}"
echo "============================================"
