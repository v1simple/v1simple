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
- never includes the private input path in replay evidence;
- writes path-free resolved scenario values only when `--scenario-evidence` is
  explicitly given;
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
.build/v1replay bench --scenario /external/input.json \
  --scenario-evidence /external/run/replay_scenario.json --machine-events
.build/v1replay export --bench --format csv
.build/v1replay export --synthetic --format lightblue
.build/v1replay idle
.build/v1replay crib
```

Without `--scenario`, `bench` uses the generated Phase 0 stimulus. It runs at
approximately 3 Hz for 276 seconds
and covers a resting lead, K and Ka ramps, a priority handoff, complete two- and
three-row alert tables, card removal and restoration, a long Ka approach, and a
32-second resting tail. The scenario owns those idle periods, so generic
`--idle-lead` and `--idle-tail` values are not added to it. It waits for the
display subscription and the firmware's alert-data request before starting;
if either becomes unavailable during playback, its clock pauses until both are
ready again. `--no-alerts`, `--no-wait`, and `--always-alerts` are therefore
rejected for managed playback.
`--exit-on-complete` closes the peripheral and returns after the resting tail.
The unified bench instead uses `--machine-events`, keeps the peripheral alive
through the complete external evidence window, and then stops its process group.
Core and display windows use the same managed emulator in idle mode, so the
complete bench never depends on a physical V1.

Bench playback defaults to the `scenario` priority-arrow blink profile. As a
provisional generated assumption, it blinks only during the 19-second authored
multi-alert interval (57 samples) and leaves all single-alert periods steady.
This is deliberately isolated in `BenchScenario.swift` so later external input
evidence can replace the assumption without changing firmware or packet
semantics. Physical display behavior is checked by the camera leg.
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

The same input can drive the managed bench transport:

```bash
.build/v1replay bench --scenario /path/outside/any/git/replay-input.json
```

Managed external playback uses the encounter's resolved offsets and idle samples,
waits for the display subscription and alert-data request, and freezes that
timeline if readiness is lost. `--scenario-evidence <output>` writes the exact
resolved sample values used for packet construction as JSON. The document and its
`scenario_resolved` machine event contain a SHA-256 hash, counts, and a generic
origin token, but never the private input path.

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
cache. Raw camera recordings can retain initial card appearance, persistence,
slot reuse, and removal during Phase 0.

### Playback keys

| Key | Action | Key | Action |
|---|---|---|---|
| `space` | pause or resume | `n` | step one sample |
| `.` | next strength change | `,` | previous change |
| `r` | restart | `m` | cycle mute override |
| `]` / `[` | double or halve speed | `p` | toggle display power |
| `1` | reset to 1× | `q` | quit |

## Protocol behavior

The tool advertises the V1 service, four core characteristics, and two
compatibility characteristics. B2CE subscription is transport readiness; managed
bench playback also waits for v1simple to request alert data. B4E0 remains
optional because the packets generated here fit B2CE.

The emulator retains incomplete command frames and drains complete frames in
wire order. Incoming v1simple commands require a valid checksum and the
`DA E6` request header. The connect handshake answers version and all-volume
queries, and alert rows are withheld until `reqStartAlertData` unless
`--always-alerts` is selected.

Each active bench step sends a complete alert table with one priority row,
followed by display data derived from that row. Empty steps send an explicit
count-zero table. Generated broadcast information uses `D8 EA`; targeted
replies use `D6 EA`. `--header draft`, `--blink-bogey`, and the
priority-arrow blink profiles select deterministic stimulus variants.

With `--machine-events`, notification events include sequence, characteristic,
payload identity, and host monotonic time. CoreBluetooth accepting an update
does not prove that the DUT received, parsed, or rendered it.

Host tests cover packet construction and session decisions. BLE subscription,
notification delivery, firmware rendering, and visible pixels require
integration, serial, or camera evidence.

## Verification

```bash
python3 verify/check_publication_safety.py
python3 verify/verify_protocol.py
env DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer \
  CLANG_MODULE_CACHE_PATH=.build/clang-cache \
  xcrun swift test --disable-sandbox
```

The publication check enforces the source-only tree and scans publishable text
for common local-path, timestamp, credential, and private-key markers. The
protocol verifier compiles `V1Protocol.swift`, feeds its in-memory packet matrix
to the firmware parser, and never reads external replay input. The Swift test
suite covers the scenario timeline and CSV.

## Limitations

- macOS controls the BLE connection interval.
- Legacy whole-second timestamps are spread evenly within each second.
- The tool intentionally refuses all replay input stored in Git, including
  private repositories.
- A synthetic pass does not replace raw capture or real-V1 camera evidence for
  a claimed firmware fix.
