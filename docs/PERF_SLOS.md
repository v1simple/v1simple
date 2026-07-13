# Perf SLOs

> Status: Active
> Date: 2026-05-03

This file defines the SLOs the project is graded against. It distinguishes between two tiers, in priority order:

1. **Correctness SLOs** — what V1 sent vs. what we drew on the LCD. These are the **load-bearing** grade. The render-frame contract now emits bounded per-frame trace events (`DisplayCorrectnessTraceEvent`) for mechanically checking the composed-frame layer; physical LCD/pixel verification is still manual until a camera/OCR gate exists.
2. **Operational SLOs** — capacity, latency, queue depth, mutex hygiene. Real numbers, mechanically verifiable today, but **not** the project's correctness grade. These prevent the device from falling over; they don't prevent the device from showing the wrong band.

Earlier SLO guidance had only the operational tier and treated it as the project grade. That was the synthetic-SLO problem: A-grade execution metrics while the display rendered the wrong thing. This rewrite separates the tiers explicitly so future grading isn't optimizing for the wrong thing.

## Correctness SLOs (the real grade)

These are user-observable invariants. Each describes what V1 sent and what the LCD must show in response. The first automation layer is the render-frame trace: it proves the display pipeline's composed frame carries the expected source fields into `V1Display::renderFrame()`. A second native render-operation layer now verifies selected `RenderFrame` inputs reach the `V1Display` leaf draw calls. This still does **not** prove the physical panel pixels changed correctly; that remains manual/future camera coverage.

- Future instrumentation has a target.
- Bug reports can cite which invariant was violated.
- The "did the display show the right thing?" question has a written answer.

Status notation:

- `aspirational-lcd` — invariant is well-defined, no LCD/pixel measurement infrastructure exists.
- `manual-lcd` — invariant can be checked by sitting next to a real V1 with eyes on both displays.
- `measured-render-frame` — invariant is mechanically checked at the composed render-frame contract via `tools/perf_correctness_invariants.json`; LCD/pixel verification remains out of scope for that status.
- `measured-render-frame+native-render-operation` — render-frame contract plus native tests that verify the frame reaches the display leaf draw calls with expected band/arrow/frequency/bogey/mute/signal arguments; physical LCD pixels remain out of scope.
- `measured` — invariant is automatically verified.

| ID | Invariant | Measurement | Status |
|---|---|---|---|
| `C-BAND-PRIMARY` | Rendered primary band matches parsed/composed primary band on every render frame. | `DisplayCorrectnessTraceEvent` with `sourceBand`, `renderedBand`, `bandStatus`; native render-operation test verifies the primary band reaches the draw path. | measured-render-frame+native-render-operation |
| `C-ARROWS-PRIMARY` | Rendered primary direction arrows match parsed/composed arrows on every render frame. | Trace event with `sourceArrows`, `renderedArrows`, `arrowsStatus`; native render-operation test verifies arrows reach `drawDirectionArrow()`. | measured-render-frame+native-render-operation |
| `C-FREQ-V1` | Rendered V1 primary frequency text receives the parsed frequency MHz value. | Trace event with `sourceFrequency`, `renderedFrequency`, `frequencyStatus`; native render-operation test verifies frequency reaches `drawFrequency()`. | measured-render-frame+native-render-operation |
| `C-BOGEY-STATUS` | Rendered bogey character receives parsed `bogeyCounterChar` on every non-empty render frame. | Trace event with `sourceBogey`, `renderedBogey`, `bogeyStatus`; native render-operation test verifies the top-counter character reaches the status-strip draw path. Blink-off cadence still requires LCD/cadence instrumentation. | measured-render-frame+native-render-operation |
| `C-MUTE-V1-LIVE` | V1 live mute icon state receives parsed `muted` flag. | Trace event with `sourceMuted`, `renderedMuted`, `muteStatus`; native render-operation test verifies V1 live mute reaches the leaf draw path. IDLE/PERSISTED/ALP intentional mute suppression is marked not-applicable. | measured-render-frame+native-render-operation |
| `C-SIGNAL-LIVE` | Live simple signal bars receive the parsed V1 LED-bar bitmap expanded to this display's 8-slot scale. | Trace event with `sourceSignalBars`, `renderedSignalBars`, `signalBarsStatus`; native render-operation test verifies signal-bar strength reaches `drawVerticalSignalBars()`. Physical bar pixels remain future LCD coverage. | measured-render-frame+native-render-operation |
| `C-PERSIST-HOLD` | Persisted alert holds for `alpAlertPersistSec` ± 50 ms after V1 clears. | Trace event on persistence start/clear with timestamps. | aspirational-lcd |
| `C-ALP-OWNER` | When ALP `ownsLaserDisplay()` is true, ALP gun + direction render takes precedence over V1 laser-band display. | Trace event records ALP ownership (`owner == ALP`) and rendered laser primary; native render-operation test verifies ALP-owned frames enter the draw path as laser. Gun-text pixel verification remains future LCD coverage. | measured-render-frame+native-render-operation |
| `C-BLINK-CADENCE` | Bogey counter blinks at V1's cadence when `bogeyCounterByte != bogeyCounterByte2`. | LCD/camera or high-rate trace sampled at ≥200 Hz; cadence computed offline; deviation from spec 96 ms toggle ≤ ±10 ms. | aspirational-lcd |
| `C-BENCH-EYES` | Bench end-to-end: real V1 next to v1simple, eyes-only check across all bands and arrows during a 10-minute run. | Manual; documented in the test run log. | manual-lcd |

The render-frame query catalog and native render-operation test anchors live in `tools/perf_correctness_invariants.json` and are checked by `scripts/check_perf_slo_contract.py`. Treat `measured-render-frame+native-render-operation` as stronger than the trace-only layer, but still not as proof of physical LCD pixels; the final LCD/camera gate is still future work.

## Operational SLOs

Capacity, latency, mutex/queue hygiene. These are real and measurable today via the existing SD-backed perf CSV pipeline (`/perf/perf_boot_<bootId>-<token8>.csv`, schema v45). They are scored by `tools/score_perf_csv.py`; the doc/threshold contract is gated by `scripts/check_perf_slo_contract.py` against `tools/perf_slo_thresholds.json`. Bench runs use `tools/score_hardware_run.py` plus `tools/hardware_metric_catalog.json` behind the simplified `tools/bench_score.py` result.

The tables below are the canonical doc-side mirror of `tools/perf_slo_thresholds.json`. They must stay in sync — the contract gate enforces it.

### Run Profiles

- `drive_wifi_off`: normal driving test with setup AP not started (default BOOT behavior).
- `drive_wifi_ap`: driving test while setup AP is intentionally active.

## Hard SLOs (Must Pass)

These are operational gates. Failing one means the device is misbehaving at the capacity/timing layer — a precursor to user-visible problems, but not by itself a correctness violation.

| Metric | Rule | `drive_wifi_off` | `drive_wifi_ap` | Why |
|---|---|---:|---:|---|
| `qDrop` | final == | `0` | `0` | Core BLE queue integrity |
| `parseFail` | final == | `0` | `0` | Parser integrity |
| `oversizeDrops` | final == | `0` | `0` | Packet framing safety |
| `bleMutexTimeout` | final == | `0` | `0` | BLE lockup guard |
| `loopMax_us` | max <= | `250000` | `250000` | Loop stall ceiling |
| `bleDrainMax_us` | max <= | `10000` | `10000` | Main-loop BLE drain target (`<10ms`) |
| `bleProcessMax_us` | max <= | `120000` | `120000` | BLE process budget |
| `dispPipeMax_us` | max <= | `80000` | `80000` | Display pipeline budget |
| `flushMax_us` | max <= | `100000` | `100000` | Storage flush budget |
| `sdMax_us` | max <= | `75000` | `75000` | SD write chunk budget |
| `fsMax_us` | max <= | `50000` | `50000` | FS serve budget |
| `queueHighWater` | final <= | `12` | `12` | Queue occupancy stays at or below half of the `24`-entry default depth |
| `dmaLargestMin` | min >= | `10000` | `10000` | DMA contiguous block floor |
| `dmaFreeMin` | min >= | `20000` | `20000` | DMA free-memory floor |
| `wifiConnectDeferred` | final == / <= | `0` | `5` | NVS/WiFi transition pressure (`==` wifi_off, `<=` wifi_ap) |
| `wifiMax_us` | max <= | `1000` | `6000` | WiFi work budget by profile (drive_wifi_ap uses a cap with headroom above observed same-firmware soaks after the IDF / Arduino_GFX stack bump) |

Bench reports SD latency with an additional CSV-import split so
start/session setup cost is visible but not conflated with steady runtime:
`sd_start_max_peak_us` is the peak `sdMax_us` in the first 10 seconds of the
selected segment and is advisory at 75 ms; `sd_runtime_max_peak_us` is the peak
after that fixed start window and is the hard runtime gate at 50 ms. The raw
`sdMax_us` SLO above remains the whole-segment CSV scorecard ceiling.

## Advisory SLOs (Track/Trend)

These do not fail the run by themselves but are tracked for regression. Like the hard SLOs, these are operational. None of them grade correctness.

| Metric | Rule | Limit | Why |
|---|---|---:|---|
| `cmdPaceNotYetPerMin` | computed <= | `25` | BLE command pacing pressure |
| `displaySkipPct` | computed <= | `20` | Display throttle ratio (skips / (updates + skips)) |
| `displaySkipsPerMin` | computed <= | `120` | UI draw-throttle pressure |
| `reconn` | final <= | `2` | Connection stability trend |
| `disc` | final <= | `2` | Disconnect trend |

## Scoring tool

```bash
python3 tools/score_perf_csv.py /Volumes/SDCARD/perf/perf_boot_1-<token8>.csv \
  --profile drive_wifi_off --session longest-connected
```

Explicit threshold file override:

```bash
python3 tools/score_perf_csv.py ... --slo-file tools/perf_slo_thresholds.json
```

Exit codes:

- `0`: all hard SLOs pass (advisories pass or warn).
- `1`: hard SLOs pass, one or more advisories fail.
- `2`: one or more hard SLOs fail.
- `3`: input/tool error.

## Notes

- The split between **correctness** and **operational** is the load-bearing change in this rewrite. Operational SLOs are valuable; they just stop being the project's correctness grade.
- The contract gate (`scripts/check_perf_slo_contract.py`) parses the **Hard SLOs** and **Advisory SLOs** tables and validates the render-frame correctness query catalog and native render-operation anchors in `tools/perf_correctness_invariants.json`. LCD/pixel-level correctness remains the future gate.
- If operational thresholds change, update `tools/perf_slo_thresholds.json` and the tables above in the same commit. Run `python3 scripts/check_perf_slo_contract.py` after any edits.
- The hard-SLO scorer excludes the first 2 API samples for `wifiMax_us` (TCP cold-start overhead on ESP32 SoftAP).
- Release evidence uses an explicit `--session` selector for multi-session captures.
- Partial-flush shape metrics (`displayPartialFlushLogical*`, `displayPartialFlushRowCallsPeak`, `displayPartialFlushWouldFullRows*`) are diagnostics for the AXS15231B row-by-row partial-flush cost model; they are not hard SLOs unless promoted in `tools/hardware_metric_catalog.json`.
- Why the rewrite: correctness and operational health are separate grades. Keep display correctness invariants explicit so performance tuning cannot mask wrong pixels.
