# ble Module API

BLE-side runtime: queueing of incoming BLE notifications, framing into V1 packets, dispatch into the parser/display pipeline, connection-state tracking, and the cadence/watchdog logic that governs when state-machine work runs in the main loop.

This module wraps the lower-level `V1BLEClient` (in `src/ble_*.cpp`) — that class handles the actual NimBLE client and proxy server. The classes in this module sit *above* `V1BLEClient` and orchestrate when and how its data flows into the rest of the system.

When Proxy / App mode is selected, `V1BLEClient` exposes the raw V1 proxy service. Local V1 Simple write features are suppressed while a proxy phone is connected, but phone-originated proxy writes still flow through to the V1 unchanged.

## Files

| File | Class / role |
|---|---|
| `ble_queue_module.{h,cpp}` | RX queue + framing + parser dispatch. |
| `connection_state_module.{h,cpp}` | Connect/disconnect transitions, parser reset, data-request re-queue. |
| `connection_runtime_module.{h,cpp}` | Aggregates connection runtime signals into a snapshot for upstream consumers. |
| `connection_state_cadence_module.{h,cpp}` | When may `connection_state_module.process()` run? |
| `connection_state_dispatch_module.{h,cpp}` | Executes the cadence gate plus starvation watchdog. |

## Class: `BleQueueModule`

**Header:** `src/modules/ble/ble_queue_module.h:18`.

Owns the FreeRTOS queue that buffers raw BLE notifications between the NimBLE callback (interrupt-context) and the main-loop parser. Holds the running RX byte buffer and the framing logic that splits the byte stream into ESP-protocol packets.

### Public types

#### `struct Config`
**Source:** `ble_queue_module.h:20-24`.

- `size_t queueDepth` — FreeRTOS queue depth (default 24).
- `size_t rxBufferCap` — RX byte buffer cap (default 512).

### Lifecycle

#### `void begin(V1BLEClient* bleClient, PacketParser* parser, V1ProfileManager* profileMgr, DisplayPreviewModule* previewModule, PowerModule* powerModule, SystemEventBus* eventBus = nullptr, Config cfg = Config())`
Wires dependencies, allocates the FreeRTOS queue, primes the RX buffer.
**Source:** `ble_queue_module.h:26-28`.

### Entry points

#### `void onNotify(const uint8_t* data, size_t length, uint16_t charUUID, uint32_t sessionGeneration)`
Called from the NimBLE notify callback with the immutable V1 session generation captured by `V1BLEClient`. Enqueues the packet only when that generation is open. Must be safe from interrupt-ish context.
**Source:** `ble_queue_module.h:49`.

#### `void openSession(uint32_t sessionGeneration)` / `void closeSession()`
Opens notification admission for an authoritative V1 link generation, or closes the outgoing session and discards its queued bytes, partial frame buffer, and parsed-frame signal. Queue entries carry the captured generation so a callback racing the close cannot republish outgoing data.

#### `void process()`
Drains the queue, frames packets out of the byte stream, parses, and forwards results to the rest of the pipeline. Call once per main-loop tick.
**Source:** `ble_queue_module.h:58`.

### Status

#### `uint32_t getLastParsedTimestamp() const`
Timestamp (millis) of the most recent successful parse — used by display latency tracking.
**Source:** `ble_queue_module.h:39`.

#### `bool consumeParsedFlag()`
Returns true if at least one packet was successfully parsed since the last call, then clears the flag.
**Source:** `ble_queue_module.h:42`.

#### `unsigned long getLastRxMillis() const`
Timestamp of last received notification (regardless of parse outcome).
**Source:** `ble_queue_module.h:60`.

#### `bool isBackpressured() const`
True when the queue / RX buffer is in the high-water region. Consumers throttle non-essential work.
**Source:** `ble_queue_module.h:61`.

## Class: `ConnectionStateModule`

**Header:** `src/modules/ble/connection_state_module.h:24`.

Tracks authoritative BLE session boundaries plus connect/disconnect presentation state. It clears queued bytes, partial and published parser alerts, and persisted alerts when a generation closes; re-requests alert data when traffic stops; notifies the power module; and refreshes display indicators on disconnect. This prevents an outgoing alert from being rendered as live or persisted after reconnect.

### Lifecycle

#### `void begin(V1BLEClient* bleClient, PacketParser* parser, V1Display* display, PowerModule* powerModule, BleQueueModule* bleQueueModule, AlertPersistenceModule* alertPersistence, SystemEventBus* eventBus = nullptr)`
Injects dependencies.
**Source:** `connection_state_module.h:28-30`.

#### `bool process(unsigned long nowMs)`
Runs one tick of state-tracking. Returns true if currently connected.
**Source:** `connection_state_module.h:34`.

#### `handleSessionOpened(...)` / `handleSessionClosed(...)`
Main-loop hooks wired to `V1BLEClient`'s monotonic generation boundary. Open runs before characteristic subscription, and close runs when quiescence invalidates the outgoing session. Boolean/generation polling in `process()` remains a watchdog fallback.

### Constants

- `DATA_STALE_MS = 2000` — data considered stale after 2 s of no traffic.
- `DATA_REQUEST_INTERVAL_MS = 1000` — re-request alert data every 1 s while stale.

**Source:** `connection_state_module.h:67-68`.

## Class: `ConnectionRuntimeModule`

**Header:** `src/modules/ble/connection_runtime_module.h:16`.

Aggregates signals from the BLE pipeline into a single per-tick snapshot consumed by the loop dispatcher.

### Public types

#### `struct ConnectionRuntimeSnapshot`
**Source:** `connection_runtime_module.h:5-14`.

Per-tick connection state — `connected`, `receiving`, `backpressured`, `skipNonCore`, `overloaded`, `bootSplashHoldActive`, `initialScanningScreenShown`, `requestShowInitialScanning`.

#### `struct Config`
**Source:** `connection_runtime_module.h:18-23`.

- `tickGapMaxUs = 25000` — non-core tick gap budget.
- `overloadLoopUs = 25000` — loop overload threshold.
- `receivingHeartbeatMs = 2000` — receiving-heartbeat threshold.
- `runStartTimeoutMs = 30000` — first-run-start timeout for boot logging.

#### `struct Providers`
**Source:** `connection_runtime_module.h:25-31`.

Function pointers for dependency injection — `isBleConnected`, `isBackpressured`, `getLastRxMillis` plus their context pointers.

### Lifecycle

#### `void begin(const Providers& hooks)` / `void begin(const Providers& hooks, const Config& cfg)`
Two overloads; second accepts non-default config.
**Source:** `connection_runtime_module.h:33`.

### Pump

#### `ConnectionRuntimeSnapshot process(unsigned long nowMs, unsigned long nowUs, unsigned long lastLoopUs, bool bootSplashHoldActive, unsigned long bootSplashHoldUntilMs, bool initialScanningScreenShown)`
Builds the per-tick snapshot from current signals.
**Source:** `connection_runtime_module.h:36-38`.

## Class: `ConnectionStateCadenceModule`

**Header:** `src/modules/ble/connection_state_cadence_module.h:21`.

Decides whether `ConnectionStateModule::process()` should run on a given tick. Governs scan-screen dwell and display-update intervals.

### Public types

#### `struct ConnectionStateCadenceContext`
**Source:** `connection_state_cadence_module.h:5-12`.

Inputs: `nowMs`, `displayUpdateIntervalMs` (default 50), `scanScreenDwellMs`, `bleConnectedNow`, `bootSplashHoldActive`, `displayPreviewRunning`.

#### `struct ConnectionStateCadenceDecision`
**Source:** `connection_state_cadence_module.h:14-18`.

Outputs: `displayUpdateDue`, `holdScanDwell`, `shouldRunConnectionStateProcess`.

### Methods

#### `void reset()`
Clears internal timestamps. Call on connection cycle reset.

#### `void onScanningScreenShown(unsigned long nowMs)`
Notifies the cadence gate that the scan screen is up so dwell logic can engage.

#### `ConnectionStateCadenceDecision process(const ConnectionStateCadenceContext& ctx)`
Returns the decision for this tick.

**Source:** `connection_state_cadence_module.h:25`.

## Class: `ConnectionStateDispatchModule`

**Header:** `src/modules/ble/connection_state_dispatch_module.h:25`.

Wraps the cadence gate and adds a starvation watchdog. If `connection_state_module.process()` hasn't run within `maxProcessGapMs`, the watchdog forces a run regardless of cadence — guards against display freeze if the cadence gate gets stuck.

### Public types

#### `struct ConnectionStateDispatchContext`
**Source:** `connection_state_dispatch_module.h:7-15`.

Inputs: `nowMs`, `displayUpdateIntervalMs`, `scanScreenDwellMs`, `bleConnectedNow`, `bootSplashHoldActive`, `displayPreviewRunning`, `maxProcessGapMs`.

#### `struct ConnectionStateDispatchDecision`
**Source:** `connection_state_dispatch_module.h:17-22`.

Output: nested `ConnectionStateCadenceDecision`, `elapsedSinceLastProcessMs`, `watchdogForced`, `ranConnectionStateProcess`.

#### `struct Providers`
**Source:** `connection_state_dispatch_module.h:27-37`.

Function pointers — `runCadence`, `runConnectionStateProcess`, `recordDecision` — plus context pointers for each.

### Lifecycle / pump

#### `void begin(const Providers& hooks)` — wire dependencies.
#### `void reset()` — clear internal state.
#### `ConnectionStateDispatchDecision process(const ConnectionStateDispatchContext& ctx)` — run one dispatch tick.

**Source:** `connection_state_dispatch_module.h:41`.

## Dependencies (module-wide)

| External dependency | Used by |
|---|---|
| `V1BLEClient` (`src/ble_client.h`) | BleQueueModule, ConnectionStateModule. |
| `PacketParser` | BleQueueModule, ConnectionStateModule. |
| `AlertPersistenceModule` | ConnectionStateModule (session-boundary clear). |
| `V1Display` | ConnectionStateModule (display refresh on disconnect). |
| `PowerModule` | BleQueueModule, ConnectionStateModule. |
| `V1ProfileManager` | BleQueueModule. |
| `DisplayPreviewModule` | BleQueueModule (preview suppression). |
| `SystemEventBus` | All five (cross-module event publication). |
| FreeRTOS queue (`xQueue*`) | BleQueueModule. |

## Notes for maintainers

The `BleQueueModule::onNotify` path runs from the NimBLE notify callback, which on this platform executes on the BLE host task — not the main loop. All shared state touched by `onNotify` must be atomic or protected by the queue boundary. Don't add unsynchronized direct field reads from `onNotify`.

The cadence and dispatch modules exist in their current shape because the previous implementation had `connection_state_module.process()` running every tick, which was wasteful, and a previous attempt to throttle it caused display freezes when the cadence gate got stuck. The watchdog in `ConnectionStateDispatchModule` is the resolution of that history — don't remove it without re-introducing the failure mode it prevents.

`ConnectionRuntimeSnapshot` is read by the main loop dispatcher to skip non-core work when overloaded or backpressured. Adding a field to the snapshot is fine; renaming or removing fields ripples into the dispatcher.
