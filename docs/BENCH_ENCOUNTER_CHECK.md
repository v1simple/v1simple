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
removed from the executable tool, along with `--inspect-transitions` and its
`freeze` / `prepare` / `finalize` qualification workflow.
Old recordings and reports remain intact; use a new analysis directory when
rechecking them with the current tool.

For blinking inputs, the report also measures the distinct visible phases and
their alternations. A readable phase held longer than a complete source-defined
blink cycle is reported with its original images when the source and continuous
recorded observations support that finding. This uses the owning blink code;
it does not impose a new acquisition deadline. Unreadable or missing images
break the evidence for a continuously held phase.

## Run against installed firmware

The command manages an isolated Python environment in `.artifacts/bench/python`,
with the exact image-library versions in `scripts/requirements-bench.txt`.
First use installs those packages (network access may be needed). Subsequent
runs use that environment regardless of the terminal's active Python or
PlatformIO environment. Before camera collection or offline analysis, the
command checks those versions against the reader qualification. It does not
change the qualification or silently accept different dependencies.

When the current tooling and retained local build match the installed image:

```sh
./bench.sh --replay --camera --no-flash
```

When tooling has advanced since the firmware build, use its original qualified
upload or no-flash recording and its exact application binary:

```sh
./bench.sh --replay --camera --no-flash \
  --resident-recording /path/to/prior/upload/replay \
  --resident-image /path/to/firmware.bin
```

This performs a fresh reset-to-ready identity check, controlled replay, camera
capture and automatic analysis. It does not upload firmware. The source tree
must be clean; commit the tested tooling before collection.

The resident options require each other. The collector verifies the prior clean
source commit, build manifest, serial record, application bytes and embedded
ELF hash before operating the device. Its fresh BOOT must match that source and
image. Each new run retains the reference evidence under `resident_reference/`.
The exercised firmware identity and current tooling identity are recorded
separately. Settings are observed independently in the fresh collection.
Use the original recording for that build, not a later recording which itself
references another build. A no-flash reference does not claim an upload occurred.

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

Analysis uses four independent frame readers and returns every reading in its
original order. `--reader-workers 1` selects serial processing for comparison;
values 1–8 are supported. Worker failure does not trigger a silent retry. The
recorded pixels, timestamps and uncertainty rules are the same for every setting.

## Observe configured persistence

Use the normal WebUI to set the active Auto-Push slot's Alert persistence to
2 seconds, then reboot to normal operation. Record the old setting so it can be
restored afterward. Run the 64-second radar sequence in its 90-second collection:

```sh
./bench.sh --replay --camera --no-flash --persistence-coverage \
  --resident-recording /path/to/original/qualified/replay \
  --resident-image /path/to/firmware.bin
```

The report shows primary retention then clearing, live-alert preemption and
secondary-card retirement, with first/last original images and observed times.
Positive persistence is evaluated as a sequence of display stages; a single
fixed idle target cannot describe it. Missing or unreadable required stages
remain incomplete. Dim numeric glyphs still require supported reader evidence;
enabling the scenario does not establish that those glyphs can be read.
This sequence does not exercise the separate wired ALP input.

## Reuse independent readings

When only input interpretation or behavior comparison has changed, reuse the
independent readings of that exact recording without repeating image recognition:

```sh
./bench.sh --analyze-recording /path/to/retained/replay \
  --reuse-readings /path/to/earlier/encounter-check/result.json
```

The tool verifies the recording, image-reader implementation, complete raw
readings and original witnesses against their retained hashes. It compares
inputs and behavior anew; it never reuses old expectations or verdicts. A
different capture or changed reader is rejected. This option is offline only.

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
| `NO_DIFFERENCES_OBSERVED` | Required target/phase or persistence-stage observations and recorded-frame coverage completed, with no supported content findings. Other acquisition content and unreadable intervals can remain. | 0 |
| `MEASUREMENT_INCOMPLETE` | Evidence, qualification, coverage or an unobserved complete target prevents that answer. | 2 |

These are observation results, not a blanket firmware-health certification.
Unresolved images before and after the first target remain in the report. A
first match does not establish the whole hold correct, and a
camera image that spans an ordinary transition does not automatically prove a
firmware defect. Later definite departures and supported contradictory final
content remain visible.

The full-interval table partitions recorded frames after complete host input
into matches, previous-input acquisition values, other acquisition content,
unresolved frames before or after the first target, and later contrary content.
Earlier frames and unavailable input boundaries are counted separately.
Acquisition literals that match neither current nor preceding values have their
own original-image witnesses, including when another field is unreadable.
Acquisition timing does not make those literals source-permitted; their physical
cause remains unassigned. No response deadline is introduced.

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

The reader uses the registered `SCAN` landmark and fixed pixel geometry. It
requires visible screen witnesses before interpreting absence. Definite
canonical frequency strokes determine literal digits even with uneven
brightness; brightness anomalies remain available separately. Partial strokes
refuse a digit. Bars require supported cell geometry and contiguous fill.
Arrow measurements retain each direction independently, including partial or
faint color without inventing its physical cause.

Main labels, arrows and bars distinguish painted resting gray from active ink.
The L label supplies a per-image resting reference within this radar-only scope.
A dark L and at least one other dark band label select the older palette, where
muted active ink can be as dim as the new resting gray. A neutral gray L selects
the brighter palette only when its stem and foot have local contrast and every
radar label has painted support. These checks prevent an erased L or a gray L
beside dark band bodies from selecting the wrong palette. An illuminated or
unclear reference leaves these measurements unresolved. Older frames with all
three radar labels lit lack the second dark reference and also remain unresolved.
The reference uses pixels only, without packet expectations or firmware-version
selection; the very dim older L is not claimed to have a verified font shape.
For the calibrated camera profile, neutral resting interiors occupy levels
55–110, bright active ink starts at 120, and saturated colored ink retains the
45-level floor. Neutral ink between those ranges remains unresolved. Whole
interiors and contrary-stroke checks reject mixed activity; a visible gray
background is not counted as an alert. These limits do not qualify arbitrary
user colors or exposure settings, and do not change frequency or card thresholds.
Resting-body checks tolerate at most one connected midlevel excursion fitting
within 4×2 source pixels (either orientation), with all channels below 130 and
no dark pixel. Such small excursions occur in otherwise clear original images;
their physical origin is unknown. Bright ink, dark holes, larger or attached
marks, and multiple coherent excursions remain unresolved. This is a declared
spatial limit, not pixel-perfect drawing validation.

The dark idle `--.---` placeholder is measured against local background, with
all five dash interiors and the separate decimal required. A region below the
numeric reader's brightness threshold is no longer automatically called blank.
Partial dark marks remain unresolved.

For unresolved frequency readings, a second fixed check aligns the complete
startup SCAN shape with a retained reference using OpenCV. A separate startup
counter validates the alignment. Both the original startup still and its
preflight metadata must be bound to the recording. This correction applies only
to the primary-frequency field; other fields keep their existing registration.
If calibration refuses, the ordinary reader remains in use and the reason is
retained in the report evidence.

The additional idle reading requires a Stb-tester template match, every dash
and the decimal, and a separate check for extra gray or red strokes. Its fixed
background envelope was learned from development images, with a three-level
additional contrast margin. That margin is above measured background texture;
it is not a guarantee to detect every three-level mark. No neighboring frames,
expected values, timing thresholds or per-run model training participate.
Existing definite readings are retained. Model assets and image-library
versions are included in qualification and invalidate incompatible cached
readings. Stb-tester uses supplied images only, with capture and OCR disabled.

When all five original numeric glyphs are definite and only the decimal is
unresolved, the startup alignment can also recheck the numeric field. The same
reader must observe the decimal and clear glyph interiors, and repeat the
original five digits exactly. This route does not admit dim, noncanonical or
mixed-stroke refusals. Existing definite readings remain unchanged.

Secondary-card text uses local Apple Vision OCR. Accepted text must satisfy the
fixed visible format; expected values never repair OCR output. No remote image
model or network service participates in the reader.
When OCR returns the band and complete frequency as two separate observations,
their unique literal candidates may be joined only with consistent left-to-right
and same-line geometry. Missing letters, digits or punctuation are not supplied.
The original band-glyph, direction and bar checks still apply.

The default qualification manifest is
`.artifacts/bench/qualification/encounter-reader.json`. Override it with
`BENCH_ENCOUNTER_QUALIFICATION` when using another retained qualification.
Qualification binds the pixel-reading method to independently checked reference
images and controls. It does not impose a firmware response deadline.

An explicit `reanalyze-static --primary-frequency-reference` supplement can
correct a prior frequency reference using independently recorded observations.
It retains the original reference unchanged and requires held-out original
images plus missing, partial and invalid glyph/decimal controls. Corrected
references and reader refusals remain visible in the qualification evidence.
Frequency supplements can retain per-image startup sources; verification
recomputes their calibration rather than trusting a stored transform. The
current method must demonstrate calibrated admissions and residual-stroke
refusals on independently labeled images from multiple startup sources.
An additional secondary-card reference can retain new opaque image observations
without replacing the historical card packet. Verification checks the exact
original crops, per-image startup inputs, complete card values and partial
identities. Reader 25 must demonstrate actual independently supported numeric
decimal and split-card admissions; saved summary counts cannot supply that proof.
Reader 26 additionally requires an independently supported complete-band pixel
reading. Its missing-band fallback is limited to a complete K glyph and the
single-letter prefix spacing beside an unchanged, uniquely recognized numeric
token. A K-shaped initial alone cannot supply the band; suffix, damaged-letter,
visibility, direction and bar uncertainty remain explicit.

Static reanalysis may validate uncommitted tooling. It records the actual Git
state and exact method digest, and refuses publication if the source changes
during verification. This allows the pending implementation to be tested before
commit. The clean-source requirement for physical collection remains in place.

Existing `--qualification-capture` remains available for retaining camera pixels
without running either automatic pixel reader. It cannot be combined with
`--compare-to`; `QUALIFICATION-CAPTURED` means the recording exists, not that
firmware behavior passed. Static reader qualification remains available through
`reanalyze-static`. Historical policy code remains in Git history and retained
run method snapshots; current analyses use `--observe-behavior`.
