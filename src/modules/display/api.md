# display Module API

Display pipeline modules — the layer **above** the low-level rendering primitives in `src/display*.cpp`. The renderer (`V1Display`) draws pixels; the modules here decide *when* to render, *what data* to render, and *how* to compose multiple sources (V1 alerts + ALP laser events + persistence latches) into a single frame.

This module is the structural locus of the project's recent display work. The `RenderFrameComposer` replaced an older "ALP-as-V1-passenger" design where ALP events flowed through V1's alert table; that design produced state drift between the two sources and was reworked into the explicit composition pattern in this directory.

## Files

| File | Class / role |
|---|---|
| `display_pipeline_module.{h,cpp}` | Main pipeline — runs after each successful parse; drives composition and render. |
| `display_orchestration_module.{h,cpp}` | Loop-level orchestration — decides whether the pipeline should run this tick; handles boot splash, overload, refresh. |
| `display_preview_module.{h,cpp}` | Color/preview test sequencer — drives a multi-phase visual test instead of the old 5-step swatch. |
| `display_restore_module.{h,cpp}` | Restores display to live data after a preview ends. |
| `render_frame_composer.{h,cpp}` | Pure composition function — combines V1 + ALP snapshots into a `RenderFrame`. |
| `display_edge_log.{h,cpp}` | Edge-triggered ALP display-decision logging from `V1Display` setter transitions. |
| `display_correctness_trace.{h,cpp}` | Bounded, heap-free trace of parsed-source vs rendered-frame correctness. |

## Class: `DisplayPipelineModule`

**Header:** `src/modules/display/display_pipeline_module.h`.

Runs after a successful parse. Pulls state from V1 + ALP + persistence sources, asks the composer to produce a frame, and forwards it to `V1Display`.

### Public types

#### `struct DisplayPipelineDependencies`
**Source:** `display_pipeline_module.h`.

Bundles all dependencies for `begin()` so the wide injection surface stays explicit:

- `DisplayMode*` — mode pointer (live / preview / sleep / etc.).
- `V1Display*` — render target.
- `PacketParser*` — V1 state source.
- `SettingsManager*` — settings.
- `V1BLEClient*` — connection state.
- `AlertPersistenceModule*` — V1 alert persistence.
- `SpeedMuteModule*` — speed-mute state.
- `QuietCoordinatorModule*` — BLE-quiet windows.
- `VoiceModule*` — local audio announcement pipeline.
- `AlpRuntimeModule*`, `AlpEventLatch*` — ALP state + persistence.
- `SpeedSourceSelector*` — for stealth-mode speed display.

### Lifecycle

#### `void begin(const DisplayPipelineDependencies& dependencies)`
Wires dependencies. Call once from `setup()`.
**Source:** `display_pipeline_module.h:40`.

### Pump

#### `void handleParsed(uint32_t nowMs)`
Called after `parser.parse()` succeeds. Composes a frame, runs the voice pipeline against the parsed alert snapshot, and updates the display.
**Source:** `display_pipeline_module.h:43`.

#### `void refreshBlinkTick(uint32_t nowMs)`
Re-renders the current frame without alert-persistence or voice side effects. The loop display phase calls this only when the orchestrator requests a blink refresh on a no-parsed-frame loop, so blink state can advance without waiting for the next V1 packet.
**Source:** `display_pipeline_module.h`.

#### `bool restoreCurrentOwner(uint32_t nowMs)`
Re-asserts current display ownership (V1 vs ALP) — used by the restore module after a preview ends.
**Source:** `display_pipeline_module.h:49`.

#### `bool allowsObdPairGesture(uint32_t nowMs) const`
True when the OBD pair gesture (e.g. touchscreen long-press) is admissible — false during alerts, previews, or boot splash.
**Source:** `display_pipeline_module.h:50`.

## Class: `DisplayOrchestrationModule`

**Header:** `src/modules/display/display_orchestration_module.h`.

Loop-level coordinator that decides whether `DisplayPipelineModule::handleParsed()` should run this tick, separately handles early-loop work (boot-splash hold, BLE-context refresh) and refresh-on-no-parsed-frame logic.

### Public types

#### `struct DisplayOrchestrationEarlyContext`
**Source:** `display_orchestration_module.h:17-23`.
Inputs for the early-loop hook — `nowMs`, boot-splash flag, overload flag, BLE context snapshot, BLE-receiving flag.

#### `struct DisplayOrchestrationParsedContext` / `struct DisplayOrchestrationParsedResult`
**Source:** `display_orchestration_module.h:25-29`.
Inputs/outputs for the "should we run pipeline this tick?" decision. Result includes `runDisplayPipeline` and `reasonSkipped` (string for logs).

#### `struct DisplayOrchestrationRefreshContext` / `struct DisplayOrchestrationRefreshResult`
**Source:** `display_orchestration_module.h:36-41`.
Inputs/outputs for the periodic refresh hook — used when no parsed frame arrives but a periodic re-render is due.

### Methods

The class is a coordinator. The shape is `(early|parsed|refresh)Context → (early|parsed|refresh)Result`. Reads dependencies (V1Display, BLE, preview, restore, parser, settings, volume-fade, speed-mute, quiet) injected via a single `begin(...)` call. The header comment calls out the wide wiring intentionally — this is the orchestration center.

## Class: `DisplayPreviewModule`

**Header:** `src/modules/display/display_preview_module.h`.

Runs a multi-phase visual preview that exercises every screen element. Used both for color preview (after settings change) and as a manual diagnostic.

### Phases (per header comment)

1. Band + direction sweep — X, K, Ka (3 freqs), Laser cycling front → side → rear with ramping signal strength.
2. Multi-alert combos with priority-only visuals (Photo radar included).
3. ALP state cycling — OFF, IDLE, warm-up, DLI active, LID active, ALERT_ACTIVE, NOISE_WINDOW, TEARDOWN with gun-abbreviation override.
4. Status indicator cycling — bogey chars, mode chars, mute, OBD states, BLE proxy, volume levels, profile slots.

### Lifecycle

#### `DisplayPreviewModule()`
Default constructor.
**Source:** `display_preview_module.h:40`.

#### `void begin(V1Display* display)`
Wires the render target. Call once.
**Source:** `display_preview_module.h:42`.

### Control

#### `void requestHold(uint32_t durationMs)`
Starts the preview sequence; nonzero `durationMs` is honored as the caller-owned hold. Pass `0` to run the full diagnostic sequence duration. Preview update renders at most one step per loop; skipped diagnostic steps are dropped/carry-forwarded rather than caught up with multiple display flushes.
**Source:** `display_preview_module.h:45`.

#### `void cancel()`
Ends preview immediately.
**Source:** `display_preview_module.h:46`.

### Status

#### `bool isRunning() const`
True while preview is active.
**Source:** `display_preview_module.h:49`.

There's also a "preview just ended" one-shot flag exposed by `consumeEnded()`.

### Constraints (per header)

- Does **not** own the display connection — receives a `V1Display*`.
- Does **not** touch real runtime modules — uses preview setters for ALP / OBD state so the modules themselves are unaffected.

## Class: `DisplayRestoreModule`

**Header:** `src/modules/display/display_restore_module.h:18`.

Runs after `DisplayPreviewModule` ends — forces a full redraw and restores live V1 (or ALP) state to the screen.

### Lifecycle

#### `void begin(V1Display* disp, PacketParser* pktParser, V1BLEClient* ble, DisplayPreviewModule* preview, DisplayPipelineModule* displayPipeline)`
**Source:** `display_restore_module.h:20-21`.

### Pump

#### `bool process()`
Checks the preview-ended flag; if set, performs restoration. Returns true if work was done.
**Source:** `display_restore_module.h:27`.

## Class: `RenderFrameComposer`

**Header:** `src/modules/display/render_frame_composer.h:20`.

Pure function: given V1 state, ALP state, settings, and time, produces a `RenderFrame`. No I/O, no side effects, no module wiring — easy to unit-test and the right place to put display-composition rules (priority, ALP overlay, persistence resolution).

### Public types

#### `struct V1Snapshot`
**Source:** `render_frame_composer.h:6-14`.

V1-side inputs — `DisplayState`, alert array + count, priority alert, has-persisted flag + persisted alert.

#### `struct AlpSnapshot`
**Source:** `render_frame_composer.h:16-21`.

ALP-side inputs — `AlpLaserEvent`, ownership flag (`ownsLaserDisplay`), persistence-latch flag, latched event.

### Method

#### `RenderFrame compose(const V1Snapshot& v1, const AlpSnapshot& alp, const V1Settings& settings, uint32_t nowMs) const`
Pure composition. `RenderFrame` (defined in `include/render_frame.h`) is the union frame consumed by `V1Display`.
**Source:** `render_frame_composer.h:25`.

## Edge logging — `display_edge_log.{h,cpp}`

Edge-triggered structured logs for `V1Display` ALP setter transitions. Free function, not a class.

- `void logV1DisplaySetterEdge(uint32_t nowMs, const char* setter, const char* detail = "")` — records ALP frequency-override setter transitions through the ALP runtime display-decision sink.

The function is stubbed to no-op under `UNIT_TEST` so native tests don't pull observability dependencies.

**Source:** `display_edge_log.h`.

## Correctness trace — `display_correctness_trace.{h,cpp}`

Bounded in-memory trace of the display pipeline's parsed-source to rendered-frame contract. Publishing is fixed-size and heap-free; when full, the trace drops the oldest entry rather than blocking BLE/display work.

- `DisplayCorrectnessTraceEvent` records source vs rendered band, arrows, frequency, bogey, mute state, and signal bars, with per-field `MATCH` / `MISMATCH` / `NOT_APPLICABLE` status.
- `DisplayCorrectnessTraceLog::kCapacity = 64`; `publish()` appends one event, `copyRecent()` copies recent events, `stats()` reports published/drop/size counters, and `reset()` clears the ring.
- Free functions (`displayCorrectnessTracePublish`, `displayCorrectnessTraceCopyRecent`, `displayCorrectnessTraceStats`, `displayCorrectnessTraceReset`) expose the singleton log.
- `buildDisplayCorrectnessTraceEvent(const RenderFrame&, uint32_t tsMs)` builds a trace event from the composed `RenderFrame`.

**Source:** `display_correctness_trace.h`.

## Dependencies (module-wide)

| External | Used by |
|---|---|
| `V1Display` (low-level renderer in `src/display*`) | All modules that produce frames. |
| `PacketParser`, `V1BLEClient`, `SettingsManager` | Pipeline + orchestration. |
| `AlpRuntimeModule`, `AlpEventLatch` | Pipeline (ALP composition). |
| `AlertPersistenceModule` | Pipeline (V1 persistence). |
| `SpeedMuteModule`, `QuietCoordinatorModule`, `SpeedSourceSelector` | Pipeline (auxiliary state). |
| `DisplayMode` (enum/state) | Pipeline + orchestration. |
| Display correctness trace | Pipeline correctness instrumentation. |

## Notes for maintainers

The composer is **the** authoritative place for display-composition rules. Don't add ALP-vs-V1 priority logic anywhere else — put it in `RenderFrameComposer::compose()` so it stays inspectable and testable. Past failures (the "ALP polled-passenger" pattern in particular) trace to composition logic spreading across multiple files; the composer-as-pure-function pattern exists to prevent recurrence.

`DisplayPipelineModule::handleParsed` runs after every successful parse — high frequency. Avoid heap allocation in this path; the composer takes its inputs by const reference and returns a stack `RenderFrame` for the same reason.

The orchestration module's "wide wiring is intentional" header comment is deliberate — it sits at the center of the display handoff. Don't try to thin it by hiding dependencies behind type-erased pointers; the explicit list is part of the contract.

`DisplayPreviewModule` deliberately uses preview-only setters on ALP/OBD state. Don't change preview to drive the real runtime modules — the test loses coverage of the off-the-wire path and starts depending on whatever happens to be connected at preview time. Preview is a lower-priority diagnostic path: under overload it should skip/drop visual catch-up, not issue back-to-back display transactions.

Edge-log functions stub to no-op in unit tests. If you add a new edge-log call, make sure both paths exist (real impl and `UNIT_TEST` stub) so native tests still link.
