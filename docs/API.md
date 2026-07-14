# API Reference

External API surfaces exposed by the device. Three categories are covered:

1. **HTTP API** — the maintenance-mode web interface backend, served on the device's maintenance IP (default `http://192.168.35.5` in AP mode, or the STA-mode IP once a saved maintenance network is joined). Normal drive runtime does not start WiFi or the web server.
2. **BLE GATT proxy service** — the Bluetooth interface that mimics the Valentine 1's own GATT service so companion phone apps can connect to this device as if to a V1.
3. **Settings wire format** — the JSON shape settings flow through. Not a separate protocol; the keys are documented inline in each HTTP settings endpoint.

**Not covered here:**

- Internal C++ module APIs. Each module under `src/modules/<module>/` should carry its own `api.md` next to the code (see project policy). This document covers external surfaces only.
- The upstream V1 BLE protocol itself. Tracked source/test citations should point at `docs/V1_PROTOCOL_REFERENCES.md` by anchor; maintainers can re-check those summaries against local official PDFs when needed.

Every entry below has a `Source:` line pointing at the stable file or handler where the route is registered or the characteristic is defined. If you're searching for "where is X handled," use that source as the entry point; exact line numbers intentionally are not documented because route registration line numbers drift during refactors.

---

# HTTP API

All routes are registered in `src/wifi_routes.cpp` and delegated to handlers in `src/modules/wifi/wifi_*_api_service.cpp` and related files. These routes are reachable during maintenance boot only; endpoints that would require a skipped normal-runtime subsystem return a structured maintenance error instead of pretending the runtime is merely disconnected. Mutating `POST /api/*` requests are accepted only while the authoritative maintenance-boot flag is active and when they include `X-V1Simple-Request: maintenance-ui`; the bundled web UI adds this header automatically. Either failed condition returns the same `403` forbidden response.

## Static / SPA assets

### GET `/_app/env.js`
**Description:** Serves environment configuration from LittleFS.
**Params:** None.
**Response:** JavaScript file served as `application/javascript` (200) or `"Not found"` (404, `text/plain`) if neither raw nor gzip asset can be served.
**Source:** route registration in `src/wifi_routes.cpp`.

### GET `/_app/version.json`
**Description:** Serves version metadata from LittleFS.
**Params:** None.
**Response:** JSON file served as `application/json` (200) or `"Not found"` (404, `text/plain`) if neither raw nor gzip asset can be served.
**Source:** route registration in `src/wifi_routes.cpp`.

### GET `/`
**Description:** Serves the Svelte SPA root. Returns `/index.html` from LittleFS, or 500 if missing.
**Params:** None.
**Response:** HTML `text/html` (200) or `{ "success": false, "error": "Web UI not found. Please reflash with ./build.sh --all" }` (500).
**Source:** route registration in `src/wifi_routes.cpp`.

### GET `/_app/*` (catch-all)
**Description:** Serves allowlisted Svelte app files from `/_app/env.js`, `/_app/version.json`, and `/_app/immutable/*` with appropriate MIME types (js, css, json). Rejects unsafe or unlisted paths; falls through to notFound handler if file not found.
**Params:** None.
**Response:** File content with appropriate MIME type (200) or `{ "success": false, "error": "Not found" }` (404).
**Source:** route registration in `src/wifi_routes.cpp`.

## Captive portal probes

These paths handle the probes that iOS, Android, ChromeOS, and Windows fire when a device joins a WiFi AP, so the OS pops a "sign in" prompt that lands the user on the device's setup UI.

### GET `/ping`
**Description:** Lightweight ping endpoint.
**Response:** `"OK"` (200, `text/plain`).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_portal_api_service.cpp`.

### GET `/generate_204`
**Description:** Android/ChromeOS captive portal probe; returns 204 No Content with no-cache headers.
**Response:** Empty body (204, `text/plain`); headers: `Cache-Control: no-store, no-cache, must-revalidate`, `Pragma: no-cache`.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_portal_api_service.cpp`.

### GET `/gen_204`
**Description:** Alternative Android/ChromeOS probe; identical to `/generate_204`.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_portal_api_service.cpp`.

### GET `/hotspot-detect.html`
**Description:** iOS/macOS captive portal probe; 302 redirect to `/settings` with no-cache headers.
**Response:** Empty HTML body (302); `Location: /settings`.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_portal_api_service.cpp`.

### GET `/fwlink`
**Description:** Windows captive portal probe; 302 redirect to `/settings`.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_portal_api_service.cpp`.

### GET `/ncsi.txt`
**Description:** Windows Network Connectivity Status Indicator probe.
**Response:** `"Microsoft NCSI"` (200, `text/plain`).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_portal_api_service.cpp`.

## Status & device

### GET `/api/status`
**Description:** Cached device status snapshot — WiFi state, battery, device info, maintenance-session state, V1 connection state, optional alert/custom fields.
**Params:** None. Rate-limit checked.
**Response:** `{ "wifi": { "setup_mode": bool, "ap_active": bool, "sta_connected": bool, "sta_ip": string, "ap_ip": string, "ssid": string, "rssi": int32, "sta_enabled": bool, "sta_ssid": string }, "device": { "uptime": ulong, "heap_free": uint32, "hostname": string, "firmware_version": string }, "battery": { "voltage_mv": uint16, "percentage": uint8, "on_battery": bool, "has_battery": bool }, "maintenanceBoot": bool, "maintenanceBootUptimeMs": uint32, "v1_connected": bool, "alert": {...}, ...merged fields... }` (200). In maintenance boot, `v1_connected` is expected to remain false because BLE/V1 runtime init is intentionally skipped. Cache TTL: `STATUS_CACHE_TTL_MS`.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_status_api_service.cpp`.

### GET `/api/device/settings`
**Description:** Device-level settings: AP credentials, proxy config, power management, ALP toggles, NVS diagnostics.
**Response:** `{ "ap_ssid": string, "ap_password": "********", "isDefaultPassword": bool, "proxy_ble": bool, "proxy_name": string, "autoPowerOffMinutes": uint8, "apTimeoutMinutes": uint8, "alpEnabled": bool, "alpSdLogEnabled": bool, "alpAlertPersistSec": uint8, "alpDisableV1LaserOnPush": bool, "powerOffSdLog": bool, "nvsDiag": { "ns": string, "valid": bool, "ver": uint8, "bright": uint8, "proxy": bool, "autoPush": bool, "healthy": bool } }` (200) or `{ "error": "Settings unavailable" }` (500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_settings_api_service.cpp`.

### POST `/api/device/settings`
**Description:** Updates device settings; accepts any subset, clamps to valid ranges.
**Params:** `ap_ssid` (string), `ap_password` (string, ≥8 chars), `proxy_ble` (bool), `proxy_name` (string), `autoPowerOffMinutes` (int 0-60), `apTimeoutMinutes` (int 5-60 or 0), `alpEnabled` (bool), `alpSdLogEnabled` (bool), `alpAlertPersistSec` (int 0-5), `alpDisableV1LaserOnPush` (bool; when true and ALP is enabled, profile pushes clear V1 laser alerting), `powerOffSdLog` (bool).
**Mode rule:** Setting `proxy_ble=true` selects Proxy / App mode and disables OBD. Setting `proxy_ble=false` only turns proxy off; use `POST /api/obd/config` with `enabled=true` to select OBD / Standalone mode. See `docs/CONNECTIVITY_MODES.md`.
**Response:** `{ "success": true }` (200) or `{ "error": "AP SSID required and password must be at least 8 characters" }` (400).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_settings_api_service.cpp`.

## V1 profile system

### GET `/api/v1/profiles`
**Description:** Lists all V1 device profiles (name, description, displayOn flag).
**Response:** `{ "profiles": [ { "name": string, "description": string, "displayOn": bool }, ... ] }` (200).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_v1_profile_api_service.cpp`.

### GET `/api/v1/profile`
**Description:** Returns one profile by name as raw JSON.
**Params:** `name` (string, required).
**Response:** Profile JSON (200) or `{ "error": "Missing profile name" }` (400) or `{ "error": "Profile not found" }` (404).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_v1_profile_api_service.cpp`.

### POST `/api/v1/profile`
**Description:** Saves a profile. Max payload 4096 bytes; triggers SD backup on success.
**Params:** JSON body — `name` (string, required), `description` (string, optional), `settings` (object or top-level fields, optional), `displayOn` (bool, default true).
**Response:** `{ "success": true }` (200) or `{ "error": "..." }` (400, 500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_v1_profile_api_service.cpp`.

### POST `/api/v1/profile/delete`
**Description:** Deletes a profile by name; triggers SD backup on success.
**Params:** JSON body — `name` (string, required).
**Response:** `{ "success": true }` (200) or `{ "error": "Profile not found" }` (404) or `{ "error": "..." }` (500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_v1_profile_api_service.cpp`.

### GET `/api/v1/current`
**Description:** Returns current V1 settings if loaded, with connected flag.
**Response:** `{ "connected": bool, "available": bool, "settings": {...} }` (200).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_v1_profile_api_service.cpp`.

### POST `/api/v1/pull`
**Description:** Requests user bytes from connected V1; response is async via BLE callback.
**Response:** `{ "success": true, "message": "Request sent. Check current settings." }` (200), `{ "error": "maintenance_mode", "message": "V1 push/pull not available in maintenance mode" }` (409), `{ "error": "V1 not connected" }` (503), or `{ "error": "Failed to send request" }` (500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_v1_profile_api_service.cpp`.

### POST `/api/v1/push`
**Description:** Pushes settings to V1. Accepts profile by name, raw byte array, or parsed settings object.
**Params:** JSON body — `name` (string, profile lookup), `bytes` (uint8[6], raw bytes), `settings` (object), or top-level settings fields; `displayOn` (bool, optional).
**Response:** `{ "success": true, "message": "Settings sent to V1" }` (200), `{ "error": "maintenance_mode", "message": "V1 push/pull not available in maintenance mode" }` (409), or `{ "error": "..." }` (400, 404, 500, 503).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_v1_profile_api_service.cpp`.

### GET `/api/v1/devices`
**Description:** Lists known V1 BLE devices (address, name, default profile, connected status).
**Response:** `{ "devices": [ { "address": string, "name": string, "defaultProfile": int, "connected": bool }, ... ], "count": int }` (200).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_v1_devices_api_service.cpp`.

### POST `/api/v1/devices/name`
**Description:** Saves a custom name for a V1 BLE device.
**Params:** `address` (string, required), `name` (string, optional).
**Response:** `{ "success": true }` (200) or `{ "error": "..." }` (400, 500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_v1_devices_api_service.cpp`.

### POST `/api/v1/devices/profile`
**Description:** Sets the default profile (0-3) for a V1 BLE device.
**Params:** `address` (string, required), `profile` (int 0-3, required).
**Response:** `{ "success": true }` (200) or `{ "error": "Invalid address or write failed" | "Invalid profile" }` (400, 500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_v1_devices_api_service.cpp`.

### POST `/api/v1/devices/delete`
**Description:** Deletes a V1 BLE device record by address.
**Params:** `address` (string, required).
**Response:** `{ "success": true }` (200) or `{ "error": "Invalid address or write failed" }` (400, 500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_v1_devices_api_service.cpp`.

## Auto-push (3-slot)

### GET `/api/autopush/slots`
**Description:** Current autopush configuration: enabled flag, active slot, three slot configs.
**Response:** `{ "enabled": bool, "activeSlot": int, "slots": [ { "name": string, "profile": string, "mode": int, "color": uint16, "volume": uint8, "muteVolume": uint8, "darkMode": bool, "muteToZero": bool, "alertPersist": uint8, "priorityArrowOnly": bool }, ... (3 slots) ] }` (200).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_autopush_api_service.cpp`.

### GET `/api/autopush/status`
**Description:** Autopush push status as JSON string.
**Response:** Custom JSON (200) or `{ "error": "Push status not available" }` (500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_autopush_api_service.cpp`.

### POST `/api/autopush/slot`
**Description:** Updates a slot (0-2). Triggers display indicator redraw if the slot is currently active.
**Params:** `slot` (int 0-2, required), `profile` (string, required), `mode` (int, required), `name` (string), `color` (int), `volume` (int), `muteVol` (int), `darkMode` (bool), `muteToZero` (bool), `alertPersist` (int 0-5), `priorityArrowOnly` (bool).
**Response:** `{ "success": true }` (200) or `{ "error": "Invalid slot" | "Missing parameters" }` (400).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_autopush_api_service.cpp`.

### POST `/api/autopush/activate`
**Description:** Activates/deactivates a slot and the autopush feature.
**Params:** `slot` (int 0-2, required), `enable` (bool, default true).
**Response:** `{ "success": true }` (200) or `{ "error": "Invalid slot" | "Missing slot parameter" }` (400).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_autopush_api_service.cpp`.

### POST `/api/autopush/push`
**Description:** Triggers immediate profile push from a slot, with optional profile/mode override.
**Params:** `slot` (int 0-2, required), `profile` (string, optional override), `mode` (int, optional override).
**Response:** `{ "success": true, "queued": true }` (200) or `{ "error": "V1 not connected" }` (503) or `{ "error": "Push already in progress" }` (409) or `{ "error": "No profile configured for this slot" }` (400) or `{ "error": "Failed to load profile" }` (500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_autopush_api_service.cpp`.

## Display

### GET `/api/display/settings`
**Description:** All display color and toggle settings.
**Response:** Color keys (bogey, freq, arrowFront, arrowSide, arrowRear, bandL, bandKa, bandK, bandX, wifiIcon, wifiConnected, bleConnected, bleDisconnected, bar1-bar6, muted, bandPhoto, persisted, volumeMain, volumeMute, rssiV1, rssiProxy, obd, alpConnected, alpDli, alpLidActive, alpAlert) as uint16 (RGB565). Toggles (freqUseBandColor, hideWifiIcon, hideProfileIndicator, hideBatteryIcon, showBatteryPercent, hideBleIcon, hideVolumeIndicator, hideRssiIndicator) as bool. Plus `brightness` (uint8 1-255) (200).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_display_colors_api_service.cpp` (full GET shape — see source).

### POST `/api/display/settings`
**Description:** Updates any subset of display colors and toggles. Sets brightness and optionally triggers preview.
**Params:** Color keys as int (via `server.arg()`), boolean toggles as `"true"`/`"1"`, `brightness` (int, clamped 1-255), `skipPreview` (bool).
**Response:** `{ "success": true }` (200) or `{ "error": "Settings unavailable" }` (500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_display_colors_api_service.cpp`.

### POST `/api/display/settings/reset`
**Description:** Resets display settings to defaults.
**Response:** Shape — see `wifi_display_colors_api_service.cpp` reset handler.
**Source:** route registration in `src/wifi_routes.cpp`.

### POST `/api/display/preview`
**Description:** Triggers display preview to show new colors for a hold duration.
**Response:** Shape — see `wifi_display_colors_api_service.cpp` preview handler.
**Source:** route registration in `src/wifi_routes.cpp`.

### POST `/api/display/preview/clear`
**Description:** Clears the display preview.
**Response:** Shape — see `wifi_display_colors_api_service.cpp` preview-clear handler.
**Source:** route registration in `src/wifi_routes.cpp`.

### GET `/api/display/visual/steps`
**Description:** Bench-only visual verification step manifest. Maintenance boot only.
**Response:** Streamed JSON with manifest binding fields (`schemaVersion`, firmware version/SHA, `settingsFingerprint`), `stepCount`, resolved per-step expectations (including firmware-selected palette roles for frequency and status visuals), and `"complete": true` (200), or maintenance/runtime errors.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_display_visual_api_service.cpp`.

### GET `/api/display/visual/layout`
**Description:** Bench-only visual verification layout and palette manifest. Maintenance boot only.
**Response:** JSON with manifest binding fields, logical/raw framebuffer geometry, transform (`canvas-rotation-1`), coarse semantic zones, asserted element geometry (`bandCells`, `directionArrows`, `mainSignalBars`, `cardSlots`, `cardMeterBars`, `frequency`, `statusText`, `statusBadges`), dynamic `roleSource` bindings, full-card `emptyRect` and drawable `coverageRect` regions, narrowly scoped ignored dynamic regions, overlap metadata, palette role colors, empty `masks`, and `"complete": true` (200), or maintenance/runtime errors.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_display_visual_api_service.cpp`.

### POST `/api/display/visual/pin`
**Description:** Pins one preview step, renders it synchronously, and returns the render sequence id. Maintenance boot only; requires `X-V1Simple-Request: maintenance-ui`.
**Params:** JSON body `{ "index": int, "clear": bool }`; `clear` defaults true.
**Response:** `{ "success": true, "index": int, "clear": bool, "renderSeq": uint32 }` (200), or invalid-body/range/runtime errors.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_display_visual_api_service.cpp`.

### GET `/api/display/visual/framebuffer`
**Description:** Streams raw RGB565LE canvas bytes for the last pinned frame. Maintenance boot only.
**Response:** `application/octet-stream` with exact `Content-Length` and headers `X-FB-Raw-Width`, `X-FB-Raw-Height`, `X-FB-Logical-Width`, `X-FB-Logical-Height`, `X-FB-Format`, `X-FB-Transform`, manifest binding headers (`X-Display-Manifest-Schema-Version`, `X-Display-Firmware-Version`, `X-Display-Firmware-Sha`, `X-Display-Settings-Fingerprint`), `X-Display-Render-Seq`, and `X-Display-Pinned-Step` (200), or fails closed when no frame is pinned.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_display_visual_api_service.cpp`.

### GET `/api/display/visual/flushshadow`
**Description:** Streams the visual-test flush shadow — a raw RGB565LE mirror of every byte actually pushed to the panel (full and partial flushes) since the current visual-test run enabled it. The host verifier asserts shadow == framebuffer after each pin; a mismatch means painted pixels were never flushed or were flushed into the wrong region. Maintenance boot only; the shadow exists only while a visual pin is active.
**Response:** Same headers and byte layout as `/api/display/visual/framebuffer` plus `X-FB-Shadow: 1` (200); fails closed with 409 when no frame is pinned or 503 `flush_shadow_unavailable` when the shadow allocation failed.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_display_visual_api_service.cpp`.

### POST `/api/display/visual/clear`
**Description:** Clears visual-test pinning and preview-owned display overrides, then synchronously redraws the maintenance screen. Maintenance boot only; requires `X-V1Simple-Request: maintenance-ui`. This idempotent release route is exempt from the shared mutation rate limit so an active pin can always be cleaned up.
**Response:** `{ "success": true, "active": false, "restored": true }` (200), or maintenance/runtime errors.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_display_visual_api_service.cpp`.

## Audio controls

### GET `/api/audio/settings`
**Description:** Audio, voice-alert, secondary-announcement, volume-fade, speed-mute, and stealth settings.
**Response:** `{ "voiceAlertMode": int, "voiceDirectionEnabled": bool, "announceBogeyCount": bool, "muteVoiceIfVolZero": bool, "voiceVolume": uint8, "announceSecondaryAlerts": bool, "secondaryLaser": bool, "secondaryKa": bool, "secondaryK": bool, "secondaryX": bool, "alertVolumeFadeEnabled": bool, "alertVolumeFadeDelaySec": uint8, "alertVolumeFadeVolume": uint8, "speedMuteEnabled": bool, "speedMuteThresholdMph": uint8, "speedMuteHysteresisMph": uint8, "speedMuteVolume": uint8, "speedMuteVoice": bool, "stealthEnabled": bool }` (200) or `{ "error": "Settings unavailable" }` (500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_audio_api_service.cpp`.

### POST `/api/audio/settings`
**Description:** Updates any subset of audio settings. Voice mode clamps to 0-3, voice volume to 0-100, fade delay to 1-10 sec, fade volume to 1-9, speed-mute thresholds to 5-60 mph, hysteresis to 1-10 mph, and speed-mute volume to 0-9.
**Params:** `voiceAlertMode` (int 0-3), `voiceDirectionEnabled` (bool), `announceBogeyCount` (bool), `muteVoiceIfVolZero` (bool), `voiceVolume` (int 0-100), `announceSecondaryAlerts` (bool), `secondaryLaser` (bool), `secondaryKa` (bool), `secondaryK` (bool), `secondaryX` (bool), `alertVolumeFadeEnabled` (bool), `alertVolumeFadeDelaySec` (int 1-10), `alertVolumeFadeVolume` (int 1-9), `speedMuteEnabled` (bool), `speedMuteThresholdMph` (int 5-60), `speedMuteHysteresisMph` (int 1-10), `speedMuteVolume` (int 0-9), `speedMuteVoice` (bool), `stealthEnabled` (bool).
**Response:** `{ "success": true }` (200) or `{ "error": "Settings unavailable" }` (500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_audio_api_service.cpp`.

## Quiet controls

> Note: the maintenance UI no longer has a separate Quiet page — these settings are
> managed on the **Audio** page via `/api/audio/settings` (a superset that includes
> `stealthEnabled`). The `/api/quiet/settings` endpoint below remains for
> compatibility and is still used for stealth preview on the Colors page.

### GET `/api/quiet/settings`
**Description:** Quiet-driving settings for V1-side volume fade, speed mute, and stealth display.
**Response:** `{ "alertVolumeFadeEnabled": bool, "alertVolumeFadeDelaySec": uint8, "alertVolumeFadeVolume": uint8, "speedMuteEnabled": bool, "speedMuteThresholdMph": uint8, "speedMuteHysteresisMph": uint8, "speedMuteVolume": uint8, "stealthEnabled": bool }` (200) or `{ "error": "Settings unavailable" }` (500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_quiet_api_service.cpp`.

### POST `/api/quiet/settings`
**Description:** Updates any subset of quiet-driving settings. Speed-mute thresholds clamp to 5-60 mph, fade delay to 1-10 sec, fade volume to 1-9, and speed-mute volume to 0-9.
**Params:** `alertVolumeFadeEnabled` (bool), `alertVolumeFadeDelaySec` (int 1-10), `alertVolumeFadeVolume` (int 1-9), `speedMuteEnabled` (bool), `speedMuteThresholdMph` (int 5-60), `speedMuteHysteresisMph` (int 1-10), `speedMuteVolume` (int 0-9), `stealthEnabled` (bool).
**Response:** `{ "success": true }` (200) or `{ "error": "Settings unavailable" }` (500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_quiet_api_service.cpp`.

## Settings backup/restore

### GET `/api/settings/backup`
**Description:** Generates a sanitized JSON backup snapshot — all profiles, non-secret settings, and catalog. Served as a downloadable attachment and cached per revision. Exports include `wifiStaSlots` metadata (`index`, `ssid`, `label`, `priority`, `lastConnectedAtSec`) but omit AP and saved STA passwords. A legacy `includePasswords` query parameter is ignored; network-accessible backups never contain credential material. Use a local SD backup when full device recovery, including credentials, is required.
**Response:** JSON backup file (200); `Content-Disposition: attachment; filename="v1simple_backup.json"`.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `backup_api_service.cpp`.

### POST `/api/settings/backup-now`
**Description:** Triggers immediate backup to SD card if available.
**Response:** Backup status response (200) or error if SD not ready.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `backup_api_service.cpp`.

### POST `/api/settings/restore`
**Description:** Restores settings from uploaded JSON backup. Enforces a 128 KB application-level limit after Arduino WebServer has received the body, validates backup format, applies atomically, and triggers post-restore sync. The current framework buffers request bodies before route dispatch; this limit is not a pre-allocation transport cap.
**Params:** JSON body with recognized `_type` field (see `backup_payload_builder.h` for accepted types). Restore accepts current `wifiStaSlots` backups and legacy single-`wifiClientSSID` / `stationSSID` + `stationPassword` backups; legacy `enableWifiAtBoot` fields from older backups are ignored.
**Note:** Restore preserves the existing AP password when `apPassword` is omitted and preserves stored STA passwords for slots whose SSID matches an existing saved network when `passwordObf` is omitted. Explicit credential fields from trusted legacy or local-SD backups remain accepted for compatibility; non-matching networks require password re-entry.
**Response:** `{ "success": true, "message": "Settings restored successfully (N profiles)" }` (200) or `{ "success": false, "error": "..." }` (400, 413, 500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `backup_api_service.cpp`.

## Debug / observability

HTTP debug, scenario-playback, and perf-file management endpoints are not
registered in the production maintenance server. Runtime observability is kept
through SD perf CSV logging and serial logs instead of live HTTP polling.

## WiFi management

### GET `/api/wifi/status`
**Description:** STA WiFi status — enabled flag, primary saved SSID alias, current state, connected SSID/IP/RSSI/slot if connected, scan-running flag.
**Response:** `{ "enabled": bool, "savedSSID": string, "state": string, "connectedSSID": string (if connected), "connectedSlotIndex": int (if connected to a known saved slot), "ip": string (if connected), "rssi": int (if connected), "scanRunning": bool }` (200) or `{ "success": false, "message": "Runtime unavailable" }` (500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_client_api_service.cpp`.

### POST `/api/wifi/scan`
**Description:** Starts a WiFi network scan; returns completed results if they are already available, or in-progress status otherwise.
**Response:** `{ "scanning": false, "networks": [ { "ssid": string, "rssi": int, "secure": bool }, ... ] }` (200) when complete, or `{ "scanning": true, "networks": [] }` (200) while in progress, or error (500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_client_api_service.cpp`.

### GET `/api/wifi/scan`
**Description:** Polls current WiFi scan status/results without starting a new scan.
**Response:** Same shape as `POST /api/wifi/scan`; returns `{ "scanning": false, "networks": [] }` when no scan is running and no completed results are available.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_client_api_service.cpp`.

### POST `/api/wifi/disconnect`
**Description:** Disconnects from current STA network.
**Response:** `{ "success": true, "message": "Disconnected" }` (200).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_client_api_service.cpp`.

### POST `/api/wifi/forget`
**Description:** Forgets all saved STA WiFi slots and their stored passwords, disables the STA client, and leaves the AP available.
**Response:** `{ "success": true, "message": "WiFi credentials forgotten" }` (200).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_client_api_service.cpp`.

### POST `/api/wifi/enable`
**Description:** Enables or disables the STA WiFi client.
**Params:** JSON body with `enabled` (bool, required).
**Response:** `{ "success": true, "message": "WiFi client enabled|disabled" }` (200) or error (400, 500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_client_api_service.cpp`.

### GET `/api/wifi/networks`
**Description:** Lists the fixed saved STA network slots. Available only in maintenance mode. Passwords are never returned.
**Response:** `{ "slots": [ { "index": int, "ssid": string, "label": string, "priority": int, "hasPassword": bool, "lastConnectedAtSec": uint32, "configured": bool }, ... ] }` (200), `{ "success": false, "error": "maintenance_required", "message": "WiFi network management is available only in maintenance mode" }` (409), or `{ "success": false, "message": "Runtime unavailable" }` (500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_client_api_service.cpp`.

### POST `/api/wifi/networks`
**Description:** Upserts a saved STA network slot. Available only in maintenance mode. Omit `password` on update to preserve the existing stored password.
**Params:** JSON body with `ssid` (string, required), optional `index` (int; when omitted, existing matching SSID or first free slot is selected), optional `password` (string), optional `label` (string), optional `priority` (0-255).
**Response:** `{ "success": true, "index": int, "message": "WiFi network saved" }` (200), `{ "success": false, "error": "maintenance_required", "message": "WiFi network management is available only in maintenance mode" }` (409), `{ "success": false, "error": "network_save_failed", "message": "Failed to save WiFi network" }` (409), or parse/runtime errors (400, 500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_client_api_service.cpp`.

### POST `/api/wifi/networks/delete`
**Description:** Deletes one saved STA network slot and its stored password. Available only in maintenance mode.
**Params:** JSON body with `index` (int, required).
**Response:** `{ "success": true, "index": int, "message": "WiFi network deleted" }` (200), `{ "success": false, "error": "maintenance_required", "message": "WiFi network management is available only in maintenance mode" }` (409), `{ "success": false, "error": "network_delete_failed", "message": "Failed to delete WiFi network" }` (404), or parse/runtime errors (400, 500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_client_api_service.cpp`.

### POST `/api/wifi/networks/test`
**Description:** Starts a one-shot connection attempt for one saved STA network slot. Available only in maintenance mode.
**Params:** JSON body with `index` (int, required).
**Response:** `{ "success": true, "index": int, "message": "Connecting..." }` (200), `{ "success": false, "error": "maintenance_required", "message": "WiFi network management is available only in maintenance mode" }` (409), `{ "success": false, "error": "network_test_failed", "message": "Failed to start saved network connection test" }` (404), or parse/runtime errors (400, 500).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `wifi_client_api_service.cpp`.

## OBD module

### GET `/api/obd/status`
**Description:** OBD runtime snapshot — connection, security, speed, scan state, error counters, state machine, timing.
**Response:** `{ "enabled": bool, "connected": bool, "securityReady": bool, "encrypted": bool, "bonded": bool, "speedValid": bool, "speedMph": float, "speedAgeMs": ulong, "rssi": int8, "scanInProgress": bool, "manualScanPending": bool, "savedAddressValid": bool, "savedAddress": string (if valid), "connectAttempts": int, "connectSuccesses": int, "connectFailures": int, "securityRepairs": int, "initRetries": int, "pollCount": ulong, "pollErrors": ulong, "staleSpeedCount": int, "consecutiveErrors": int, "bufferOverflows": int, "commandInFlight": string, "commandInFlightRaw": int, "lastConnectStartMs": ulong, "lastConnectSuccessMs": ulong, "lastFailureMs": ulong, "lastBleError": int, "lastSecurityError": int, "lastFailureRaw": int, "state": int }` (200) or `{ "error": "maintenance_mode", "message": "OBD runtime endpoints are not available in maintenance mode" }` (409).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `obd_api_service.cpp`.

### GET `/api/obd/devices`
**Description:** Saved OBD device list.
**Response:** `{ "devices": [ { "address": string, "name": string, "connected": bool, "active": bool }, ... ], "count": int }` (200).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `obd_api_service.cpp`.

### GET `/api/obd/config`
**Description:** OBD configuration — enabled, RSSI threshold, scan/retry timing, V1-coordination timing.
**Response:** `{ "enabled": bool, "minRssi": int8, "obdScanWindowMs": uint32, "obdRetryIntervalMs": uint32, "proxyOpenWindowMs": uint32, "wifiOpenTimeoutMs": uint32, "v1SettleQuietMs": uint32, "v1SettleFallbackMs": uint32, "cycleTeardownAckTimeoutMs": uint32 }` (200).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `obd_api_service.cpp`.

### POST `/api/obd/devices/name`
**Description:** Saves a custom name for the saved OBD device.
**Params:** `address` (string, required, must match saved), `name` (string, optional, max 32 chars).
**Response:** `{ "success": true }` (200), `{ "error": "Missing address" }` (400), or `{ "error": "Saved OBD device not found" }` (404).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `obd_api_service.cpp`.

### POST `/api/obd/scan`
**Description:** Requests a manual OBD device discovery scan.
**Response:** `{ "success": true, "requested": true, "scanInProgress": bool, "message": "..." }` (200), `{ "error": "maintenance_mode", "message": "OBD runtime endpoints are not available in maintenance mode" }` (409), `{ "success": false, "message": "OBD is disabled" }` (409), or `{ "success": false, "message": "OBD scan already requested or in progress" }` (409).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `obd_api_service.cpp`.

### POST `/api/obd/forget`
**Description:** Forgets the saved OBD device.
**Response:** `{ "success": true }` (200) or `{ "error": "maintenance_mode", "message": "OBD runtime endpoints are not available in maintenance mode" }` (409).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `obd_api_service.cpp`.

### POST `/api/obd/config`
**Description:** Updates OBD configuration; accepts any subset of fields with value clamping.
**Params:** `enabled` (bool), `minRssi` (int -90 to -40), `obdScanWindowMs` (int ms, clamped to 1000-60000), `obdRetryIntervalMs` (int ms, clamped to 30000-600000), `proxyOpenWindowMs` (int ms, clamped to 1000-300000), `wifiOpenTimeoutMs` (int ms, clamped to 1000-120000), `v1SettleQuietMs` (int ms, clamped to 100-5000), `v1SettleFallbackMs` (int ms, clamped to 500-10000), `cycleTeardownAckTimeoutMs` (int ms, clamped to 25-1000). Out-of-range timing values are accepted and clamped into range (see `src/settings_sanitize.h`).
**Mode rule:** Setting `enabled=true` selects OBD / Standalone mode and disables BLE proxy. Setting `enabled=false` only turns OBD off; use `POST /api/device/settings` with `proxy_ble=true` to select Proxy / App mode. If a proxy client is connected, OBD runtime work is dropped to idle.
**Response:** `{ "success": true }` (200) or `{ "error": "Invalid JSON" | "Missing JSON body" }` (400).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `obd_api_service.cpp`.

## ALP module

### GET `/api/alp/status`
**Description:** ALP runtime module snapshot for diagnostics/UI; safe to call from main loop.
**Response:** ALP status JSON (200), `{ "error": "maintenance_mode", "message": "ALP runtime status is not available in maintenance mode" }` (409), or `{ "error": "alp runtime not wired" }` (503). Exact key set defined by the snapshot function in `alp_runtime_module.cpp`.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `alp_api_service.h`.

## GPS module

### GET `/api/gps/config`
**Description:** Current GPS settings.
**Response:** `{ "gpsEnabled": bool, "gpsBaud": uint32, "gpsEnablePinActiveHigh": bool, "gpsLogUtcToPerf": bool, "gpsLogUtcToAlp": bool }` (200).
**Note:** `gpsEnablePinActiveHigh` is a deprecated compatibility field. GPS EN is not driven on supported hardware; the value is served as normalized `true`, and submitted values are accepted but ignored.
**Source:** route registration in `src/wifi_routes.cpp`, delegate `gps_api_service.h`.

### POST `/api/gps/config`
**Description:** Saves GPS settings (any subset of fields).
**Params:** JSON body with GPS config fields.
**Note:** `gpsEnablePinActiveHigh` is deprecated/no-op. Old clients may include it, but submitted values do not change live state or persisted settings.
**Response:** `{ "success": true }` (200; in maintenance mode includes `"message": "GPS settings saved; live runtime resumes on next normal boot."`), `{ "error": "Invalid JSON", "message": "Invalid JSON" }` (400), or `{ "error": "gps runtime not wired" }` (503).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `gps_api_service.h`.

### GET `/api/gps/status`
**Description:** Live `GpsRuntimeStatus` snapshot.
**Response:** Live GPS status JSON (200) with enabled/fix/parser fields plus `counters`, or `{ "error": "maintenance_mode", "message": "GPS runtime status is not available in maintenance mode" }` (409), or `{ "error": "gps runtime not wired" }` (503).
**Source:** route registration in `src/wifi_routes.cpp`, delegate `gps_api_service.h`.

---

# BLE GATT Proxy Service

The device exposes a BLE GATT server that mirrors the Valentine 1's own service UUID and characteristic layout. This lets companion phone apps (V1Driver, V1connection LE, JBV1) connect to this device as if it were a V1 directly. All write characteristics forward client writes to the upstream V1; notify characteristics relay V1's notifications back to the client.

The proxy is active only in Proxy / App mode (`proxy_ble=true`) during normal drive runtime. In that mode OBD is disabled, local V1 Simple write features are suppressed while a proxy phone is connected, and the companion app is trusted to own muting/V1-control behavior. Display and logging remain active; WiFi and the Web UI remain off until the user deliberately reboots into maintenance mode. See `docs/CONNECTIVITY_MODES.md`.

**Service:** `92A0AFF4-9E05-11E2-AA59-F23C91AEC05E` — defined as `V1_SERVICE_UUID` in `include/config.h`. This is Valentine Research's published V1 service UUID; the proxy mimics it exactly so the V1 attribute table presented to phone apps matches what they expect.

## Characteristics

### `92A0B2CE-9E05-11E2-AA59-F23C91AEC05E` — Display data (short notify)
**Direction:** Server → Client (NOTIFY).
**Properties:** READ, NOTIFY.
**Carries:** V1's primary `infDisplayData` packet — bands, arrows, frequency, bogey counter, signal bars, mute, status. Notified at V1's display-update cadence. The proxy copies the raw V1 packet to the phone queue unchanged. Byte-format summary is tracked in `docs/V1_PROTOCOL_REFERENCES.md#infdisplaydata`; parsing lives in `src/packet_parser.cpp`.
**Source:** characteristic definition in `src/ble_proxy.cpp`.

### `92A0B4E0-9E05-11E2-AA59-F23C91AEC05E` — Long alert/response (long notify)
**Direction:** Server → Client (NOTIFY).
**Properties:** READ, NOTIFY.
**Carries:** Multi-fragment V1 responses — alert tables, version strings, sweep definitions, voltage, etc. Notified when a long packet arrives from V1.
**Source:** characteristic definition in `src/ble_proxy.cpp`.

### `92A0B6D4-9E05-11E2-AA59-F23C91AEC05E` — Client write (short)
**Direction:** Client → Server (WRITE_NR).
**Properties:** WRITE_NR (write without response).
**Carries:** Client commands forwarded to the V1 — request packets, control commands, settings writes. Handled by `ProxyWriteCallbacks::onWrite`; bytes are copied and forwarded unchanged.
**Source:** characteristic definition and write callback in `src/ble_proxy.cpp`.

### `92A0B8D2-9E05-11E2-AA59-F23C91AEC05E` — Client write (long)
**Direction:** Client → Server (WRITE_NR).
**Properties:** WRITE_NR.
**Carries:** Long-form client commands (multi-fragment). Same callback as the short-write characteristic.
**Source:** characteristic definition in `src/ble_proxy.cpp`.

### `92A0BCE0-9E05-11E2-AA59-F23C91AEC05E` — Compatibility-stub notify
**Direction:** N/A — never written, never notified on.
**Properties:** READ, NOTIFY.
**Carries:** Nothing. The characteristic exists in the attribute table because companion apps (V1Driver, JBV1) verify its presence when enumerating the V1 service. Defined as `V1_NOTIFY_ALT_UUID` in `include/config.h`. Do not remove — apps will fail to recognize the device as a V1.
**Source:** characteristic definition in `src/ble_proxy.cpp`.

### `92A0BAD4-9E05-11E2-AA59-F23C91AEC05E` — Alternate write (with response)
**Direction:** Client → Server (WRITE).
**Properties:** WRITE, WRITE_NR.
**Carries:** Client commands using the response variant of write. Same callback as the other write characteristics.
**Source:** characteristic definition in `src/ble_proxy.cpp`.

## Discovery and pairing

The proxy advertises with flags `0x06` (LE General Discoverable, BR/EDR Not Supported) — required for reliable discovery on some Android handsets that otherwise cache the device as DUAL-mode and attempt BR/EDR connections. The full V1 service UUID is included in advertising data. Companion apps see the device as a V1 and complete connection / characteristic enumeration normally.

**Source:** advertising config in `src/ble_proxy.cpp`.

---

# Settings Wire Format

The device persists configuration in NVS (Non-Volatile Storage) via the `V1Settings` struct in `src/settings.h`. That struct is the canonical source of truth: it is a mostly flat, grouped set of fields covering WiFi/client, BLE proxy, display/colors/visibility, quiet controls, auto-push slots, power, OBD, ALP, GPS, and diagnostics. It also contains small helper slot-view structs for per-slot access. Avoid duplicating exact field counts here; they drift as settings evolve.

The wire format **is the JSON shape documented in each HTTP settings endpoint above** — there is no separate settings serialization protocol. The keys you see in `/api/device/settings`, `/api/display/settings`, `/api/quiet/settings`, `/api/obd/config`, `/api/gps/config`, `/api/v1/profile`, `/api/autopush/slot`, etc., correspond directly to fields in the `V1Settings` struct.

To find a specific settings key:

- Check the relevant HTTP endpoint section above (most user-visible settings live there).
- For programmatic access, search `src/settings.h` for the field name.
- Per-group serialization code lives in `src/modules/wifi/wifi_*_api_service.cpp`.

Persistence: settings are saved to NVS on POST. The system also supports SD-card backups via `/api/settings/backup-now` and restore via `/api/settings/restore`.

---

# Notes on this document

Maintained by hand. When adding a route or characteristic, also update this file with a stable `Source:` line citing the registration or handler file, not an exact line number. Per-module C++ APIs do not belong here — they belong in `api.md` files inside each module's directory under `src/modules/`.

When in doubt about V1 protocol details (byte layouts, characteristic semantics, blink cadence, etc.), cite `docs/V1_PROTOCOL_REFERENCES.md` by anchor first. Maintainers can re-check those tracked summaries against local official PDFs, but fresh clones should not depend on gitignored scratch files for rationale.
