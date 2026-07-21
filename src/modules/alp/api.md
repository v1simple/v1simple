# alp Module API

ALP (AL Priority) integration — listens to the laser jammer's serial protocol, decodes laser alert events (DLI / LID / Targeted / Warm-Up), exposes them to the display pipeline, and publishes a status surface for the HTTP API. Coordinates with V1 alert handling but is structurally independent from the V1 BLE pipeline.

The module is the authoritative source for laser-direction and laser-gun classification — V1 produces a generic "laser band" alert; ALP refines it with direction (Front/Side/Rear) and gun identification (Echo, Stealth, Photo, etc.).

## Files

| File | Purpose |
|---|---|
| `alp_runtime_module.{h,cpp}` | Main runtime class — UART listener, state machine, event publisher. |
| `alp_event_latch.{h,cpp}` | Persistence latch for laser events across raw-event gaps and post-close hold. |
| `alp_laser_event.h` | `AlpLaserEvent` POD struct — atomic snapshot of an active laser event. |
| `alp_sd_logger.{h,cpp}` | Optional SD-card structured logger for ALP frames, state transitions, and gun identification. |
| `alp_api_service.{h,cpp}` | HTTP handler for `/api/alp/status`. |

## Types

### `enum class AlpState : uint8_t`
**Source:** `alp_runtime_module.h:54-61`.
ALP runtime state (Idle / Warm-Up / Active / TearDown / NoiseWindow / etc. — see header for full enumeration).

### `enum class AlpGunType : uint8_t`
**Source:** `alp_runtime_module.h:68-78`.
Identified laser-gun model (Echo, Stealth, Photo, etc.). Forward-declared in `alp_laser_event.h:14` and `alp_sd_logger.h:27` to keep includes light.

### `enum class AlpLaserDirection : uint8_t`
**Source:** `alp_runtime_module.h:83-87`.
Laser direction (Front / Side / Rear / Unknown).

### `struct AlpLaserEvent`
**Source:** `alp_laser_event.h:17-24`.
Atomic snapshot of an active laser event. Five event-owned fields propagated from `AlpRuntimeModule` to `V1Display` via the render frame composer:
- `bool active` — session open?
- `AlpGunType gun`
- `AlpLaserDirection direction`
- `bool lidActive` — LID mode (vs DLI)
- `uint32_t openedAtMs`, `uint32_t closedAtMs`

This struct exists to replace a previous "three-flag dance" where direction, gun, and LID flags were propagated separately and could desync.

### `struct AlertSession`
**Source:** `alp_runtime_module.h`.
Per-session state — open/close timestamps, gun-identification timing, trigger and rearm counts. Read-only via `AlpRuntimeModule::currentSession()`.

## Class: `AlpRuntimeModule`

**Header:** `src/modules/alp/alp_runtime_module.h:186`.

The main runtime class. One instance, owned by main; injected into modules that need ALP state.

### Configuration constants

UART pinning, framing, and timing constants are public `static constexpr` members:

- `ALP_RX_PIN` — `2` (UART RX pin).
- `ALP_BAUD` — `19200`.
- `FRAME_LEN` — `4` bytes (byte0, byte1, byte2, checksum).
- `ALERT_BYTE0` / `ALERT_BYTE1` / `ALERT_BYTE2` — `0x98 / 0x00 / 0xE3` (ALERT trigger frame).
- `HEARTBEAT_SINGLE_0` / `HEARTBEAT_PAIRED_0` / `HEARTBEAT_TRIPLE_0` — `0xB0 / 0xB8 / 0xE0`.
- `HEARTBEAT_TIMEOUT_MS` — `3000`.
- `NOISE_WINDOW_MAX_MS` — `35000`.
- `TEARDOWN_TIMEOUT_MS` — `5000`.
- `ALERT_ACTIVE_TIMEOUT_MS` — `15000` (no `0x98` rearm in 15 s → teardown).
- `WARM_UP_PREAMBLE_WINDOW_MS` — `5000`.
- `WARM_UP_ENVELOPE_MS` — `35000`.
- See `alp_runtime_module.h:189-265` for the complete list.

### Lifecycle

#### `void begin(bool enabled, AlpSdLogger* sdLogger = nullptr)`
Initializes the UART listener and state machine. SD logger is optional; pass `nullptr` to disable structured logging.
**Source:** `alp_runtime_module.h:273`.

#### `void setEventBus(SystemEventBus* bus)`
Wires the system event bus for cross-module event publication (e.g. `ALP_STATE_CHANGED` for display wake).
**Source:** `alp_runtime_module.h:278`.

#### `void process(uint32_t nowMs)`
Pump method. Call once per main-loop tick. Drains the UART buffer, advances the state machine, fires events.
**Source:** `alp_runtime_module.h`.

### State queries

#### `AlpState getState() const`
Current state-machine state.
**Source:** `alp_runtime_module.h`.

#### `bool isEnabled() const`
Whether the module is enabled (per `begin(enabled, ...)`).
**Source:** `alp_runtime_module.h`.

#### `uint32_t lastUartByteMs() const`
Timestamp of last UART byte received. Useful for diagnosing dead serial.
**Source:** `alp_runtime_module.h`.

#### `uint32_t lastValidFrameMs() const`
Timestamp of last successfully-framed (non-error) ALP frame.
**Source:** `alp_runtime_module.h`.

#### `bool isAlertActive() const`
True when the state machine is in a session-open state.
**Source:** `alp_runtime_module.h`.

#### `AlpGunType lastIdentifiedGun() const`
Most recently identified laser gun (across all sessions).
**Source:** `alp_runtime_module.h`.

#### `uint32_t lastGunTimestampMs() const`
Timestamp when `lastIdentifiedGun()` was set.
**Source:** `alp_runtime_module.h`.

#### `uint8_t lastHeartbeatByte1() const`
Most recent heartbeat byte1 — `01`=Targeted, `02`=Warm-Up, `03`=DLI, `04`=LID. Used by display to color the ALP indicator badge.
**Source:** `alp_runtime_module.h`.

### Event surface (read by display pipeline)

#### `const AlpLaserEvent& currentEvent() const`
The currently active event snapshot. `currentEvent().active` indicates whether to render anything.
**Source:** `alp_runtime_module.h`.

#### `bool hasLaserEvent() const`
Canonical predicate for whether a live laser alert should be shown on the display.
**Source:** `alp_runtime_module.h`.

#### `const AlertSession& currentSession() const`
The current alert session — timing, counters, gun-identification status. Empty when no session is open.
**Source:** `alp_runtime_module.h`.

#### `bool ownsLaserDisplay() const`
True when ALP currently has authoritative ownership of the laser-band display slot. Read by the render frame composer to decide whether to overlay ALP gun/direction or fall back to V1's generic laser indication.
**Source:** `alp_runtime_module.h`.

### Logging

#### `void logDisplayDecision(uint32_t nowMs, const char* event, const char* detail)`
Records a display-side decision (e.g. "render ALP", "fall back to V1") into the SD log if enabled. Used for post-hoc diagnosis of display-vs-runtime divergence.
**Source:** `alp_runtime_module.h`.

### Test seam

#### `void testSyncCurrentEvent(uint32_t nowMs = 0)`
**Test only.** Forces a sync of `currentEvent_` from internal state — used by native tests that bypass the UART path.
**Source:** `alp_runtime_module.h`.

## Class: `AlpEventLatch`

**Header:** `src/modules/alp/alp_event_latch.h:15`.

Persistence latch for laser events. Mirrors the V1 alert persistence behavior but keeps the ALP event in its live-shaped form so the display can continue rendering the same laser frame through brief raw-event gaps and the shared post-close persist window.

#### `void setEvent(const AlpLaserEvent& ev)`
Stores the event to be persisted.
**Source:** `alp_event_latch.h:17`.

#### `void startPersistence(uint32_t nowMs)`
Begins the persistence window.
**Source:** `alp_event_latch.h:18`.

#### `bool shouldShowPersisted(uint32_t nowMs, uint32_t windowMs) const`
True if persistence is active and `nowMs - persistStartMs_ < windowMs`.
**Source:** `alp_event_latch.h:19`.

#### `void clearLatch()`
Ends persistence and clears the latched event.
**Source:** `alp_event_latch.h:20`.

#### `const AlpLaserEvent& latchedEvent() const`
Read the latched event. Only meaningful while `isLatched()`.
**Source:** `alp_event_latch.h:22`.

#### `bool isLatched() const`
True between `setEvent` and `clearLatch`.
**Source:** `alp_event_latch.h:23`.

## Class: `AlpSdLogger`

**Header:** `src/modules/alp/alp_sd_logger.h:30`.

Optional SD-card structured logger. Captures state transitions, heartbeats, frame errors, gun identifications, and session events to a CSV-like file for offline analysis. Disabled by default; enabled via `V1Settings::alpSdLogEnabled` from the `/alp` web UI page.

### Lifecycle

#### `void begin(bool enabled, bool sdReady, GpsTimePublisher* timePub = nullptr)`
Initializes the logger. Optional GPS time source for wall-clock timestamping; otherwise log uses `millis()`.
**Source:** `alp_sd_logger.h:45`.

#### `void setBootId(uint32_t id, uint32_t bootToken = 0)`
Stamps log entries with a boot identifier so multi-boot logs can be partitioned.
**Source:** `alp_sd_logger.h:50`.

#### `void setEnabled(bool enabled)` / `bool isEnabled() const`
Runtime enable/disable; effective only when `sdReady_` is also true.
**Source:** `alp_sd_logger.h:120`, `alp_sd_logger.h:109`.

#### `void drainAndClose(uint32_t timeoutMs)`
Flushes pending writes and closes the file. Called on power-off.
**Source:** `alp_sd_logger.h:123`.

### Log methods

The class exposes typed log methods rather than a generic write — each captures the relevant fields for its event type. See header lines 48–104 for full signatures:

- `logStateTransition(...)` — `AlpState` from→to with reason.
- `logHeartbeatByte1(...)` — heartbeat byte1 transitions.
- `logGunIdentified(...)` — gun identification with raw bytes.
- `logFrame(...)` — generic frame log (typed).
- `logHeartbeat(...)` — periodic heartbeat (rate-limited via `HEARTBEAT_LOG_INTERVAL_MS`, default 3000 ms).
- `logEvent(...)` — generic event with state context.
- `logSessionEvent(...)` — session-scoped event.

### Test seam

#### `void testClearLastLine()`
**Test only.** Resets internal de-duplication buffer for native tests.
**Source:** `alp_sd_logger.h:127`.

## Namespace: `AlpApiService`

**Header:** `src/modules/alp/alp_api_service.h`.

Thin wrapper over `AlpRuntimeModule` for the HTTP layer.

#### `void handleApiStatus(WebServer& server, AlpRuntimeModule* alpRuntime, void (*markUiActivity)(void* ctx), void* uiActivityCtx, bool maintenanceBootActive)`
Handler for `GET /api/alp/status`. Calls `markUiActivity` to keep the WiFi keep-alive signal current, rejects maintenance mode with 409 before consulting the live runtime, returns 503 when the runtime is absent in normal mode, and otherwise writes the runtime snapshot.
**Source:** `alp_api_service.h`.

### Threading note (from header)

The handler reads runtime fields without synchronization. This is safe because the ESP32 Arduino `WebServer` dispatches handlers synchronously from the main loop on Core 1 — the same task that runs `AlpRuntimeModule::process()`. If migrated to `AsyncWebServer` or pinned to Core 0, the snapshot path needs hardening.

## Dependencies

| Dependency | Purpose |
|---|---|
| `AlpSdLogger*` | Optional structured logging sink. |
| `SystemEventBus*` | Cross-module event publication. |
| `GpsTimePublisher*` | Wall-clock timestamping in SD logs (optional). |
| Hardware UART2 (RX pin 2, 19200 baud) | ALP serial input. |

## Notes for maintainers

State machine documentation is dense and lives in `alp_runtime_module.{h,cpp}`. Don't rewrite it here — link to it.

The render frame composer (`src/modules/display/render_frame_composer.cpp`) is the consumer of `AlpRuntimeModule::ownsLaserDisplay()` and `currentEvent()`. Changes to event semantics typically require a coordinated change there.

The `AlpEventLatch` is **separate** from V1 alert persistence (`src/modules/alert_persistence/`). The two systems are independent — the composer decides which one renders when both are active in the same frame.

Tracked ALP protocol evidence lives in `docs/ALP_PROTOCOL_EVIDENCE.md`.
Cite that file by section when adding behavior assertions in code comments — the FSD-pattern lesson applies to ALP as much as it applies to V1.
