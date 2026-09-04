# Recorded encounter check

The offline encounter reader compares the visible counter, primary frequency,
active bands, main arrows and strength, associated secondary cards and strength,
and literal MUTED badge against independently decoded replay input. Its HTML
review puts the original image, permitted input states, observations and reasons
beside one another. The normal camera bench invokes it after recording. The
standalone analyzer operates on retained recordings without flashing or starting
a capture.

This is a calibrated instrument for the current registered display layout, with
sampled checks and bounded consecutive-frame review. It is useful for finding
and inspecting input/display disagreements;
it is not a general vision model or a whole-firmware certification.

## Normal bench use

```sh
./bench.sh --replay --camera
```

This command builds and flashes the firmware, collects the replay and camera
evidence, and runs the encounter check once. `--all --camera` also collects the
existing core and display legs, with encounter analysis on the replay leg only.
Use `--no-flash` only when the installed image can be independently linked to the
clean source and retained build artifact. Dirty source is rejected before collection.
The bench explicitly resets the connected ESP32-S3 into a fresh normal boot
before collecting a leg, including with `--no-flash`. This supplies the boot
boundary needed to associate display settings with the captured behavior.

The console separates collection/runtime qualification, host packet acceptance,
camera integrity, the narrow counter result and `sampled encounter checks`.
It prints field-check counts, original-frame coverage, the largest unobserved
gap, and a link into `replay/encounter-check/report.html` at the first sample
needing attention. Original images, input expectations, pixel observations and
unknown reasons remain available there. The normal command also selects every
recorded image around input changes and includes event summaries in this report.
Reader progress is printed during this denser analysis and retained in the run log.

| Bench outcome | Exit |
| --- | ---: |
| Hard collection failure | 2 |
| Collection-only evidence, regardless of its separately printed content result | 1 |
| Qualified collection and sampled encounter `PASS` | 0 |
| Qualified collection and sampled encounter `INCONCLUSIVE`, missing camera evidence or incomplete analysis | 1 |
| Qualified collection and sampled encounter `FAIL` | 2 |

A command without `--camera` retains its collection behavior and prints content
`NOT_EVALUATED`. The counter check retains its own result and report; it cannot
raise the encounter result. The former image-change timing summary is no longer
invoked automatically. Its standalone command and historical ledger retain their
original meaning; they do not qualify the latency of correct content. Current
event timing uses the hash-verified camera/input path described below.

## Recheck a recording

Use Python 3.11+, NumPy 2.x, Pillow 10.4–12.x and FFmpeg. Secondary text recognition
uses the local macOS Apple Vision framework and Swift compiler. Other platforms,
or execution environments that block Vision, retain unknown card text. There is
no remote inference or model download. The geometric readers continue to work.

Reader version 4 tolerates ordinary variation in frequency-segment brightness
without using a single saturated stroke as the reference for every digit.
Both brighter and darker lingering strokes can still make a frequency ambiguous.
Main arrows now inspect inset shape interiors as well as the original probes,
and retain each direction's filled, partial, faint or unlit observation. A faint
direction does not hide a clearly filled direction beside it. Color is reported
as an observation; the checker does not establish color correctness.

The secondary reader locates the six meter cells from their shared perimeter
before counting filled interiors. It excludes outlined edges and their camera
fringe, checks interior quadrants and nearby background, and refuses partial,
faint or noncontiguous fills. Registration never scores the expected bar count.
The faint-fill guard has an eight-intensity-level local-contrast floor; it does
not distinguish arbitrarily faint ink from noise. Main strength and text OCR
retain their established methods.

```sh
python3 scripts/bench/encounter_check.py \
  --run-dir path/to/run/replay \
  --out path/to/new-encounter-result
```

Open `report.html` in a local browser. The folder also contains `report.md`,
`result.json`, the frozen selection, original selected PNGs, and retained analysis
source. Each new run requires a new output directory. The original recording and
previous judgments are never replaced.

By default, selection extends from the first to the last replay request. It takes
one midpoint for every unchanged packet state, regular samples every two seconds,
and fixed probes around each packet-state change. Direction, strength and alert
identity changes therefore receive observations even when the counter stays the
same. Selection is complete before pixels are read. Unknown and unfinished
samples stay in the denominator. Duplicate source frames are identified and do
not become independent trials.

Limit the scope with repeated `--range START:END` arguments; offsets are seconds
from the first replay request. `--cadence` controls the regular sample interval.

To inspect transitions automatically, use the same mode as the normal bench:

```sh
python3 scripts/bench/encounter_check.py \
  --run-dir path/to/run/replay --inspect-transitions \
  --out path/to/new-encounter-review
```

This preserves every default held-sample requirement and adds each recorded
frame from 50 ms before through 500 ms after every change in authored packet
bytes. Windows are clipped to the requested range and next input change. The
initial input also receives a window. These bounds are inspection coverage,
never response deadlines. Selection is frozen before decoding; unreadable
frames do not trigger a search for a more convenient answer. The command refuses
more than 20,000 observations before reading pixels; `--range` bounds larger work.
Coverage outside these windows remains sampled.

For a closer look at a transition, select every recorded frame in an explicit
observation window:

```sh
python3 scripts/bench/encounter_check.py \
  --run-dir path/to/run/replay \
  --transition-window 32.95:33.50 --all-frames \
  --out path/to/new-handoff-detail
```

`--all-frames` selects each original video frame whose source capture timestamp
falls inside the declared range, including its start and excluding its end. It
uses source indices and timestamps directly: a nominal 5 ms cadence can miss
closely spaced frames or select another frame twice. Overlapping ranges still
select each source image once. At least one explicit `--range` or
`--transition-window` is required, and selection stops before reading pixels if
it would exceed 5,000 frames. `--cadence` does not apply in this mode. Default
sampling remains unchanged.

The report shows available, selected and read frame counts; actual first and
last timestamps; unrecorded source drops; and the largest gap, including range
boundaries. Complete recorded-frame coverage means every available image in that
range received a reader attempt, including images whose fields remain unreadable.
It does not recover dropped frames or establish what happened between exposures.

Use the frame slider or arrow buttons to step through original images. Slowed
playback uses recorded timestamp spacing and stops at an unobserved source gap;
it does not interpolate frames. Named
changes jump to the first image with that literal reading. Per-field spans group
only adjacent source frames with identical states, values and refusal reasons.
Every one-frame difference or unreadable state remains in the history; gaps break
spans. There is no smoothing, majority vote, value carry-forward or repair using
the expected input. These timestamps describe first and last observed readings,
not a qualified response latency.

A transition window reports current, previous, differing and unreadable content;
it does not turn host acceptance into a device response deadline. Using
`--all-frames --range` treats every selected image as a held observation; use
`--transition-window` for an observational
handoff review without a response deadline.

## Explain each input event

The **What happened** view groups unchanged repeated packets into one input event.
It reports the first original image where all seven fields and their checked
joint state agree, first correct readings by field, earlier differing content,
and subsequent differing or unresolved spans. One-frame changes remain present.
Permitted blink alternatives remain permitted; they are not repaired or smoothed.
Partial arrow/card details remain available even when the combined field is unreadable.

Pending input cannot establish a new completed response. A byte-identical resend
can retain the previously completed input expectation only when all intervening
requests and overlapping transmissions belong to that same complete packet state
and the presentation policy is unchanged. Unscoped traffic, changed packets and
mute's second-display confirmation cannot use this exception. The report retains
the pending transmission and the earlier accepted source; it does not move the
image or claim that the resend has completed. The separate counter checker retains
its existing stricter readiness rule.

Where the contract requires
two accepted displays to confirm mute, the target is not complete before that
second input. A superseding or unscoped table/display request ends the event.
An event with no correctly observed image is explicitly distinguished from a
proved missing alert. The report shows unread recorded images and source drops
throughout each event, including any remainder beyond a dense inspection window.

Timing is **host-send-to-first-correct-capture**, using the completing
notification's attempted/accepted send bounds and the original camera timestamp.
It also reports whether every recorded image from the event request to that
capture was read. With an incomplete prefix, this is only a sampled observation
time. Even a complete prefix cannot rule out earlier correctness during an
unreadable image or between exposures. Exposure integration, DUT receipt and
absolute clock error are unestablished, so physical appearance latency and deadline
compliance remain unqualified. No nominal 200 fps interval or UVC exposure setting
is silently converted into a physical uncertainty bound.

## Interpret the result

The report's **What can be judged** section separates held-observation agreement
from whether each input target appeared in an analyzed image before the event
ended. It links grouped held failures or unknowns directly to their original
images. A target observed once does not establish response speed or continued
correctness; later differing and unresolved spans remain visible and counted.
Missing observations and analysis errors prevent a complete execution claim.

The existing aggregate below remains strict: any unresolved required field or
nonmatching transition observation prevents `PASS`. This includes partial drawing
and preceding states during transitions that have no response deadline. The
separate assessments explain what was established and what blocked judgment;
they do not upgrade that aggregate or establish acceptance of the tool.

| Result | Meaning |
| --- | --- |
| `PASS` | Required sampled fields and their resolved shared blink states agree. |
| `FAIL` | At least one supported held observation disagrees with recorded input. Other unknowns remain visible. |
| `INCONCLUSIVE` | Required evidence is incomplete, unreadable or conditional, or transition observations differ without a response deadline. |

CLI exit codes are 0, 1 and 2 respectively. These are offline analyzer codes;
`bench.sh` converts them to its established 0, 2 and 1 content outcomes while
preserving collection qualification precedence as described above.

A discrepancy does not identify its cause. Host delivery, firmware drawing,
panel response, camera exposure and reader limitations can all matter. A previous
state in an early transition image is an observation, not automatically a bug.
No sampled result proves what happened in unobserved gaps or between exposures.

The reader receives only RGB pixels, dimensions and camera registration. It uses
segment/shape measurements for large display elements and Apple Vision for small
card text. Dark, partial, malformed or inconsistent observations are refused.
Card identities remain associated with their own direction and strength. Shared
counter/band/arrow blink phases are checked together; independently allowed values
cannot silently form an impossible combined display state.

The expectation module supports ordinary nonzero-frequency X/K/Ka input with
one priority and at most two secondary cards. Same-band simultaneous rows within
5 MHz require additional identity/continuity modeling and are refused explicitly.
Laser, Ku, junk/photo rows, ALP interaction and other layouts are outside this
initial scope. Physical audio is not measured by a visible mute badge.

## Configuration and evidence limits

New normal-runtime firmware emits compact configuration snapshots in the recorded
serial evidence. The analyzer uses their boot identity, active slot, configuration
revision and display settings automatically. A stable revision must surround all
selected observations. A change away and back still changes the revision and
cannot masquerade as an unchanged configuration.

Serial receipt alone can include delayed output. Configuration coverage therefore
also requires the recorded reset-to-ready boundary and device uptime, with a
conservative clock allowance of 1% plus 100 ms. Missing reset evidence, invalid
timestamps, a changed boot or insufficient surrounding snapshots leave the
affected settings unknown. Older recordings do not gain verified settings merely
by running the new analyzer.

Missing boot-bound stealth and persistence settings leave affected idle fields
unknown. Retired card content can remain conditional; host elapsed time is not a
device-side expiry clock. Do not fill historical settings using current defaults
or by choosing the setting that agrees with the picture.

`--configuration` accepts independently verified static settings for the selected
window. Its JSON requires `window_result_sha256`, the exact `runtime_identity`,
`status: "verified"`, a nonempty `basis`, and `coverage.start_capture_ns` /
`coverage.end_capture_ns` covering every selected observation. The `settings`
object accepts only `stealthEnabled`, `priorityArrowOnly` and
`alertPersistenceSeconds` (integer 0–5). This binding does not manufacture the
independent readback evidence described by `basis`. Supplied settings cannot
override contradictory or changing recorded configuration.

Camera files, packet files, source timing and the original timing verification are
hash-bound to the recording. The analyzer retains its source and helper/dependency
identity. An interrupted decoder preserves unfinished requirements as unknown
and reports coverage from frames actually reached.

Reader development, independent photographed validation, and synthetic fault
controls establish different things. Keep their denominators separate. Synthetic
missing/wrong content tests validate the instrument; they are not firmware bugs.
An unresolved reference photograph cannot validate an automatic assertion.
