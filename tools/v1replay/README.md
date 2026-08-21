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
through the end of the firmware metrics window, and then stops its process group.
Core and display windows use the same managed emulator in idle mode, so the
complete bench never depends on a physical V1.

The unified runner alone uses `bench --handshake-only` before the scored replay.
That mode reacts directly to each accepted start-alert request and ensures one
canonical count-zero alert row is queued until CoreBluetooth accepts it for
delivery; it does not depend on the player's polling cadence. Once delivery is
recorded it stays quiet and alive until the runner removes the peripheral. Its
bounded packet log contains only the two targeted startup replies and that clear
row; it does not enter the scenario or idle stream.

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
- `V1-CONNECT-READBACK-001` pins V1Simple's automatic connected readback: after
  short-notification setup and its alert-stream request, the accepted version
  query precedes the accepted all-volume query on the selected short command
  characteristic. A transient local pacing or transport deferral retains the
  unsent query for a later loop instead of skipping it. Exact retry count,
  delay, and reply arrival order are not gated.
- `V1-RECONNECT-SESSION-001` pins a managed same-boot reconnect boundary. A
  handshake-only process must independently complete one active startup epoch,
  then disappear; after the still-open serial session observes the board's V1
  cleanup, a replacement process must independently complete a fresh epoch.
  The two bounded ledgers are distinct, so neither can borrow readiness or
  replies from the other. Schema 2 records anonymous epoch-relative monotonic
  milliseconds for every event. One initial start plus at most four recovery
  starts are legal before the delivered clear row when every consecutive retry
  is at least 1000 ms later. The evidence is
  `ConnectionStateModule::DATA_REQUEST_INTERVAL_MS` in
  `src/modules/ble/connection_state_module.h`, the strict `>` rate-limit check
  in `src/modules/ble/connection_state_module.cpp`, and
  `V1SessionContractTests.testDuplicateStartIsDeterministicAndStopEmitsNoReply`.
  Because the ledger stores integer milliseconds, the gate accepts a measured
  1000 ms boundary for an underlying interval that can be just over 1000 ms.
  The five-start limit leaves the existing 12-event ledger cap enough room to
  record a violating sixth start alongside the six non-start events. A sixth
  start, a faster retry, or any start after stream delivery fails. Historical
  schema-1 evidence remains readable only for the unambiguous single-start
  shape because it cannot prove retry spacing. This is host emulator and
  board-cleanup integration evidence, not proof of physical-V1 reconnect
  timing, cache or bond behavior, or control persistence. The reconnect
  evidence alone does not prove board-side all-volume parsing; that is joined
  separately under `V1-ALL-VOLUME-001`.
- `V1-VERSION-REPLY-001` pins a valid version request, its checksummed
  V1-to-app reply, and selection of the B2CE short-display channel.
- `V1-ALL-VOLUME-001` pins the four ordered current/saved volume fields and the
  B2CE short-packet route. Equal current and saved values are emulator fixture
  configuration, not a universal device default. For the default-v4.1038
  managed replay, the independently decoded replacement ledger must contain the
  canonical delivered reply and the same replacement metrics window must record
  exactly one canonical four-field parser commit. The counter is recorded only
  after the four values enter parser state; it does not claim that the firmware
  independently validated the packet checksum or that a physical V1 was used.
- `V1-CONTROL-MODE-001` pins the default-US one-byte mode commands `01`, `02`,
  and `03`. They update the current mode without a semantic reply. Later idle
  display information carries the matching mode glyph and Aux1 mode bits; active
  display information retains those mode bits while its glyph shows the alert
  count.
- `V1-CONTROL-VOLUME-001` pins V1Simple's exact three-byte `aux0=00` volume
  write: main and muted values are `0...9`; it updates the current pair, leaves
  the saved pair unchanged, and produces no semantic reply. For V4.1028+, a
  full eight-byte ID31 payload carries current main/muted volume in Aux2's
  high/low nibbles; saved values are not carried. Vendor documentation assigns
  `aux0` bit `04` to saving on V4.1037+, but that branch is host-modeled
  compatibility, not emitted by V1Simple or physically confirmed. Before
  V4.1037, the emulator leaves saved state unchanged because reserved-bit
  handling is unknown. A later all-volume reply carries both pairs.
- `V1-ALERT-STREAM-CONTROL-001` pins start and stop as state transitions with no
  invented immediate reply. Repeating start while already enabled is an
  idempotent request, not a second state transition. Delivery already queued
  around a stop and the timing of the first or last alert row remain provisional
  and non-gating.
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
- `V1-DISPLAY-FRAME-001` pins the default-v4.1038 full eight-byte B2CE display
  payload after the table, with its count, meter, band, direction, and mute
  state derived from the flagged priority row. Aux1 carries the current
  default-US mode even during an active alert; Aux2 carries current main and
  muted volume in its high and low nibbles, never saved values. Generated
  display and alert information use the `D8 EA` broadcast header; targeted
  request replies remain `D6 EA`. Identical steady image planes are a
  deterministic fixture choice.

These tests cover pure session decisions and the pure playback packet plan.
Actual notification delivery, subscription mechanics, and characteristic
permissions remain integration or bench evidence. Mute on/off and display-on
accept empty payloads; display-off accepts empty, `00`, or `01`. These state
commands do not invent same-ID replies. Mode and volume writes accept their
validated one- and three-byte payloads without an immediate packet reply.
Feedback rendering and timing, other reserved volume-control bits, disconnect
restore, power-cycle persistence, and non-US mode variants remain outside this
host-state gate.

Managed replay writes two bounded anonymous startup-handshake ledgers: one for
the quiet preflight process and one for the scored replacement. Neither contains
a central identifier or absolute timestamp; schema 2 contains only epoch-relative
monotonic milliseconds. Neither ledger is a general packet transcript.
The grader decodes them separately and requires exactly one connection epoch in
each with B2CE subscription, one or more accepted pre-stream start requests
within the bounded retry contract, accepted version and all-volume requests,
CoreBluetooth-accepted short replies, and alert-stream start. The preflight's
only stream packet must be the canonical count-zero clear row. The scored
replacement window additionally requires one boot-cumulative canonical
all-volume parser commit, while the independently decoded replacement ledger
owns the reply's exact bytes, checksum, and route. Together those two sources
provide bounded integration evidence that a canonical reply from the controlled
emulator reached replacement board parser state; the counter alone does not
identify exact values, checksum, header, or route. Preflight board-side
consumption remains unclaimed. Each ledger models one logical short-notify
session; concurrent short subscribers end the active evidence epoch instead of
being merged.

With `--machine-events`, every queued notification emits
`notification_requested`; every successful CoreBluetooth `updateValue` emits
`notification_accepted`. Both records carry a process-global TX sequence,
the whole-notification FNV-1a32 digest used by DUT causal traces,
optional stimulus sequence and emission ordinal, characteristic, exact payload
hex, SHA-256 digest, and host monotonic nanoseconds. The shared identities join
repeated equal packets without treating payload equality as causality.
CoreBluetooth acceptance means the host stack accepted the update for delivery;
it does not prove DUT receipt, parsing, rendering, or pixels.

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
queue drops, or parse failures. The replacement window must also advance
`v1AllVolumeParsed` exactly once; earlier preflight credit cannot satisfy it.
Display-update and notify-to-display latency counters provide transport context;
they are not synthetic pass/fail proof.

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
