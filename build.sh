#!/bin/bash
# Complete build script for V1 Simple
# Builds web interface, deploys to data/, builds firmware, and optionally uploads

set -euo pipefail

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

RESET_SKIP_REASON=""

file_size_bytes() {
    wc -c < "$1" | tr -d '[:space:]'
}

file_sha256() {
    local path="$1"

    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$path" | awk '{print $1}'
        return
    fi

    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "$path" | awk '{print $1}'
        return
    fi

    echo "sha256-unavailable"
}

stage_dist_artifact() {
    local src="$1"
    local dest="$2"
    local label="$3"

    if [ ! -f "$src" ]; then
        echo -e "${RED}Missing ${label}: ${src}${NC}" >&2
        exit 1
    fi

    cp "$src" "$dest"
    local bytes
    bytes="$(file_size_bytes "$dest")"
    local sha256
    sha256="$(file_sha256 "$dest")"
    echo -e "${GREEN}Staged → ${dest}${NC}"
    echo "   bytes=${bytes} sha256=${sha256}"
}

reset_device_via_rts() {
    local port="$1"

    if [ -z "$port" ]; then
        RESET_SKIP_REASON="no serial port available for post-upload reset"
        return 1
    fi

    if ! command -v python3 >/dev/null 2>&1; then
        RESET_SKIP_REASON="python3 not found; skipping post-upload reset"
        return 1
    fi

    if ! python3 - <<'PY' >/dev/null 2>&1
import importlib.util
import sys

raise SystemExit(0 if importlib.util.find_spec("serial") else 1)
PY
    then
        RESET_SKIP_REASON="pyserial not installed for python3; skipping post-upload reset"
        return 1
    fi

    if ! python3 - "$port" <<'PY' >/dev/null 2>&1
import serial
import sys

port = sys.argv[1]
ser = serial.Serial(port, 115200)
ser.setRTS(True)
ser.setRTS(False)
ser.close()
PY
    then
        RESET_SKIP_REASON="failed to toggle RTS on $port"
        return 1
    fi

    RESET_SKIP_REASON=""
    return 0
}

# Detect Windows and set PIO command accordingly. PIO_CMD may be set by callers
# that need a specific PlatformIO executable, for example /opt/homebrew/bin/pio.
if [[ -n "${PIO_CMD:-}" ]]; then
    :
elif [[ "${OSTYPE:-}" == "msys" || "${OSTYPE:-}" == "cygwin" || "${OSTYPE:-}" == "win32" ]] || \
   [[ -n "${WINDIR:-}" ]] || [[ "${OS:-}" == "Windows_NT" ]] || [[ -d "/c/Windows" ]]; then
    # Check if pio is in PATH first (e.g., pip install), then fall back to .platformio path
    if command -v pio &> /dev/null; then
        PIO_CMD="pio"
    elif [[ -f "$HOME/.platformio/penv/Scripts/pio.exe" ]]; then
        PIO_CMD="$HOME/.platformio/penv/Scripts/pio.exe"
    else
        echo -e "${RED}PlatformIO not found. Install it first.${NC}"
        echo "   Run: pip install platformio"
        exit 1
    fi
    echo -e "${BLUE}Detected Windows${NC}"
else
    if command -v pio &> /dev/null; then
        PIO_CMD="pio"
    else
        echo -e "${RED}PlatformIO not found in PATH.${NC}"
        echo "   Install PlatformIO CLI or use VS Code PlatformIO extension terminal."
        exit 1
    fi
fi

# Give PlatformIO/requests an explicit CA bundle before package downloads.
# This avoids macOS Python installs that fail TLS verification without certifi.
source "$SCRIPT_DIR/scripts/platformio_ca_bundle.sh"
export PIO_CMD SSL_CERT_FILE REQUESTS_CA_BUNDLE

DEFAULT_ENV="waveshare-349"

# Parse arguments
CLEAN=false
UPLOAD_FS=false
UPLOAD_FW=false
MONITOR=false
SKIP_WEB=false
RUN_TESTS=false
DIST_BUILD=false
PIO_ENV="$DEFAULT_ENV"
UPLOAD_PORT=""
PIO_JOBS=""

while [[ $# -gt 0 ]]; do
    case $1 in
        --clean|-c)
            CLEAN=true
            shift
            ;;
        --upload-fs|-f)
            UPLOAD_FS=true
            shift
            ;;
        --upload|-u)
            UPLOAD_FW=true
            shift
            ;;
        --monitor|-m)
            MONITOR=true
            shift
            ;;
        --all|-a)
            UPLOAD_FS=true
            UPLOAD_FW=true
            MONITOR=true
            shift
            ;;
        --skip-web|-s)
            SKIP_WEB=true
            shift
            ;;
        --test|-t)
            RUN_TESTS=true
            shift
            ;;
        --env|-e)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --env" >&2
                exit 1
            fi
            PIO_ENV="$2"
            shift 2
            ;;
        --car)
            PIO_ENV="esp32-s3-car-install"
            shift
            ;;
        --dist)
            DIST_BUILD=true
            shift
            ;;
        --upload-port)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --upload-port" >&2
                exit 1
            fi
            UPLOAD_PORT="$2"
            shift 2
            ;;
        --jobs|-j)
            if [[ $# -lt 2 ]]; then
                echo "Missing value for --jobs" >&2
                exit 1
            fi
            PIO_JOBS="$2"
            if ! [[ "$PIO_JOBS" =~ ^[1-9][0-9]*$ ]]; then
                echo -e "${RED}Invalid --jobs value: $PIO_JOBS${NC}"
                echo "   Expected a positive integer."
                exit 1
            fi
            shift 2
            ;;
        --help|-h)
            echo "Usage: $0 [OPTIONS]"
            echo ""
            echo "Options:"
            echo "  -c, --clean        Clean build (remove .pio/build/)"
            echo "  -f, --upload-fs    Upload filesystem after build (tests run only with --test)"
            echo "  -u, --upload       Upload firmware after build (tests run only with --test)"
            echo "  -m, --monitor      Open serial monitor after upload"
            echo "  -a, --all          Upload filesystem, firmware, and monitor"
            echo "  -s, --skip-web     Skip web interface build"
            echo "  -t, --test         Run unit tests before upload (native environment)"
            echo "  -e, --env ENV      PlatformIO environment (default: waveshare-349)
  --car              Build for car-install (CAR_MODE_PWR_SHORT, env: esp32-s3-car-install)
  --dist             Copy firmware.bin + littlefs.bin to dist/standard/ or dist/car/"
            echo "  -j, --jobs N       PlatformIO job count override (default: PlatformIO default)"
            echo "  --upload-port PORT COM port for upload (e.g., COM6)"
            echo "  -h, --help         Show this help"
            echo ""
            echo "Examples:"
            echo "  $0                 # Build everything (no upload)"
            echo "  $0 --clean --all   # Clean build and upload everything"
            echo "  $0 --clean --all --jobs 1  # Force single-threaded PlatformIO run"
            echo "  $0 -u -m           # Build firmware, upload, and monitor"
            echo "  $0 -f              # Build and upload filesystem only"
            echo "  $0 -s -u           # Skip web build, just build and upload firmware"
            echo "  $0 --all --test    # Build, test, upload everything"
            echo "  $0 --all --env waveshare-349    # Explicit env
  $0 --car --clean --all         # Car-install build + upload
  $0 --car --dist                # Car-install build, stage to dist/car/"
            exit 0
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            echo "Use --help for usage information"
            exit 1
            ;;
    esac
done

python3 "$SCRIPT_DIR/scripts/check_platformio_core_version.py" --pio "$PIO_CMD"

# Build PIO arguments
PIO_ARGS="-e $PIO_ENV"
if [ -n "$UPLOAD_PORT" ]; then
    PIO_ARGS="$PIO_ARGS --upload-port $UPLOAD_PORT"
fi
PIO_RUN_ARGS="$PIO_ARGS"
if [ -n "$PIO_JOBS" ]; then
    PIO_RUN_ARGS="$PIO_RUN_ARGS -j $PIO_JOBS"
fi

echo -e "${BLUE}== V1 Simple build ==${NC}"
echo ""

# Clean build artifacts if requested
if [ "$CLEAN" = true ]; then
    echo -e "${YELLOW}Cleaning build artifacts...${NC}"
    rm -rf .pio/build interface/build interface/.svelte-kit
    echo -e "${GREEN}Clean complete${NC}"
    echo ""
fi

# Web interface
if [ "$SKIP_WEB" = false ]; then
    echo -e "${YELLOW}Building web interface...${NC}"

    # Check if node_modules exists
    if [ ! -d "interface/node_modules" ]; then
        echo -e "${YELLOW}Installing npm dependencies (first time)...${NC}"
        cd interface
        npm install
        cd ..
    fi

    cd interface
    npm run build
    echo -e "${GREEN}Web interface built${NC}"

    # Deploy to data/
    echo -e "${YELLOW}Deploying to data/ folder...${NC}"
    npm run deploy
    cd ..
    echo -e "${GREEN}Web files deployed to data/${NC}"

    echo ""
else
    echo -e "${YELLOW}Skipping web interface build${NC}"
    # Ensure data directory exists with at least one file for LittleFS build
    if [ ! -d "data" ] || [ -z "$(ls -A data 2>/dev/null)" ]; then
        echo -e "${YELLOW}   Creating minimal data/ directory for filesystem build...${NC}"
        mkdir -p data
        echo '{"placeholder":true}' > data/.placeholder.json
    fi
    echo ""
fi

# Firmware
echo -e "${YELLOW}Building firmware (env: $PIO_ENV)...${NC}"
"$PIO_CMD" run $PIO_RUN_ARGS
echo -e "${GREEN}Firmware built successfully${NC}"

# Show build size
echo -e "${BLUE}Build size:${NC}"
"$PIO_CMD" run $PIO_RUN_ARGS -t size | grep -E "RAM:|Flash:" || true
echo ""

# Stage binaries to dist/ if requested
if [ "$DIST_BUILD" = true ]; then
    echo -e "${YELLOW}Building LittleFS image for dist package...${NC}"
    "$PIO_CMD" run $PIO_RUN_ARGS -t buildfs
    echo -e "${GREEN}LittleFS image built successfully${NC}"

    if [[ "$PIO_ENV" == *"car"* ]]; then
        DIST_VARIANT="car"
    else
        DIST_VARIANT="standard"
    fi
    DIST_DIR="dist/${DIST_VARIANT}"
    mkdir -p "$DIST_DIR"
    FW_SRC=".pio/build/${PIO_ENV}/firmware.bin"
    FS_SRC=".pio/build/${PIO_ENV}/littlefs.bin"

    stage_dist_artifact "$FW_SRC" "${DIST_DIR}/firmware.bin" "firmware artifact"
    stage_dist_artifact "$FS_SRC" "${DIST_DIR}/littlefs.bin" "LittleFS artifact"
    echo ""
fi

# Tests (requires gcc/g++ on host)
if [ "$RUN_TESTS" = true ]; then
    echo -e "${YELLOW}Running unit tests...${NC}"
    "$PIO_CMD" test -e native
    echo -e "${GREEN}All tests passed${NC}"

    # Also check firmware compilation (catches platform-specific issues)
    echo -e "${YELLOW}Checking firmware compilation...${NC}"
    "$PIO_CMD" run $PIO_RUN_ARGS --target buildprog
    echo -e "${GREEN}Firmware compiles without errors${NC}"
    echo ""
fi

# Upload filesystem
if [ "$UPLOAD_FS" = true ]; then
    echo -e "${YELLOW}Warning: uploadfs overwrites internal LittleFS data.${NC}"
    echo -e "${YELLOW}   If profile storage ever fell back to LittleFS, those profiles will be erased.${NC}"
    echo -e "${YELLOW}   Confirm SD is mounted in boot logs before relying on profile persistence.${NC}"
    echo -e "${YELLOW}Uploading filesystem (LittleFS)...${NC}"
    "$PIO_CMD" run $PIO_RUN_ARGS -t uploadfs
    echo -e "${GREEN}Filesystem uploaded${NC}"
    echo ""
fi

# Upload firmware
if [ "$UPLOAD_FW" = true ]; then
    echo -e "${YELLOW}Uploading firmware...${NC}"
    "$PIO_CMD" run $PIO_RUN_ARGS -t upload
    echo -e "${GREEN}Firmware uploaded${NC}"

    # Extra reset after upload to ensure clean BLE state
    echo -e "${YELLOW}Resetting device for clean start...${NC}"
    sleep 1
    RESET_PORT="$UPLOAD_PORT"
    if [ -z "$RESET_PORT" ]; then
        # Auto-detect port like PlatformIO does
        RESET_PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
    fi
    if reset_device_via_rts "$RESET_PORT"; then
        echo -e "${GREEN}Device reset${NC}"
    else
        echo -e "${YELLOW}${RESET_SKIP_REASON}${NC}"
    fi
    echo ""
fi

# Serial monitor
if [ "$MONITOR" = true ]; then
    echo -e "${GREEN}Opening serial monitor...${NC}"
    echo -e "${BLUE}(Press Ctrl+C to exit)${NC}"
    echo ""
    sleep 1
    # Remap upload port to monitor port and strip upload-only flags
    MONITOR_ARGS="$PIO_ARGS"
    if [[ "$MONITOR_ARGS" =~ --upload-port[[:space:]]+([^[:space:]]+) ]]; then
        PORT_VAL="${BASH_REMATCH[1]}"
        MONITOR_ARGS="${MONITOR_ARGS/--upload-port $PORT_VAL/}"
        MONITOR_ARGS="$(echo "$MONITOR_ARGS" | xargs)"
        MONITOR_ARGS="$MONITOR_ARGS --port $PORT_VAL"
    fi
    "$PIO_CMD" device monitor $MONITOR_ARGS
fi

echo ""
echo -e "${GREEN}== Build complete ==${NC}"
echo ""

if [ "$UPLOAD_FW" = false ] && [ "$UPLOAD_FS" = false ]; then
    echo -e "${YELLOW}Tip: use --all to build and upload everything${NC}"
    echo -e "${YELLOW}   Example: ./build.sh --all${NC}"
fi
