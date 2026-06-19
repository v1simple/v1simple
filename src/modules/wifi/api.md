# wifi Module API

The largest module in the project (27 headers). It covers everything WiFi-related: AP and STA lifecycle orchestration, the heap-guard / boot / priority / cadence policy decisions that gate when WiFi is allowed to run, the per-tick orchestrator that wires those decisions, and **all** HTTP API handlers (one file per endpoint group).

The HTTP-handler files are referenced from `src/wifi_routes.cpp`. Per-route documentation (method, path, params, response shape) lives in **`docs/API.md`** at the project root — that is the user-facing API reference. This file documents the *internal* shape of the wifi module: the runtime/orchestration/policy classes, the helper utilities, and the integration points the HTTP handlers depend on.

## Files (categorized)

### Runtime / orchestration / policy

| File | Role |
|---|---|
| `wifi_runtime_module.{h,cpp}` | Main runtime — per-tick state pump. |
| `wifi_orchestrator_module.{h,cpp}` | Higher-level orchestration — owns dependencies, drives the runtime. |
| `wifi_process_cadence_module.{h,cpp}` | Decides when `WiFiManager::process()` may run. |
| `wifi_boot_policy.{h,cpp}` | Boot-time policy — when may WiFi auto-start? |
| `wifi_priority_policy_module.{h,cpp}` | Cross-subsystem priority — should WiFi yield to BLE/OBD? |
| `wifi_auto_start_module.{h,cpp}` | Auto-start gate evaluation. |
| `wifi_auto_timeout_module.{h,cpp}` | AP idle timeout decision. |
| `wifi_heap_guard_module.{h,cpp}` | Heap-pressure check — is there enough memory to run dual-radio? |
| `wifi_visual_sync_module.{h,cpp}` | Coordinates the WiFi indicator with display preview / boot splash. |
| `wifi_stop_reason_module.{h,cpp}` | Classifies why WiFi was stopped (Timeout, NoClients, LowDma, etc.). |

### Helpers

| File | Role |
|---|---|
| `wifi_static_path_guard.{h,cpp}` | Path-traversal and static-file exposure guard for LittleFS-served UI assets. |
| `wifi_json_document.{h,cpp}` | PSRAM-aware ArduinoJson allocator. |
| `wifi_api_response.{h,cpp}` | JSON-response helpers shared across handlers. |
| `backup_snapshot_cache.{h,cpp}` | TTL-cached backup-snapshot bytes for `/api/settings/backup`. |

### HTTP API handlers (per endpoint group)

| File | Endpoints |
|---|---|
| `wifi_status_api_service.{h,cpp}` | `/api/status`. |
| `wifi_settings_api_service.{h,cpp}` | `/api/device/settings`. |
| `wifi_v1_profile_api_service.{h,cpp}` | `/api/v1/profile*`, `/api/v1/pull`, `/api/v1/push`, `/api/v1/current`. |
| `wifi_v1_devices_api_service.{h,cpp}` | `/api/v1/devices*`. |
| `wifi_autopush_api_service.{h,cpp}` | `/api/autopush/*`. |
| `wifi_display_colors_api_service.{h,cpp}` | `/api/display/settings*`, `/api/display/preview*`. |
| `wifi_quiet_api_service.{h,cpp}` | `/api/quiet/settings`. |
| `wifi_client_api_service.{h,cpp}` | `/api/wifi/*` (status, scan, disconnect, forget, enable, saved-network management). |
| `wifi_portal_api_service.{h,cpp}` | Captive-portal probes (`/ping`, `/generate_204`, `/hotspot-detect.html`, etc.). |
| `backup_api_service.{h,cpp}` | `/api/settings/backup`, `/api/settings/backup-now`, `/api/settings/restore`. |

For the full per-route HTTP reference (params, response shapes), see `docs/API.md`.

## Class: `WifiRuntimeModule` / `WifiOrchestrator`

**Headers:** `wifi_runtime_module.h`, `wifi_orchestrator_module.h`.

The orchestrator owns a `WiFiManager`, the policy modules, and dependency pointers. The runtime module is the per-tick pump that the loop calls. The split exists so the orchestration layer can hold long-lived state while the runtime module remains a thin coordinator.

### `struct WifiRuntimeContext`
**Source:** `wifi_runtime_module.h:5+`.

Per-tick input: `nowMs`, `v1ConnectedAtMs`, `enableWifi`,
`bleConnected`, `canStartDma`, `wifiAutoStartAllowed`, `wifiAutoStartDone`,
`wifiManualStartIntentLatched`, `skipLateNonCoreThisLoop`, `bleBackpressure`,
`overloadLateThisLoop`, `bleConnectBurstSettling`, `displayPreviewRunning`, and
`bootSplashHoldActive`. Aggregates everything the policy modules need to make a
decision. `canStartDma` is live-probed only while an auto/manual WiFi start is
otherwise eligible; in ineligible gates it may be the last cached snapshot value.

### `class WifiOrchestrator`
**Source:** `wifi_orchestrator_module.h:8+`.

Constructor takes `WiFiManager&`, `V1BLEClient&`, `PacketParser&`, `SettingsManager&` (and `QuietCoordinatorModule*` per forward decl). Wires the runtime + policy modules.

The exact public surface is in the header — the loop calls `orchestrator.process(nowMs, ...)` (or equivalent) and the orchestrator drives everything beneath it.

## Policy modules

Each policy module is a small, testable decision unit with a context struct in / decision struct out shape.

### `WifiProcessCadenceModule`
**Source:** `wifi_process_cadence_module.h`.

Decides whether `WiFiManager::process()` should run on this tick. `WifiProcessCadenceContext` has `nowProcessUs`, `minIntervalUs` (default 2000); `WifiProcessCadenceDecision` has `shouldRunProcess`.

### `namespace WifiBootPolicy`
**Source:** `wifi_boot_policy.h`.

Boot-time policy (when may WiFi auto-start?) as free functions in a namespace.

### `wifi_priority_policy_module.h`
**Source:** `wifi_priority_policy_module.h`.

Free function:
```cpp
bool isWifiProcessingEnabledPolicy(const WiFiManager&);
```
Centralizes the "may WiFi run right now?" predicate.

### `WifiAutoStartModule`
**Source:** `wifi_auto_start_module.h`.

#### `enum class WifiAutoStartGate : uint8_t`
Reasons manual WiFi start is gated or acted on: `Unknown`, `AlreadyDone`, `WifiDisabled`, `ManualStartNotRequested`, `WaitingCoordinatorOpen`, `WaitingDma`, `Starting`, `StartFailed`.

The module evaluates the gate condition each tick.

### `WifiAutoTimeoutModule`
**Source:** `wifi_auto_timeout_module.h`.

#### `struct WifiAutoTimeoutInput`
`timeoutMins`, `setupModeActive`, `nowMs`, `setupModeStartMs`, `lastClientSeenMs`, `lastUiActivityMs`, `staCount`, `inactivityGraceMs`.

Decides when the AP should auto-shut-down due to idleness.

### `WifiHeapGuardModule`
**Source:** `wifi_heap_guard_module.h`.

#### `struct WifiHeapGuardInput`
`dualRadioMode`, `staRadioOn`, `staOnlyMode`, `freeInternal`, `largestInternal`, `criticalFree`, `criticalBlock`, `apStaFreeJitterTolerance`, `staOnlyBlockJitterTolerance`.

Decides whether the heap is healthy enough to keep WiFi running. Triggers shutdown if not.

### `WifiVisualSyncModule`
**Source:** `wifi_visual_sync_module.h`.

Coordinates the WiFi visual indicator with display preview and boot splash so the indicator doesn't flicker when those overlays are up.

### `wifi_stop_reason_module.h`
**Source:** `wifi_stop_reason_module.h`.

#### `enum class WifiStopReason : uint8_t`
Reasons WiFi was stopped: `Timeout`, `NoClients`, `NoClientsAuto`, `LowDma`, `Poweroff`, `Other`. Classification is used in perf counters and serial logs.

## Helpers

### `namespace WifiStaticPathGuard`
**Source:** `wifi_static_path_guard.h`.

#### `bool isSafe(const char* path)`
Path-traversal guard for static requests — rejects `..`, relative paths, backslashes, etc.

#### `bool isAllowedServedPath(const char* path)`
Allowlist for LittleFS paths the HTTP server may expose: shipped HTML pages,
`/_app/env.js`, `/_app/version.json`, `/_app/immutable/*`, `/audio/*.mul`, and
`/branding/*.png`.

#### `bool isHtmlPagePath(const char* path)`
Recognizes prerendered UI page routes that may be resolved to `.html` files.

### `namespace WifiJson`
**Source:** `wifi_json_document.h`.

PSRAM-aware ArduinoJson allocator. `WifiJson::Allocator::instance()` returns a singleton allocator that prefers PSRAM (`MALLOC_CAP_SPIRAM`) and falls back to internal RAM (`MALLOC_CAP_INTERNAL`). Used by every JSON-building handler so large responses don't pressure internal heap.

### `namespace WifiApiResponse`
**Source:** `wifi_api_response.h`.

Helpers shared across HTTP handlers:
- `void sendJsonDocument(WebServer&, int statusCode, const JsonDocument&)` — streams JSON to client.
- `void setErrorAndMessage(JsonDocument&, const char*)` — populates a standard error envelope.

### `namespace BackupApiService` (cache)
**Source:** `backup_snapshot_cache.h`.

#### `struct BackupSnapshotCache`
`data`, `capacity`, `length`, `inPsram` — TTL-cached backup-snapshot bytes for `/api/settings/backup`. Built once per revision; served from cache thereafter.

## HTTP API handlers (summary)

Each `wifi_*_api_service.h` exports one or more `handle*` free functions or namespaces. The pattern is uniform:

- Each handler takes `WebServer&` plus dependencies (settings, runtime modules) and a callbacks/`Runtime` struct for cross-cutting concerns (rate-limit, UI activity mark, post-change sync).
- Per-route detail (params, response shape, status codes) is in `docs/API.md`, not duplicated here.

Consult the header for each service to see its exact call signatures. The patterns are consistent — once you've read one (e.g. `wifi_status_api_service.h`), the rest follow.

## Dependencies

| External | Used by |
|---|---|
| ESP32 Arduino `WiFi` / `WebServer` | Runtime, orchestrator, all handlers. |
| `WiFiManager` (`src/wifi_manager*.cpp`) | Orchestrator owns it. |
| `V1BLEClient`, `PacketParser`, `SettingsManager` | Wide cross-module surface. |
| `QuietCoordinatorModule` | Some handlers (mute/volume) route through here. |
| `AutoPushModule`, `V1ProfileManager` | V1 profile / autopush handlers. |
| `ObdRuntimeModule`, `AlpRuntimeModule`, `GpsRuntimeModule` | Their respective status handlers. |
| `ArduinoJson` | All JSON-building handlers. |
| PSRAM (preferred) / internal heap | `WifiJson::Allocator`. |

## Notes for maintainers

The HTTP-handler files are intentionally thin — most of their logic is parameter parsing, validation, and JSON serialization. Real business logic lives in the runtime modules they call. Don't add side-effect logic to a handler beyond what's necessary to translate HTTP ↔ runtime.

`WifiJson::Allocator` is the right place for any large JSON response. Don't allocate `JsonDocument` with `DynamicJsonDocument` constructor variants — use the allocator so PSRAM is preferred.

The policy split (cadence, boot, priority, auto-start, auto-timeout, heap guard) is the result of a previous refactor where a single function decided everything and was untestable. Keep the policy modules small and pure. If you find yourself adding cross-policy state, that's a smell — push the state into the runtime module and let policies stay stateless.

`WifiStaticPathGuard::isSafe` is the one place path-traversal is checked, and
`WifiStaticPathGuard::isAllowedServedPath` is the static-file exposure
allowlist. If you add a new static-file route, route it through both policies.
Bypassing either one is a security bug.

The backup snapshot cache TTL is keyed off settings/profile revision, not wall time — the cache stays valid until the underlying state changes. Don't add a wall-time TTL on top.

Per-route HTTP reference is in `docs/API.md`. When adding a new route or changing a response shape, update both that file and the handler — the convention is route docs in `docs/API.md`, internal module structure here.
