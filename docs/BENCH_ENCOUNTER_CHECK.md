# External firmware visual testing

The normal camera command applies controlled V1 inputs, reads the physical
screen independently, and reports how the observed display compares with the
inputs, recorded settings and supported firmware behavior. The reader receives
registered RGB images, not expected values or packet contents. Expected states
are compared after reading pixels.

The active command uses `firmware_visual_behavior/v1`. It reports observed
values, the first observed target, supported discrepancies, their original
images, unresolved intervals and capture coverage. Appearance times are
measurements from complete host input acceptance to camera capture markers.
They are not firmware-only latency measurements or pass/fail deadlines.

The former `VISIBLE_EVENT_PRESENTATION/v3` policy and its 100 ms deadline are
historical analysis. They do not control ordinary live or offline bench results.
Old recordings and reports remain intact; use a new analysis directory when
rechecking them with the current tool.

## Run against installed firmware

When the current tooling and retained local build match the installed image:

```sh
./bench.sh --replay --camera --no-flash
```

When tooling has advanced since the firmware upload, use that prior qualified
upload recording and its exact application binary:

```sh
./bench.sh --replay --camera --no-flash \
  --resident-recording /path/to/prior/upload/replay \
  --resident-image /path/to/firmware.bin
```

This performs a fresh reset-to-ready identity check, controlled replay, camera
capture and automatic analysis. It does not upload firmware. The source tree
must be clean; commit the tested tooling before collection.

The resident options require each other. The collector verifies the prior clean
source commit, upload manifest, serial record, application bytes and embedded
ELF hash before operating the device. Its fresh BOOT must match that source and
image. Each new run retains the reference evidence under `resident_reference/`.
The exercised firmware identity and current tooling identity are recorded
separately. Settings are observed independently in the fresh collection.

**Without `--no-flash`, `--replay --camera` builds and flashes firmware.**
`--all --camera` also runs the existing core and display collection legs; visual
behavior analysis runs on the replay leg.

## Recheck recordings and compare builds

Analyze an existing recording without operating hardware:

```sh
./bench.sh --analyze-recording /path/to/retained/replay
```

This writes a new ordinary run directory, leaves the recording unchanged and
identifies the recorded firmware. It does not evaluate today's connected DUT.
Repeat `--range START:END` to select intervals in seconds from the first replay
request. Without ranges, the analyzer examines the full authored sequence.

Compare the new observation with an earlier behavior result:

```sh
./bench.sh --analyze-recording /path/to/current/replay \
  --compare-to /path/to/baseline/encounter-check/result.json
```

`--compare-to` also works on a live camera command. Comparison requires the same
ordered authored input, effective settings, expected targets, reader method and
behavior rules. Firmware and tooling versions are reported separately and may
differ. Incompatible comparisons explain the mismatch; they do not manufacture
a firmware regression.

The comparison shows changed target observations, newly observed and no-longer
observed discrepancy literals, appearance and occurrence times, unresolved
measurements and coverage. A disappeared finding beside unreadable images is
not proof of a repair. Independent run-clock offsets are not timing changes.

## Read the result

The console links `encounter-check/report.html`. The report retains original
frame witnesses so an observed symptom can be traced to the input and expected
behavior, then checked after a firmware change. `result.json` contains the same
small behavior summary for subsequent comparisons.

| Visual result | Meaning | Exit |
| --- | --- | ---: |
| `DIFFERENCES_FOUND` | Qualified evidence contains supported discrepancy findings. Other unknowns remain visible. | 1 |
| `NO_DIFFERENCES_OBSERVED` | Every requested target was observed, recorded-frame coverage completed, and no supported discrepancy was found. | 0 |
| `MEASUREMENT_INCOMPLETE` | Evidence, qualification, coverage or an unobserved complete target prevents that answer. | 2 |

These are observation results, not a blanket firmware-health certification.
Unresolved transition images remain in the report even when every target was
later observed. A first match does not establish the whole hold correct, and a
camera image that spans an ordinary transition does not automatically prove a
firmware defect. Later definite departures and supported contradictory final
content remain visible.

The console reports targets observed per event, events with findings, total
findings, read/available recorded frames, unresolved frames and unresolved field
observations. Counts are checked against the individual event evidence. A
missing report, inconsistent counts or mismatched analyzer exit cannot become
a successful result.

Collection-only evidence retains its existing exit 1. Missing camera preflight
also exits 1; hard collection failures exit 2. Without `--camera`, visible
behavior is `NOT_EVALUATED`. The older sampled-counter check is a separate
observation and does not determine the visual behavior result.

## Supported visible behavior

The fixed display reader currently covers normal-runtime V1 X, K and Ka with
one primary alert and up to two associated secondary cards. Same-band alert
frequencies must be more than 5 MHz apart. It measures:

- counter or mode glyph;
- primary frequency;
- active band labels;
- main direction arrows;
- main strength bars;
- secondary card identity, frequency, direction and strength;
- literal `MUTED` badge.

The observer preserves complete, partial, faint, ambiguous and unreadable
measurements as distinct evidence. It does not turn faint outgoing arrow color
into absence, infer missing digits from the requested value, or hide a wrong
card because another field cannot be read. Input changes, blink behavior and
stateful mute presentation are evaluated with their context.

The supported scope does not include audio, ALP interaction, laser, Ku,
junk/photo rows, or layouts outside the registered display. Unsupported inputs
and unknown settings must remain explicit. Findings concern the exercised
recording and cannot establish behavior of unexercised firmware paths.

## Reader and retained evidence

Reader V9 uses the registered `SCAN` landmark and fixed pixel geometry. It
requires visible screen witnesses before interpreting absence. Definite
canonical frequency strokes determine literal digits even with uneven
brightness; brightness anomalies remain available separately. Partial strokes
refuse a digit. Bars require supported cell geometry and contiguous fill.
Arrow measurements retain each direction independently, including partial or
faint color without inventing its physical cause.

Secondary-card text uses local Apple Vision OCR. Accepted text must satisfy the
fixed visible format; expected values never repair OCR output. No remote image
model or network service participates in the reader.

The default qualification manifest is
`.artifacts/bench/qualification/encounter-reader.json`. Override it with
`BENCH_ENCOUNTER_QUALIFICATION` when using another retained qualification.
Qualification binds the pixel-reading method to independently checked reference
images and controls. It does not impose a firmware response deadline.

Existing `--qualification-capture` remains available for retaining camera pixels
without running either automatic pixel reader. It cannot be combined with
`--compare-to`; `QUALIFICATION-CAPTURED` means the recording exists, not that
firmware behavior passed. Reader qualification workflows and old policy
implementation remain available for their explicit purposes and historical
reproduction, outside the normal observation result.
