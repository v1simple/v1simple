# ALP session identity and display ownership

This map follows the production path repaired in `daf32ed`: a newly opened,
unidentified ALP engagement must not inherit the previous engagement's gun name.
It describes current source behavior, including the final display setter.
The governing contract is [Valentine's Law](../VALENTINE_PHILOSOPHY.md):
live ALP owns the laser presentation, live V1 beats persisted ALP, and ALP
warm-up suppression is an explicitly named project deviation.

## Owner chain

| Boundary | Owner and contract |
|---|---|
| UART bytes → admitted frames | [`AlpRuntimeModule::drainUart`, `tryParseFrame`, `parseRingBuffer`](../../src/modules/alp/alp_runtime_module.cpp): bounded buffering, supported header and checksum before frame dispatch. |
| Frames → engagement | [`transitionTo`, frame handlers, `updateCurrentEvent`](../../src/modules/alp/alp_runtime_module.cpp): session lifetime, warm-up, gun, direction and display eligibility. |
| Engagement → typed snapshot | [`AlpLaserEvent`](../../src/modules/alp/alp_laser_event.h): active, gun, direction, LID mode, open/close times and session generation travel together. |
| Runtime → presentation wake | [`SystemEventBus`](../../src/modules/system/system_event_bus.h) coalesces edges; [`DriveRuntime::consumeDisplayEdges`](../../src/drive_runtime.cpp) also services presentation deadlines. |
| Snapshot → presentation/latch | [`DisplayPipelineModule::buildPresentedAlpEvent`, `updateAlpLatch`](../../src/modules/display/display_pipeline_module.cpp) retain context only within a session and manage the separate ALP tail. |
| Sources → selected owner | [`RenderFrameComposer::compose`](../../src/modules/display/render_frame_composer.cpp) orders ALP live, V1 live, ALP persisted, V1 persisted, idle. |
| Selected owner → stored label | [`renderComposedFrame`](../../src/modules/display/display_pipeline_module.cpp) calls the real [`V1Display::setAlpLaserEvent`](../../src/display_indicators.cpp) before `renderFrame`. |
| Stored label → paint | [`renderFrame`](../../src/display_update.cpp) synthesizes the laser alert; [`resolveFrequencyPresentation`, `renderFrequencyPresentation`](../../src/display_frequency.cpp) resolve and draw its text. |

## Admission and runtime lifecycle

`begin(true)` configures production UART2 receive on GPIO2 at 19200 baud, 8N1,
with no transmit pin. UART hardware, pull-up, startup drain and glitch filtering
are outside native tests. Disabled runtime stays `OFF`; enabled startup is `IDLE`.

Frames contain three payload bytes and checksum `(b0 + b1 + b2) & 0x7f`.
Admission requires a recognized header as well as checksum agreement.
Invalid candidates advance by one byte to resynchronize; partial frames remain
buffered. Each drain and parse pass is bounded by the 64-byte ring budget.
Valid traffic can establish `LISTENING`; a gun-ID frame alone does not open an alert.

The accepted `98 00 E3` and `98 02 00` detections refresh the alert watchdog.
Targeted B0 heartbeat mode `01` can also open an engagement from idle/listening.
The separate `detectGeneration` counts accepted detection frames; it is **not**
the identity used to decide whether a display may retain a gun name.

`transitionTo(ALERT_ACTIVE)` from `IDLE` or `LISTENING` opens a fresh session,
clears prior gun/direction context, and increments `sessionGeneration_`.
Rearming from `TEARDOWN` keeps the existing session and generation.
The generation, not an open timestamp or an active/inactive display edge,
distinguishes adjacent engagements. Generation zero never authorizes retention.

Relevant timeouts and terminators are:

| Condition | Runtime result |
|---|---|
| Raw UART silence greater than 3000 ms | Return to `IDLE`, close an open session and reset link/warm-up admission state. |
| Listening heartbeat timeout greater than 3000 ms | Return to `IDLE`. |
| Alert watchdog greater than 15000 ms without a detection rearm | Enter `TEARDOWN`; repeated targeted heartbeats do not refresh this watchdog. |
| Eight consecutive rejected frame candidates in the alert path | Enter `NOISE_WINDOW`; valid traffic or its 35000 ms maximum leads to teardown. |
| D0–D3 register frame ending in FD during alert | Enter `TEARDOWN`. |
| Teardown age greater than 5000 ms | Enter `LISTENING` and close the session; teardown housekeeping does not extend its entry time. |

`process()` evaluates state timeouts before parsing queued frames. It can close
one session and parse a trigger opening the next in the **same call**. A display
consumer can therefore observe active → active with a different generation.
After teardown timeout, heartbeat reopening is suppressed for that process pass;
an explicit detection frame can still open the next engagement immediately.

Warm-up is admission policy, not an unknown-gun synonym. A fresh session may be
withheld for an early preamble/35-second boot envelope, no heartbeat yet, or
unconfirmed heartbeat modes. Recognized gun identity and LID deploy release the
gate; heartbeat release follows the explicit mode/envelope conditions in
`handleHeartbeatFrame`. Merely repeating an unidentified detection does not
establish a confirmed alert. See the philosophy's named deviation.

`updateCurrentEvent` exposes an active event only for an open, non-warm-up
session in `ALERT_ACTIVE`, `NOISE_WINDOW`, or **identified `TEARDOWN`**.
Unknown-gun teardown is inactive. A known gun can therefore remain live through
teardown without being a persisted tail. Inactive snapshots expose unknown gun
and direction while retaining generation and edge timestamps.

Direction is session-owned: targeted `01` supplies front; DLI/LID `03`/`04`
requires a recognized gun before rear can latch. Front can replace rear;
later rear samples cannot replace front. Unknown maps to no synthetic arrow.
Gun tables and heartbeat interpretation are protocol assumptions encoded in
source and fixtures; this document does not independently qualify every gun.

## Presentation, persistence and the repaired setter

`buildPresentedAlpEvent` preserves a previously known gun or direction on an
unknown update only when the incoming nonzero generation matches its stored
generation. A new session replaces both fields, including unknown values.
An inactive close into abnormal `LISTENING` can retain known presentation
context for less than 1000 ms; normal listening modes `02`/`03`/`04` end that
hold. This bounded hold is separate from runtime session lifetime.

`AlpEventLatch` stores the presented active event. The live-to-inactive edge
starts the global `alpAlertPersistSec` window, not V1's per-slot persistence.
Repeated inactive frames do not restart it. A new live event replaces the latch;
expiry or disabled persistence clears it. Hold and persistence deadlines request
a display refresh even without a new BLE packet or ALP frame.

The composer suppresses V1 laser only while selecting a live ALP primary;
connected-but-silent ALP does not hide V1 laser. Concurrent V1 radar remains in
cards. Any live V1 priority takes ownership ahead of an ALP persisted tail.
`synthesizeAlpPrimaryState` gives ALP laser full bars and `muted = false`.

The pipeline already enforced session identity before `daf32ed`. The downstream
real setter nevertheless retained a previous known gun whenever both old and
new events were active and the incoming gun was unknown. It could undo the
pipeline's correct new-session result. The repair adds the same nonzero,
equal-generation requirement to `V1Display::setAlpLaserEvent`.

The setter writes the snapshot, live flag, gun-text override and LID mode, then
invalidates affected indicator/arrow/frequency caches when visible state changes.
Unknown identity in a new session clears the override and text. The frequency
resolver then selects `LASER` for the synthetic laser band; same-session unknown
updates may still retain the established gun abbreviation.

For `ALP_PERSISTED`, the pipeline passes retained identity with `active = false`.
This preserves gun text while disabling live-only badge/arrow behavior.
It does **not** imply a grey gun label: the current composer and `renderFrame`
use the unmuted ALP render path for both ALP owner kinds. Frequency color follows
the actual muted argument and LID/DLI setting; expiry clears the override.

## Other callers and restoration

- [`DriveLoopCoordinator::tick`](../../src/runtime_coordinator.h) processes ALP
  before consuming display edges, including while settings are open; live alerts
  can preempt settings. [`processAlpPresentationAndPower`](../../src/drive_runtime.cpp)
  cancels an active preview for a live ALP event.
- [`DisplayPreviewModule`](../../src/modules/display/display_preview_module.cpp)
  deliberately writes `setAlpPreviewState` and `setAlpFrequencyOverride` directly.
  Cleanup clears these overrides; [`DisplayRestoreModule::process`](../../src/modules/display/display_restore_module.cpp)
  restores the current owner through the pipeline when preview ownership ends.
- Touch restore callbacks, connection-owner restoration and power-warning
  preemption in [`DriveRuntime`](../../src/drive_runtime.cpp) call
  `restoreCurrentOwner`. Its ALP branches use the same composer and real setter;
  disconnected restoration removes stale V1 alert state first.
- [`syncTopIndicators`](../../src/display_indicators.cpp) refreshes ALP badge
  status only; it does not overwrite the pipeline-owned laser event or gun text.
- [`prewarmFrequencyRasterCache`](../../src/display_frequency.cpp) temporarily
  substitutes sample gun text, then restores the saved override, mode and text.
  It is not a source of session identity.

## Regression evidence and executable controls

[`test_alp_display_session`](../../test/test_alp_display_session/test_alp_display_session.cpp)
includes the actual runtime parser, pipeline, composer, latch, persistence code
and **real display setter**. It injects checksummed bytes and models UART arrival
timestamps. Settings/BLE/clock/panel dependencies are mocked; `renderFrame`
records a frame instead of painting. Its three tests are:

1. `test_new_unknown_session_clears_prior_gun_without_an_inactive_display_frame`:
   normal heartbeat at 100 ms, trigger at 200, PL3 at 210, teardown at 250,
   keepalive through 5000, fresh trigger at 5251. Assert changed generation,
   unknown runtime/frame gun, active laser, and empty real setter text/override.
2. `test_same_session_unknown_update_keeps_identified_gun`: the same generation
   retains PL3 when an unknown update reaches the actual setter.
3. `test_identified_teardown_keeps_gun_in_persisted_tail_then_clears`: a 3-second
   tail keeps PL3 with live flag false at 5251 and clears text/override at 8251.

The first test fails against the pre-repair setter; the latter two protect
intentional retention and expiry. Useful adjacent controls are:

| Suite | Exact test examples and boundary |
|---|---|
| [`test_alp_runtime`](../../test/test_alp_runtime/test_alp_runtime.cpp) | `test_unknown_header_checksum_collision_does_not_count_as_valid_activity`, `test_bad_checksum_frame_rejected`, `test_session_survives_teardown_rearm_cycle`, `test_teardown_without_gun_id_does_not_display`, `test_warmup_does_not_fallback_release_unknown_session`: real runtime with injected bytes/test seams. |
| [`test_display_pipeline_module`](../../test/test_display_pipeline_module/test_display_pipeline_module.cpp) | `test_new_alp_session_does_not_inherit_identity_even_at_same_millis_tick`, `test_alp_hold_and_persistence_deadlines_each_request_one_refresh`: pipeline composition with a mock display and runtime state seams. |
| [`test_render_frame_composer`](../../test/test_render_frame_composer/test_render_frame_composer.cpp) | `test_render_frame_composer_v1_laser_survives_silent_alp`, `test_render_frame_composer_v1_live_outranks_alp_persisted`: owner selection from constructed snapshots. |

From the repository root with PlatformIO available, run these suites serially:

```sh
python3 scripts/run_native_tests_serial.py --env native-sanitized test_alp_display_session test_alp_runtime test_display_pipeline_module test_render_frame_composer
```

These tests do not close the UART-electrical → rasterized gun text → physical
panel boundary. [`test_display_live_dispatch`](../../test/test_display_live_dispatch/test_display_live_dispatch.cpp)
executes real frame/arrow/card dispatch but stubs the ALP setter and frequency
drawing, so it cannot supply that missing proof. Physical qualification needs a
known gun → identified teardown → fresh unknown engagement, same-session
retention, and tail expiry on the tested image, with UART timing and visible
frames correlated. No new hardware, RF, speaker or physical panel result is
claimed by this source map.
