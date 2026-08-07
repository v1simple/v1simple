#!/usr/bin/env bash
# Synthetic maintenance-mode contract check.
#
# Exercises a live unit's HTTP API the way the bundled web UI does and asserts
# the contract the UI depends on: response codes, the JSON keys each page
# consumes, maintenance-mode 409 gating, the X-V1Simple-Request write header,
# the static-path guard, and that the unit serves the repo's current UI build.
#
# Read-only except one benign display preview (brief color demo on the LCD,
# then cleared). No settings are modified.
#
# Usage: tools/synthetic_maintenance_check.sh <device-ip>
# Requires: curl, python3. Exit 0 = all checks pass.
set -uo pipefail

IP="${1:?usage: synthetic_maintenance_check.sh <device-ip>}"
BASE="http://$IP"
HDR="X-V1Simple-Request: maintenance-ui"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0

ok()   { PASS=$((PASS + 1)); echo "PASS  $1"; }
bad()  { FAIL=$((FAIL + 1)); echo "FAIL  $1"; }

# expect_code <desc> <expected-code> <curl args...>
expect_code() {
  local desc="$1" want="$2"; shift 2
  local got
  got=$(curl -s -m 10 -o /dev/null -w "%{http_code}" "$@")
  if [ "$got" = "$want" ]; then ok "$desc ($got)"; else bad "$desc (want $want, got $got)"; fi
}

# fetch_json <name> <path>  -> saves $TMP/<name>.json, asserts 200
fetch_json() {
  local name="$1" path="$2" got
  got=$(curl -s -m 10 -o "$TMP/$name.json" -w "%{http_code}" "$BASE$path")
  if [ "$got" = "200" ]; then ok "GET $path (200)"; else bad "GET $path (want 200, got $got)"; return 1; fi
}

# require_keys <name> <json-file> <keys...> — keys the web UI reads; extra keys are fine.
require_keys() {
  local name="$1" file="$2"; shift 2
  if python3 - "$TMP/$file" "$@" <<'PY'
import json, sys
doc = json.load(open(sys.argv[1]))
missing = [k for k in sys.argv[2:] if k not in doc]
sys.exit(1 if missing else 0)
PY
  then ok "$name has all UI-consumed keys"; else bad "$name missing UI-consumed keys"; fi
}

echo "== Synthetic maintenance-mode check against $BASE =="

# --- reachability / maintenance state ---------------------------------------
fetch_json status /api/status || { echo "Device unreachable or not serving; aborting."; exit 1; }
if python3 -c "import json,sys;sys.exit(0 if json.load(open('$TMP/status.json')).get('maintenanceBoot') else 1)"; then
  ok "unit reports maintenanceBoot=true"
else
  bad "unit is not in maintenance boot (checks below may 409/timeout)"
fi
require_keys "/api/status" status.json \
  wifi device battery v1_connected maintenanceBoot maintenanceBootUptimeMs maintenanceBootTimeoutMs

# --- settings endpoints: keys each UI page consumes --------------------------
fetch_json device_settings /api/device/settings &&
  require_keys "/api/device/settings" device_settings.json \
    ap_ssid ap_password isDefaultPassword proxy_ble proxy_name autoPowerOffMinutes \
    apTimeoutMinutes alpEnabled alpSdLogEnabled alpAlertPersistSec alpDisableV1LaserOnPush powerOffSdLog

fetch_json display_settings /api/display/settings &&
  require_keys "/api/display/settings" display_settings.json \
    bogey freq freqUseBandColor arrowFront arrowSide arrowRear bandL bandKa bandK bandX bandPhoto \
    wifiIcon wifiConnected bleConnected bleDisconnected bar1 bar2 bar3 bar4 bar5 bar6 \
    muted persisted volumeMain volumeMute rssiV1 rssiProxy obd alpConnected alpDli alpLidActive alpAlert \
    hideWifiIcon hideProfileIndicator hideBatteryIcon showBatteryPercent hideBleIcon \
    hideVolumeIndicator hideRssiIndicator brightness

fetch_json audio_settings /api/audio/settings &&
  require_keys "/api/audio/settings" audio_settings.json \
    voiceAlertMode voiceDirectionEnabled announceBogeyCount muteVoiceIfVolZero voiceVolume \
    announceSecondaryAlerts secondaryLaser secondaryKa secondaryK secondaryX \
    alertVolumeFadeEnabled alertVolumeFadeDelaySec alertVolumeFadeVolume \
    speedMuteEnabled speedMuteThresholdMph speedMuteHysteresisMph speedMuteVolume speedMuteVoice stealthEnabled

fetch_json quiet_settings /api/quiet/settings &&
  require_keys "/api/quiet/settings" quiet_settings.json \
    alertVolumeFadeEnabled alertVolumeFadeDelaySec alertVolumeFadeVolume \
    speedMuteEnabled speedMuteThresholdMph speedMuteHysteresisMph speedMuteVolume stealthEnabled

fetch_json wifi_status /api/wifi/status &&
  require_keys "/api/wifi/status" wifi_status.json enabled savedSSID state scanRunning
fetch_json wifi_networks /api/wifi/networks &&
  require_keys "/api/wifi/networks" wifi_networks.json slots
fetch_json v1_profiles /api/v1/profiles && require_keys "/api/v1/profiles" v1_profiles.json profiles
fetch_json v1_current  /api/v1/current  && require_keys "/api/v1/current" v1_current.json connected available
fetch_json v1_devices  /api/v1/devices  && require_keys "/api/v1/devices" v1_devices.json devices count
fetch_json autopush_slots /api/autopush/slots &&
  require_keys "/api/autopush/slots" autopush_slots.json enabled activeSlot slots
fetch_json obd_config /api/obd/config &&
  require_keys "/api/obd/config" obd_config.json \
    enabled minRssi obdScanWindowMs obdRetryIntervalMs proxyOpenWindowMs wifiOpenTimeoutMs \
    v1SettleQuietMs v1SettleFallbackMs cycleTeardownAckTimeoutMs
fetch_json obd_devices /api/obd/devices && require_keys "/api/obd/devices" obd_devices.json devices count
fetch_json gps_config /api/gps/config &&
  require_keys "/api/gps/config" gps_config.json \
    gpsEnabled gpsBaud gpsEnablePinActiveHigh gpsLogUtcToPerf gpsLogUtcToAlp
fetch_json diagnostics_logs /api/diagnostics/logs &&
  require_keys "/api/diagnostics/logs" diagnostics_logs.json \
    success maxListedFiles maxScannedEntries maxDownloadBytes files truncated

# --- backup export ------------------------------------------------------------
fetch_json backup /api/settings/backup
if python3 - "$TMP/backup.json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
assert d.get("_type") in ("v1simple_backup", "v1simple_sd_backup")
ap = d.get("apPassword", "")
assert not isinstance(ap, str) or not ap or ap.startswith("hex:"), "apPassword not obfuscated"
assert not any("passwordObf" in s for s in d.get("wifiStaSlots", [])), "STA password leaked in default export"
PY
then ok "backup export: recognized _type, obfuscated apPassword, no STA passwords"; else bad "backup export contract"; fi

# --- maintenance-mode 409 gating ----------------------------------------------
for p in /api/obd/status /api/gps/status /api/alp/status; do
  expect_code "GET $p returns maintenance 409" 409 "$BASE$p"
done
expect_code "POST Auto-Push live push returns maintenance 409" 409 \
  -X POST -H "$HDR" "$BASE/api/autopush/push"

# --- write-header enforcement + benign preview round-trip ----------------------
expect_code "POST without header rejected" 403 -X POST "$BASE/api/display/preview/clear"
expect_code "POST preview with header" 200 -X POST -H "$HDR" "$BASE/api/display/preview"
sleep 1
expect_code "POST preview clear with header" 200 -X POST -H "$HDR" "$BASE/api/display/preview/clear"

# --- static-path guard + SPA pages ---------------------------------------------
for p in / /settings /colors /audio /profiles /devices /autopush /alp /obd /gps /logs; do
  expect_code "page $p served" 200 "$BASE$p"
done
expect_code "/dev blocked in production" 404 "$BASE/dev"
expect_code "path traversal rejected" 404 "$BASE/..%2fsecret"
expect_code "unlisted static path rejected" 404 "$BASE/foo.txt"

# --- served UI build matches repo build -----------------------------------------
if [ -f "$ROOT_DIR/interface/build/_app/version.json" ]; then
  fetch_json device_version /_app/version.json
  if diff -q "$ROOT_DIR/interface/build/_app/version.json" "$TMP/device_version.json" >/dev/null 2>&1; then
    ok "device serves the repo's current UI build ($(cat "$TMP/device_version.json"))"
  else
    bad "device UI build differs from repo build (device: $(cat "$TMP/device_version.json" 2>/dev/null); repo: $(cat "$ROOT_DIR/interface/build/_app/version.json")) — rebuild/reflash web assets"
  fi
else
  echo "SKIP  repo UI build not present (interface/build); run the web build first for version comparison"
fi

echo "== $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
