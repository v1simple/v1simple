# v1replay

A macOS command-line test tool that advertises the Valentine One Gen2 BLE
service and drives v1simple through the same connection and packet paths used by
hardware.

This directory is intentionally source-only. It contains no recorded input,
captures, exported packets, screenshots, videos, or replay fixtures. The demo
and multi-alert bench are generated deterministically in memory.

## Privacy boundary

Private replay input must remain outside every Git checkout. `v1replay` resolves
the supplied path and refuses to open it when any parent directory contains a
`.git` marker. It also:

- never prints input paths, filenames, source labels, notes, or timestamps;
- discards absolute timestamps after deriving relative playback offsets;
- never copies or normalizes input;
- writes exports to standard output only; and
- runs a source-tree safety check before every build.

The local `.gitignore` blocks common capture, data, media, and export formats as
defense in depth. Do not bypass it with a forced Git add.

The repository's staged-snapshot privacy scanner independently enforces this
source allowlist and detects replay-shaped JSON even when it is renamed. The
tracked pre-commit and pre-push hooks both run that scanner; activate them once
per clone with `./scripts/setup-hooks.sh` from the repository root.

## Build

```bash
cd tools/v1replay
./scripts/build.sh
```

The build needs the Xcode command-line tools and no network access. It uses
`swiftc` directly so the Bluetooth permission descriptions in `Resources/Info.plist`
can be embedded in the executable.

The first run may request Bluetooth permission for the terminal application
that launched the tool. If advertising does not start, enable that application
under System Settings → Privacy & Security → Bluetooth.

## Safe use

Generated stimuli require no data file:

```bash
.build/v1replay demo
.build/v1replay demo --paused
.build/v1replay demo --speed 4 --loop
.build/v1replay bench
.build/v1replay bench --blink-profile steady
.build/v1replay bench --blink-profile stress
.build/v1replay bench --exit-on-complete
.build/v1replay export --bench --format csv
.build/v1replay export --synthetic --format lightblue
.build/v1replay idle
.build/v1replay crib
```

`bench` is the Phase 0 stimulus. It runs at approximately 3 Hz for 254 seconds
and covers a resting lead, K and Ka ramps, a priority handoff, complete two- and
three-row alert tables, card removal and restoration, a long Ka approach, and a
10-second resting tail. The scenario owns those idle periods, so generic
`--idle-lead` and `--idle-tail` values are not added to it. It waits for the
display subscription and the firmware's alert-data request before starting;
if either becomes unavailable during playback, its clock pauses until both are
ready again. `--no-alerts`, `--no-wait`, `--always-alerts`, and `--rate` are
therefore rejected for this fixed-cadence command.
`--exit-on-complete` closes the peripheral and returns after the resting tail.
The unified bench instead uses `--machine-events`, keeps the peripheral alive
through the end of the firmware metrics window, and then stops its process group.
Core and display windows use the same managed emulator in idle mode, so the
complete bench never depends on a physical V1.

Bench playback defaults to the `scenario` priority-arrow blink profile. As a
provisional generated assumption, it blinks only during the 19-second authored
multi-alert interval (57 samples) and leaves all single-alert periods steady.
This is deliberately isolated in `BenchScenario.swift` so encounter evidence
that includes the real V1's display image planes can replace the assumption
without changing firmware or packet semantics. The firmware's current compact
encounter CSV records alert-table assignments, not those image planes, so it
cannot establish blink intent by itself.
`--blink-profile steady` is the negative control; `--blink-profile stress`
blinks every active priority arrow (708 samples) as the worst-case repaint
control. `--blink-arrow` remains a legacy alias for the stress profile.

The long approach derives only the aggregate cadence, approximate durations,
and strength envelope recorded during diagnosis: roughly 95 seconds mostly at
one bar, a rise toward three bars, a 20-second six-bar plateau, and a collapse
back toward one. Its broad direction transitions are authored deterministic
stimulus because the private input's exact direction sequence is not available.
It is not a sample-for-sample copy and no replay fixture is stored here.

Private external input can still be replayed when it is stored outside every
Git checkout:

```bash
.build/v1replay play /path/outside/any/git/replay-input.json
```

Relative `offsetSeconds` values are preferred. Legacy ISO-8601 `timestamp`
values are accepted for compatibility, converted to relative offsets in memory,
and discarded. Other consumed sample fields are `strength`, `frequencyGHz`,
`direction`, and `muteState`; top-level protocol fields are `band` and
`frequencyMHz`.

The `export` command prints derived packet streams to standard output. Bench
export intentionally supports only the machine-readable CSV format. Keep any
redirected output outside Git as well. A later external bench runner can save:

```bash
.build/v1replay export --bench --format csv > /external/run/replay/expected.csv
```

The destination must remain outside every Git checkout. `v1replay` itself does
not create `expected.csv` or any other artifact.

### Expected bench CSV

`export --bench --format csv` emits one row per nominal 3 Hz timeline step:

```text
offset_s,phase,active_alert_count,priority_frequency_mhz,priority_band,priority_direction,priority_bars,scenario_arrow_blink,card_1_frequency_mhz,card_1_direction,card_1_bars,card_2_frequency_mhz,card_2_direction,card_2_bars
```

Frequencies are integer MHz, bands are stable `K`/`Ka` tokens, directions are
`FRONT`, `SIDE`, or `REAR`, and absent priority/card fields are empty. The
`scenario_arrow_blink` column records authored scenario intent independently of
an explicit steady or stress control. Priority fields drive the main display
packet. Card fields list active secondary alerts in alert-table order with their
raw 0–8 strength projected onto the six-cell card meter as `(raw * 6 + 4) / 8`.

The CSV describes transmitted active state, not the renderer's card-grace
cache. Initial card appearance, persistence, slot reuse, and removal remain
camera-reviewed during Phase 0.

### Playback keys

| Key | Action | Key | Action |
|---|---|---|---|
| `space` | pause or resume | `n` | step one sample |
| `.` | next strength change | `,` | previous change |
| `r` | restart | `m` | cycle mute override |
| `]` / `[` | double or halve speed | `p` | toggle display power |
| `1` | reset to 1× | `q` | quit |

## Protocol behavior

The public contract inventory uses these stable behavior IDs:

- `V1-BLE-IDENTITY-001` pins the service UUID, four core characteristic UUIDs,
  and the separate two-characteristic compatibility surface. It covers literal
  identity only; CoreBluetooth behavior remains integration or bench evidence.
- `V1-SESSION-TRANSPORT-001` pins the emulator's ordered command stream: an
  incomplete frame is retained, valid complete frames are handled once in wire
  order, and large coalesced writes are drained before the incomplete residual
  tail is bounded. B2CE subscription is core readiness; the alert bench also
  requires alert-stream enablement. B4E0 remains optional capacity for genuinely
  long packets and is not required for the current 15-byte alert rows.
  Fragmented writes are an emulator robustness guarantee, not a claim about
  typical physical-device boundaries. Inbound checksums remain mandatory even
  when generated checksums are disabled. The strict `DA E6` request header is
  the V1Simple-host policy; `E6` is not claimed as every possible client's
  universal origin.
- `V1-VERSION-REPLY-001` pins a valid version request, its checksummed
  V1-to-app reply, and selection of the B2CE short-display channel.
- `V1-ALL-VOLUME-001` pins the four ordered current/saved volume fields and the
  B2CE short-packet route. Equal current and saved values are emulator fixture
  configuration, not a universal device default.
- `V1-ALERT-STREAM-CONTROL-001` pins start and stop as state transitions with no
  invented immediate reply. Delivery already queued around a stop and the
  timing of the first or last alert row remain provisional and non-gating.
- `V1-USER-BYTES-001` pins the six-byte payload shape, version-aware read/write
  state, and the B2CE read response. Under the default v4.1038 identity, writes
  preserve `FF FF` in the final two positions. Writes have no invented immediate
  reply; verification uses a later readback. Gen2 full six-byte storage begins
  with v4.1039.
- `V1-ALERT-TABLE-001` pins complete one-based tables on B2CE: rows repeat the
  total, carry exactly one priority flag for an authored active sample, and are
  planned in row order before the display packet. Empty samples use the
  emulator's explicit all-zero count-zero clear fixture. The chosen inverse
  raw-strength bytes and row transmission order are deterministic emulator
  choices, not claims that every physical V1 emits those exact bytes or cadence.
- `V1-DISPLAY-FRAME-001` pins the full eight-byte B2CE display payload after the
  table, with its count, meter, band, direction, and mute state derived from the
  flagged priority row. Generated display and alert information use the `D8 EA`
  broadcast header; targeted request replies remain `D6 EA`. Identical steady
  image planes are a deterministic fixture choice.

These tests cover pure session decisions and the pure playback packet plan.
Actual notification delivery, subscription mechanics, and characteristic
permissions remain integration or bench evidence. Mute on/off and display-on
accept empty payloads; display-off accepts empty, `00`, or `01`. These state
commands do not invent same-ID replies. Mode and volume writes accept their
one- and three-byte payloads without an immediate packet reply.

The tool advertises the V1 service, four core characteristics, and two
compatibility additions. It answers the handshake v1simple performs on connect
and emits display and alert-table rows. Alert rows wait for
`reqStartAlertData` unless `--always-alerts` is used.

Every active bench step sends a complete one-, two-, or three-row B2CE table
with exactly one priority flag. The K alert remains row 1 when Ka becomes the
row-2 priority, so the stimulus exercises priority selection rather than relying
on row order. Empty steps send an explicit count-zero table. The B2CE bogey
count matches the active-row count, and its meter, band, and direction come from
the priority alert.

Playback uses `D8 EA` for generated broadcast information and `D6 EA` for
targeted replies by default. Its steady bogey planes remain a deterministic
stimulus choice; `--header draft` and `--blink-bogey` select fixture-compatible
alternatives. Priority-arrow blink profiles clear only the selected direction
bit from image2; timing and visual cadence remain non-gating bench/camera
evidence.

The pure plan does not prove CoreBluetooth subscription properties, notification
delivery, or a future long-packet chunk wrapper. B4E0 segmentation is outside
this slice and is not synthesized for packets that already fit B2CE.

The tool is a stimulus source, not the sole oracle. Literal host contract tests,
firmware parser tests, and attached-device bench evidence each own a distinct
layer of assertions.

With metrics reset before a complete bench run, the generated timeline presents
708 complete active tables, including 30 three-bogey tables, and 708 selections
with a valid priority-row flag. Healthy informational evidence therefore has
matching `alertTablePublishes`, `alertTablePublishes3Bogey`, and
`prioritySelectRowFlag` relationships, with no assembly timeouts, invalid
priority selections, invalid-band rows, live-display priority skips/fallbacks,
queue drops, or parse failures. Display-update and notify-to-display latency
counters provide transport context; they are not synthetic pass/fail proof.

## Verification

```bash
python3 verify/check_publication_safety.py
python3 verify/verify_protocol.py
```

The publication check enforces the source-only tree and scans publishable text
for common local-path, timestamp, credential, and private-key markers. The
protocol verifier generates its complete test matrix in memory; it never reads
external replay input. It also compiles the portable Swift scenario model and
checks its emitted timeline and CSV without creating a replay fixture.

## Limitations

- macOS controls the BLE connection interval.
- Legacy whole-second timestamps are spread evenly within each second.
- The tool intentionally refuses all replay input stored in Git, including
  private repositories.
- A synthetic pass does not replace raw capture or real-V1 camera evidence for
  a claimed firmware fix.
