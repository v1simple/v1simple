# obd Module API

OBD-II integration via ELM327-compatible BLE adapter (OBDLink CX). Provides vehicle speed for the speed-mute feature, runs an independent BLE client that coordinates with the V1 BLE pipeline (V1 has priority on contended scan windows), parses ELM327 responses, and exposes runtime status to the HTTP API.

OBD is part of the explicit OBD / Standalone operating mode. Selecting Proxy / App mode disables OBD, and a connected proxy client causes the OBD runtime to stop scanning, cancel pending connects, disconnect the OBD BLE client if active, clear transient state, and return to idle.

The OBD pipeline owns its own NimBLE client — it does **not** share `V1BLEClient`. This is structural: OBD and V1 connection lifecycles are independent and can race; the arbitration enum (`obd_ble_arbitration.h`) handles the cases where they do.

## Files

| File | Class / role |
|---|---|
| `obd_runtime_module.{h,cpp}` | Main runtime — state machine, scan/connect orchestration, speed publication. |
| `obd_runtime_state_machine.cpp` | State machine implementation (no separate header — internal). |
| `obd_runtime_commands.cpp` | Command/PID dispatch plus failure-reason classification and error counters. |
| `obd_runtime_transport.cpp` | BLE write/read transport layer. |
| `obd_ble_client.{h,cpp}` | OBD-owned `NimBLEClient` + scan/disconnect callbacks. Independent of `V1BLEClient`. |
| `obd_ble_arbitration.h` | Arbitration enum for V1 ↔ OBD scan/connect contention. |
| `obd_elm327_parser.{h,cpp}` | ELM327 response parser (PIDs, DIDs, VIN, temperatures). |
| `obd_settings_sync_module.{h,cpp}` | Watches settings for OBD-config changes; debounces and calls runtime hooks. |
| `obd_scan_policy.h` | All timing constants for scan/connect/poll/backoff. |
| `obd_string_utils.h` | Small string helpers shared across the OBD sources. |
| `obd_api_service.{h,cpp}` | HTTP handlers for `/api/obd/*`. |

## Public types (selected)

#### `enum class ObdConnectionState : uint8_t`
**Source:** `obd_runtime_module.h:13-25`.
States: `IDLE`, `WAIT_BOOT`, `SCANNING`, `CONNECTING`, `SECURING`, `DISCOVERING`, `AT_INIT`, `POLLING`, `ERROR_BACKOFF`, `DISCONNECTED`, `ECU_IDLE`.

#### `enum class ObdCommandKind : uint8_t`
**Source:** `obd_runtime_module.h:27+`.
Active command type — `NONE`, `AT_INIT`, `SANITY`, etc. Used in status snapshot to indicate what's in flight.

#### `enum class ObdBleArbitrationRequest : uint8_t`
**Source:** `obd_ble_arbitration.h:5-9`.
- `NONE`
- `HOLD_PROXY_FOR_AUTO_OBD` — auto OBD wants to scan; hold proxy advertising open.
- `PREEMPT_PROXY_FOR_MANUAL_SCAN` — user-requested scan; preempt the proxy.

#### `struct ObdRuntimeStatus`
**Source:** `obd_runtime_module.h`.
The full snapshot consumed by `/api/obd/status`. Fields: enabled, connected, securityReady, encrypted, bonded, speedValid, speedMph, speedAgeMs, speedSampleTsMs, rssi, scan/connect counters, error counters, state-machine fields, timing snapshots. See header for the complete list.

#### `struct ObdRuntimeReadiness` / `struct ObdBleContext` (internal context structs)
Used by the state machine for boot dwell, V1 contention, scan-allowance decisions. Members include `bootReady`, `v1Connected`, `bleScanIdle`, `proxyAdvertising`, `obdScanAllowed`, etc.

#### `struct Elm327ParseResult`
**Source:** `obd_elm327_parser.h:6-16`.
Parsed ELM327 frame — `valid`, `service`, `pid`, `did`, `dataBytes[32]`, `dataLen`, `noData`, `error`, `busInit`.

#### `struct Elm327VinParseResult`
**Source:** `obd_elm327_parser.h:18-23`.
VIN-specific parse result.

#### `enum class Elm327TempDecodeFormat : uint8_t`
**Source:** `obd_elm327_parser.h:25-29`.
`U8_OFFSET40`, `U16_DIV10_OFFSET40`, `U16_RAW_OFFSET40` — temperature decode variants.

## Class: `ObdRuntimeModule`

**Header:** `src/modules/obd/obd_runtime_module.h`.

### Lifecycle

#### `void begin(ObdBleClient* bleClient, ...)`
**Source:** `obd_runtime_module.h`.
Wires the OBD-owned BLE client and any other dependencies.

#### `void setEnabled(bool enabled)` / `bool isEnabled() const`
Runtime enable/disable.
**Source:** `obd_runtime_module.h:142-144`.

#### `void setMinRssi(int8_t minRssi)`
Adjusts the RSSI gate used by the scan callback (default `-80` per `obd_ble_client.h:21`).
**Source:** `obd_runtime_module.h:143`.

### Pump

#### `void update(uint32_t nowMs, const ObdBleContext& bootReadyContext)`
Main pump. Drives the state machine using BLE / V1 / proxy context.
If `bootReadyContext.proxyClientConnected` is true, the pump treats the companion app as the authority and drops all OBD work to idle before any scan/connect/poll transition can run.
**Source:** `obd_runtime_module.h:136`.

#### `void update(uint32_t nowMs, bool bootReady, bool v1Connected, bool bleScanIdle)`
Convenience overload.
**Source:** `obd_runtime_module.h:137`.

### Scan / connect control

#### `bool startScan()` / `void stopActiveScan()`
Programmatic scan start/stop.
**Source:** `obd_runtime_module.h:151-152`.

#### `bool requestManualPairScan(uint32_t nowMs)`
User-requested scan via `/api/obd/scan`. Returns false if already in progress / disabled.
**Source:** `obd_runtime_module.h:153`.

#### `void cancelPendingConnect()` / `void forgetDevice()`
Cancel outstanding connect attempt; forget saved device.
**Source:** `obd_runtime_module.h:154-155`.

#### `bool isScanStopped() const` / `bool isConnectIdle() const`
State predicates.
**Source:** `obd_runtime_module.h:156-157`.

### Status

#### `ObdRuntimeStatus snapshot(uint32_t nowMs) const`
Builds the JSON-ready status snapshot. Used by `/api/obd/status`.
**Source:** `obd_runtime_module.h:140`.

#### `bool getFreshSpeed(uint32_t nowMs, float& speedMphOut, uint32_t& tsMsOut) const`
Latest speed if younger than `obd::SPEED_MAX_AGE_MS` (3000 ms). Used by speed-mute.
**Source:** `obd_runtime_module.h:146`.

#### `const char* getSavedAddress() const` / `uint8_t getSavedAddrType() const`
Saved device address + type (BLE address-type byte).
**Source:** `obd_runtime_module.h:148-149`.

#### `ObdConnectionState getState() const`
Current state-machine state.
**Source:** `obd_runtime_module.h:171`.

### BLE callbacks (called from `ObdBleClient` callbacks)

#### `void onDeviceFound(const char* name, const char* address, int rssi, uint8_t addrType = 0)`
Scan callback hit — the OBD scan callback found a candidate.
**Source:** `obd_runtime_module.h:159`.

#### `void onBleDisconnect(int reason = 0)`
BLE link dropped. Triggers state-machine transition to `DISCONNECTED`.
**Source:** `obd_runtime_module.h:160`.

#### `void onBleData(const uint8_t* data, size_t len)`
Data received from ELM327. Forwarded to the parser path.
**Source:** `obd_runtime_module.h:161`.

### Test seams (UNIT_TEST only)

The test surface is unusually large because the state machine has many failure paths to cover. Selected test seams (header lines 164-185):

- `injectSpeedForTest(float, uint32_t)`
- `forceStateForTest(ObdConnectionState, uint32_t)`
- `setConsecutiveErrorsForTest(uint32_t)` / `setBackoffCyclesForTest(uint32_t)` / `setLastFailureForTest(ObdFailureReason)` etc.
- Counter accessors — `getStartScanCallCountForTest()`, `getConnectCallCountForTest()`, etc.
- Last-command introspection — `getLastCommandForTest()`.

## Class: `ObdBleClient`

**Header:** `src/modules/obd/obd_ble_client.h`.

OBD-owned `NimBLEClient` plus scan/disconnect callbacks. **Created once at boot, never deleted** — the comment at line 5 calls out the heap-safety rule. NimBLE does not handle client teardown gracefully; recreating it leaks resources or crashes on some firmware versions.

### `class ObdScanCallback : public NimBLEScanCallbacks`
**Source:** `obd_ble_client.h:14`.

- `void configure(ObdRuntimeModule* parent, int8_t minRssi)`
- `void onResult(const NimBLEAdvertisedDevice* device) override` — filters for `OBDLink CX` name + RSSI gate.
- `void onScanEnd(const NimBLEScanResults& results, int reason) override`.

### `class ObdClientCallback : public NimBLEClientCallbacks`
**Source:** `obd_ble_client.h:26`.

- `void configure(ObdBleClient* owner, ObdRuntimeModule* parent)`
- `void onConnect(NimBLEClient*) override`
- `void onConnectFail(NimBLEClient*, int reason) override`

(Plus other NimBLE callback overrides for disconnect/MTU/security — see header.)

## ELM327 parser — `obd_elm327_parser.{h,cpp}`

Free functions that decode ELM327 responses. No class; called from the runtime module. Public functions (in the header):

- ELM327 frame parser → `Elm327ParseResult`.
- VIN-specific parser → `Elm327VinParseResult`.
- Temperature decoders for the three formats above.

The exact function signatures live in the header — search for `parse` and `decode` prefixes.

## Class: `ObdSettingsSyncModule`

**Header:** `src/modules/obd/obd_settings_sync_module.h:9`.

Watches `SettingsManager` for OBD-related changes (saved address, address type) and calls into `ObdRuntimeModule` after a stability window — this prevents thrash if the user is mid-edit on the web UI.

#### `void begin(SettingsManager* settings, ObdRuntimeModule* obdRuntimeModule)`
**Source:** `obd_settings_sync_module.h:11`.

#### `void process(uint32_t nowMs)`
Per-tick poll. Detects changes via in-memory `Snapshot` comparison; debounces over `STABILITY_WINDOW_MS = 5000`.
**Source:** `obd_settings_sync_module.h:12`.

### Constants
- `ADDR_BUF_LEN = 18`, `STABILITY_WINDOW_MS = 5000`.
**Source:** `obd_settings_sync_module.h:21-22`.

## Constants — `obd_scan_policy.h`

Namespace `obd::`. All scan/connect/poll/backoff timing in one place. Selected constants:

- `SCAN_DURATION_MS = 5000`
- `DEVICE_NAME_CX = "OBDLink CX"`
- `CONNECT_TIMEOUT_MS = 5000`
- `RECONNECT_BACKOFF_MS = 5000`
- `MAX_DIRECT_CONNECT_FAILURES = 3`
- `POST_CONNECT_SETTLE_MS = 500`
- `SECURITY_TIMEOUT_MS = 3000`
- `POST_SUBSCRIBE_SETTLE_MS = 150`
- `POST_BOOT_DWELL_MS = 10000` — V1 gets first crack at the radio for 10 s after boot.
- `POLL_INTERVAL_MS = 500`, `POLL_TIMEOUT_MS = 1000`
- `SEARCH_EXTENDED_TIMEOUT_MS = 10000`
- `POLL_COMMAND_RETRIES = 1`
- `SPEED_MAX_AGE_MS = 3000`
- `MAX_CONSECUTIVE_ERRORS = 5`

**Source:** `obd_scan_policy.h:8-30`.

## Namespace: `ObdApiService`

**Header:** `src/modules/obd/obd_api_service.h`.

HTTP handlers for `/api/obd/*` routes. All take a `Runtime` callbacks struct.

#### `struct Runtime`
**Source:** `obd_api_service.h:10-15`.

- `void (*markUiActivity)(void* ctx)`
- `bool (*checkRateLimit)(void* ctx)`
- `void (*syncAfterConfigChange)(void* ctx)` — hook to retrigger settings sync after config save.
- `void* ctx` — shared context pointer for the callbacks.
- `bool maintenanceBootActive` — when set, `/api/obd/status`, `/api/obd/scan` return the structured maintenance 409 and forget/config persist without live runtime work.

#### Handlers

- `handleApiConfigGet(WebServer&, SettingsManager&, const Runtime&)` — `GET /api/obd/config`.
- `handleApiStatus(WebServer&, ObdRuntimeModule&, const Runtime&)` — `GET /api/obd/status`.
- `handleApiDevicesList(WebServer&, ObdRuntimeModule&, SettingsManager&, const Runtime&)` — `GET /api/obd/devices`.
- `handleApiDeviceNameSave(WebServer&, ...)` — `POST /api/obd/devices/name`.
- `handleApiScan(WebServer&, ...)` — `POST /api/obd/scan`.
- `handleApiForget(WebServer&, ...)` — `POST /api/obd/forget`.
- `handleApiConfig(WebServer&, ...)` — `POST /api/obd/config`.

**Source:** `obd_api_service.h`.

## Dependencies

| External | Used by |
|---|---|
| NimBLE (`NimBLEDevice`, `NimBLEClient`, scan callbacks) | `ObdBleClient`, `ObdRuntimeModule`. |
| `SettingsManager` | API service, settings-sync module. |
| `WebServer` | API service. |
| `V1BLEClient` (indirectly) | Arbitration via `ObdBleContext` flags. |
| `ProxyServer` (indirectly) | `proxyAdvertising` / `proxyClientConnected` flags. |

## Notes for maintainers

The OBD client and the V1 client are intentionally independent. Don't try to share `NimBLEClient` between them — NimBLE does not multiplex cleanly, and past attempts produced disconnect storms. The arbitration is at the *scan window* level, not the client level: V1 always wins contended scans; OBD waits or preempts based on user intent.

The scan/connect state machine is the most complex part of this module. It lives in `obd_runtime_state_machine.cpp` and is tested heavily via the `*ForTest` seams. Don't add transitions without adding the corresponding test — past regressions traced to under-tested edge cases.

`ObdBleClient` is created once and never deleted (per heap-safety rule in the header comment). If you find code that destroys it, that's a bug — NimBLE client teardown does not work reliably on this platform.

`obd_scan_policy.h` is the single source for timing. If you need to tune any scan/connect timing, change it there, not inline in the state machine — tests refer to the constants by name.

The `ObdSettingsSyncModule` debounce window (5 s) exists because the web UI saves on every input keystroke. Without debounce, every character typed in the address field would trigger a runtime reconfiguration. If you reduce the window, expect thrash.
