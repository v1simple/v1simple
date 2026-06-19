# system Module API

The main-loop architecture. Each "loop_*" module owns one phase of the per-tick work; `system_event_bus.h` is the cross-module event channel; `connection_cycle_coordinator_module` arbitrates V1/OBD/Proxy connection ordering.

This module is large (14 headers) because the main loop has been carved into discrete phases — each phase is its own object with its own context/result struct. The shape is intentional: the phases are sequenced explicitly in `main.cpp` (or its successor) rather than buried in a god-object loop function. If you're reading this and `main.cpp` looks short, the phases here are the reason.

## Files (loop phases)

Listed in the rough order they execute per tick:

| File | Phase |
|---|---|
| `loop_pre_ingest_module.{h,cpp}` | Boot-readiness gate, replay-mode flag. |
| `loop_runtime_snapshot_module.{h,cpp}` | Per-tick runtime snapshot (BLE connected, DMA ready, preview running). |
| `loop_settings_prep_module.{h,cpp}` | Settings-derived flags for this tick (WiFi enable, etc.). |
| `loop_connection_early_module.{h,cpp}` | Early connection runtime — converts `ConnectionRuntimeSnapshot` into per-loop flags. |
| `loop_ingest_module.{h,cpp}` | BLE queue drain + parser. |
| `parsed_frame_event_module.{h,cpp}` | Merges queue parsed-signal with `SystemEventBus` events. |
| `loop_display_module.{h,cpp}` | Parsed-frame collection, display pipeline dispatch, refresh. |
| `loop_post_display_module.{h,cpp}` | Auto-push, speed selector, dispatch. |
| `loop_power_touch_module.{h,cpp}` | Power button + touch event. |
| `loop_telemetry_module.{h,cpp}` | Per-loop telemetry sampling, heap/cache perf recording. |
| `loop_tail_module.{h,cpp}` | Late-loop BLE drain + yield + loop-duration finalization. |
| `periodic_maintenance_module.{h,cpp}` | Periodic maintenance (perf reports, etc.) — preserves call order. |
| `connection_cycle_coordinator_module.{h,cpp}` | V1 / OBD / Proxy cycle orchestration (not strictly per-loop). |
| `system_event_bus.h` | Cross-module event channel. |

## File: `system_event_bus.h`

Cross-module event bus. POD payloads, fixed-size, no heap, designed for hot-path use.

### `enum class SystemEventType : uint8_t`
**Source:** `system_event_bus.h:5+`.

Event types — `NONE`, `BLE_FRAME_PARSED`, `ALP_STATE_CHANGED`, etc. Used by modules that need to react to events without direct coupling.

### `class SystemEventBus`
Subscribe / publish API. Subscribers register callbacks; publishers fire typed payloads. Implementation details in the cpp; consumers should see the type-safe surface.

## Class: `LoopPreIngestModule`

**Header:** `loop_pre_ingest_module.h`.

#### `struct LoopPreIngestContext` — `nowMs`, `bootReady`, `bootReadyDeadlineMs`.
#### `struct LoopPreIngestResult`
#### `process(const LoopPreIngestContext&) → LoopPreIngestResult`
Gates the rest of the loop on boot readiness; returns flags that suppress non-core work until ready.

## Class: `LoopRuntimeSnapshotModule`

**Header:** `loop_runtime_snapshot_module.h`.

#### `struct LoopRuntimeSnapshotValues` — `bleConnected`, `canStartDma`, `displayPreviewRunning`.
#### `struct LoopRuntimeSnapshotContext` — `canStartDmaProbeAllowed`.
#### `process(const LoopRuntimeSnapshotContext&) → LoopRuntimeSnapshotValues`
Builds the per-tick snapshot consumed by later phases. The DMA start-eligibility
probe is gated by `canStartDmaProbeAllowed`; when gated off, the module returns
the last cached value (or `false` before the first live probe) to avoid lower
priority WiFi heap probes on loops where WiFi start cannot consume the result.

## Class: `LoopSettingsPrepModule`

**Header:** `loop_settings_prep_module.h`.

#### `struct LoopSettingsPrepValues` — `enableWifi`.
#### `struct LoopSettingsPrepContext` — `nowMs`.
#### `process(...) → LoopSettingsPrepValues`
Derives per-tick boolean flags from `SettingsManager`.

## Class: `LoopConnectionEarlyModule`

**Header:** `loop_connection_early_module.h`.

#### `struct LoopConnectionEarlyContext` — `nowMs`, `nowUs`, `lastLoopUs`, `bootSplashHoldActive`, `bootSplashHoldUntilMs`, `initialScanningScreenShown`.
#### `struct LoopConnectionEarlyResult` — `bootSplashHoldActive`, `initialScanningScreenShown`, `bleConnectedNow`, `bleBackpressure`, `skipNonCoreThisLoop`, `overloadThisLoop`, `bleReceiving`.

Wraps the BLE connection-runtime module call into the per-tick phase shape.

## Class: `LoopIngestModule`

**Header:** `loop_ingest_module.h`.

#### `struct LoopIngestContext` — `nowMs`, `bleProcessEnabled`, `skipNonCoreThisLoop`, `overloadThisLoop`.
#### `struct LoopIngestResult` — `bleBackpressure`, `skipLateNonCoreThisLoop`, `overloadLateThisLoop`.
Drains the BLE queue, runs the parser. Skipped entirely on overloaded ticks.

## Class: `ParsedFrameEventModule`

**Header:** `parsed_frame_event_module.h`.

#### `struct ParsedFrameSignal` — `parsedReady`, `parsedTsMs`.

Merges the queue's parsed flag with `BLE_FRAME_PARSED` events from `SystemEventBus`. Either source signals "frame ready" — module unifies them.

## Class: `LoopDisplayModule`

**Header:** `loop_display_module.h`.

#### `struct LoopDisplayContext` — `nowMs`, `bootSplashHoldActive`, `overloadLateThisLoop`.

Orchestrates parsed-frame signal collection, display pipeline dispatch, lightweight refresh, and blink-refresh ticks. Wraps `DisplayOrchestrationModule` (in `src/modules/display/`) into the per-tick phase shape.

## Class: `LoopPostDisplayModule`

**Header:** `loop_post_display_module.h`.

#### `struct LoopPostDisplayContext` — `enableAutoPush`, `runSpeedAndDispatch`, `nowMs`, `displayUpdateIntervalMs`, `scanScreenDwellMs`, `bootSplashHoldActive`, `displayPreviewRunning`, `maxProcessGapMs`, `bleConnectedNow`.
#### `struct LoopPostDisplayResult` — `dispatchNowMs`, `bleConnectedNow`.

Auto-push state machine pump, speed-selector update, downstream dispatch.

## Class: `LoopPowerTouchModule`

**Header:** `loop_power_touch_module.h`.

#### `struct LoopPowerTouchContext` — `nowMs`, `loopStartUs`, `bootButtonPressed`.
#### `struct LoopPowerTouchResult` — `inSettings`, `shouldReturnEarly`.

Power-button + touchscreen event handling. Deals with long-press WiFi toggle, settings-mode entry, etc.

## Class: `LoopTelemetryModule`

**Header:** `loop_telemetry_module.h`.

Per-loop perf recording — jitter, heap, cache stats.

#### `struct Providers`
Function-pointer seam for perf recording: `microsNow`, `recordLoopJitterUs`, etc.

## Class: `LoopTailModule`

**Header:** `loop_tail_module.h`.

#### `struct Providers`
Function-pointer seam: `perfTimestampUs`, `loopMicrosUs`, etc.

Late-loop BLE drain + yield + loop-duration finalization. Always runs last.

## Class: `PeriodicMaintenanceModule`

**Header:** `periodic_maintenance_module.h`.

#### `struct Providers`
Function-pointer seam: `timestampUs`, `runPerfReport`, etc.

#### `struct Context`
Loop pressure hints (`bleBackpressure`, `loopOverloaded`, `forceTailBleDrainPending`) used to defer the synchronous settings NVS writer.

Periodic maintenance — perf report scheduling, log rotation, etc. Preserves call order across the various maintenance tasks that are admitted on the current loop.

## Class: `ConnectionCycleCoordinatorModule`

**Header:** `connection_cycle_coordinator_module.h`.

#### `enum class CycleState : uint8_t`
**Source:** `connection_cycle_coordinator_module.h:3-12`.

States: `SCAN_V1`, `V1_SETTLING`, `OBD_SCAN`, `OBD_CONNECT`, `OBD_SETTLED`, `PROXY_OPEN`, etc.

Orchestrates the V1 / OBD / Proxy connection ordering. V1 wins boot-time scan window; OBD runs after V1 settles in OBD / Standalone mode; passive Proxy advertising only opens when explicit Proxy / App mode is selected and the coordinator admits it. Explicit Proxy / App mode keeps advertising available for the drive after V1 settles; BLE runtime starts at a fast discovery cadence, downshifts to slower background advertising when idle, and briefly returns to fast cadence after an app disconnect. If a proxy client is connected, OBD scan/connect work is stopped and OBD runtime is dropped to idle. The cycle restarts on disconnect events.

This is the higher-level coordinator that the per-tick `LoopConnectionEarlyModule` defers to for cross-phase decisions.

## Dependencies

| External | Used by |
|---|---|
| `SystemEventBus` (defined here) | Many modules — display, BLE, ALP, etc. |
| Function-pointer providers per module | All. |
| `ConnectionRuntimeModule` (`src/modules/ble/`) | `LoopConnectionEarlyModule`. |
| `DisplayOrchestrationModule` (`src/modules/display/`) | `LoopDisplayModule`. |
| `AutoPushModule`, `SpeedSourceSelector`, `SpeedMuteModule` | `LoopPostDisplayModule`. |
| Battery, touch | `LoopPowerTouchModule`. |
| Perf metrics | `LoopTelemetryModule`, `LoopTailModule`, `PeriodicMaintenanceModule`. |

## Notes for maintainers

The phase decomposition is the result of a previous refactor — `main.cpp` used to be thousands of lines and the loop body was a single function with deep nesting. The phases were carved out to make per-phase work testable and to pin perf budgets to phases. Don't fold them back into a god-loop.

`SystemEventBus` is the right channel for cross-phase signals where direct module-to-module wiring would create cycles. POD payloads, fixed size — don't put `String` or anything heap-allocating in event payloads. The bus is on the hot path.

`ConnectionCycleCoordinatorModule` is the *only* place where V1 / OBD / Proxy ordering is decided. Don't add ordering logic to individual modules — they should respect the cycle state and not race.

The loop-tail BLE drain is critical — without it, BLE notifications can pile up faster than the parser drains them, and you get backpressure. Don't move BLE drain out of the tail without verifying the new placement under bench load.

Per-loop phase modules are written to be **noisy** at high time pressure — when `overloadThisLoop` is true, several phases skip their work. Don't add code that needs to run every tick into a phase that can be skipped; put it in `LoopTailModule` (always runs) or `PeriodicMaintenanceModule` (rate-limited, with low-priority persistence admitted only on unpressured loops).
