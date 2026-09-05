# Visible encounter bench product

The visible encounter check is an external optical test of the V1 display. It
uses the replay input record as the requested state, reads the recorded screen,
and judges whether each supported display event appeared on time and remained
correct through a complete blink cycle. The reader is outside the firmware and
receives only the registered RGB image. It does not receive the expected value,
packet bytes, or timestamps while reading pixels.

The primary bench answer is the qualified `VISIBLE_EVENT_PRESENTATION/v2`
verdict. A raw frame result remains in the report for diagnosis, but it does not
control the product verdict. `INCONCLUSIVE` at the product layer is a failure of
the testing product to answer the question. Repair the reader, qualification,
capture, configuration evidence, or unsupported scope before treating that run
as useful product evidence.

This product currently covers normal-runtime V1 X, K, and Ka presentation with
exactly one primary alert whenever alerts are live and at most two associated
secondary cards. Same-band alert frequencies must be more than 5 MHz apart. It
checks these seven visible fields and their legal shared blink states:

- counter or mode glyph;
- primary frequency;
- active band labels;
- main direction arrows;
- main strength bars;
- secondary card identity, frequency, direction, and strength;
- literal `MUTED` badge.

It does not judge audio, arrow color correctness, ALP interaction, laser, Ku,
junk/photo rows, or layouts outside the registered display. Those events remain
unsupported rather than receiving a convenient answer.

## Run the product

The normal replay and camera bench is:

```sh
./bench.sh --replay --camera
```

This builds and flashes the firmware, resets into a fresh normal boot, records
the replay and camera streams, and runs the product check once. The worktree
must be clean so the source, binary, boot, and retained evidence have one exact
identity. Use `--no-flash` only when the installed image can be independently
linked to that clean source and retained build artifact.

`./bench.sh --all --camera` also runs the core and display collection legs. The
visible encounter product is evaluated on the replay leg.

To collect a new reserved qualification run without exposing its pixels to any
reader, first freeze the candidate classifier, selection rule, observer rubric,
and implementation hashes, then run:

```sh
./bench.sh --replay --camera --qualification-capture
```

This mode performs the same clean-source, runtime, replay-delivery, and camera
integrity checks, but it does not invoke either the sampled-counter reader or the
visible-encounter reader. It retains
`replay/qualification_capture.json`, binding the source commit, bench-script
bytes, collection result, and camera evidence while recording that both pixel
analyzers were disabled and their output directories were absent. A successful
run ends `QUALIFICATION-CAPTURED`; this is evidence ready for blind labeling,
not a product `PASS`. A missing camera is a hard qualification-capture failure.
The separation prevents classifier choices from being adjusted after seeing the
reserved pixels, which is the value of this distinct mode.

The reader qualification manifest defaults to:

```text
.artifacts/bench/qualification/encounter-reader.json
```

Set `BENCH_ENCOUNTER_QUALIFICATION` to use another retained manifest. A missing,
stale, or rejected manifest makes the primary product verdict `INCONCLUSIVE`.
The report may show an unqualified candidate judgment to help repair the tool,
but that candidate cannot produce the bench result.

The console reports collection/runtime qualification, host packet acceptance,
camera integrity, the visible-event verdict and event denominator, original
frame coverage, the largest unobserved analysis gap, and a link to
`replay/encounter-check/report.html`. The sampled counter result is printed
separately and cannot raise or lower the visible-event verdict.

| Bench outcome | Exit |
| --- | ---: |
| Qualified collection and visible encounter `PASS` | 0 |
| Reserved `QUALIFICATION-CAPTURED` with pixels withheld | 0 |
| Collection-only evidence | 1 |
| Requested camera absent during preflight | 1 |
| Visible encounter `INCONCLUSIVE` | 1 |
| Visible encounter `FAIL` | 2 |
| Camera capture failure during a leg | 2 |
| Other hard collection, source identity, emulator, or semantic failure | 2 |

A command without `--camera` does not evaluate visible content. It retains the
existing collection result and prints the encounter product as `NOT_EVALUATED`.

## Exact visible-event contract

The tracked policy profile is `v1-normal-x-k-ka-blink96-v2`. Timing values are
fixed by that profile; the command line cannot loosen them.

| Contract point | Exact value |
| --- | --- |
| Clock | host monotonic capture marker |
| Event anchor | first complete target input `allAcceptedNs` |
| Nominal appearance deadline | anchor + 100 ms |
| Observable deadline decision | first source marker at or after the nominal deadline |
| Maximum deadline-marker bracket | 10 ms |
| Verification duration | 192 ms from the first correct frame |
| Maximum accepted source-marker gap | 10 ms |
| Required event hold | 312 ms after the anchor |
| Selected event window | `[anchor - 10 ms, min(anchor + 312 ms, actual event end))` |

The 100 ms appearance bound is two declared 50 ms display-update intervals. The
192 ms verification interval covers two 96 ms image phases. The 10 ms source
bound is two periods of the fixed 200 fps camera profile. Because the camera is
a discrete observer, version 2 gives the deadline exactly one observation
opportunity: when no earlier frame is fully current, the first recorded source
marker at or after 100 ms is decisive. Its gap from the immediately preceding
marker must be no more than 10 ms. The required 312 ms hold is the nominal
appearance bound, one deadline-observation bracket, the full-cycle verification
interval, and one closing source guard combined.

An input event begins with the initial authored packet set or with a change from
the preceding set. Byte-identical repeats remain in that event, and a target is
not complete until its full alert table and display packet have been accepted.
Stateful mute presentation requires two
consecutive accepted mute displays, so its anchor is the first complete input
that establishes the final muted target. A changed authored packet set or an
unscoped alert-table/display request ends the current event.

Selection is frozen from input timestamps before any product-window pixels are
read. Every written source frame in the union of the exact event windows must be
selected and read. The adapter rejects missing or extra event-window originals.
It also checks source-frame sequence, video index, capture timestamp, boundary
coverage, recorded drops, and actual marker spacing. A superseding input clips
the event at its real end. A clipped event cannot pass: it is `INCONCLUSIVE`
unless complete readable evidence separately proves a definite visible
violation, which remains `FAIL`.

For a changed event, the last source frame before the anchor must show the
previous target within 10 ms. For an unchanged resend, it must show the current
target. A fully current marker at or before 100 ms establishes acquisition
directly. Otherwise, the first source marker at or after 100 ms must be fully
current and its bracket with the preceding marker must be no more than 10 ms.
An unresolved first post-deadline marker is `INCONCLUSIVE`; a previous or
definitely other state is `FAIL`. A later marker never replaces that sole
deadline observation. From the accepted first correct frame, the reader must
observe 192 ms of continued legal presentation, including every required shared
blink phase and a closing correct frame no more than 10 ms after verification.

The baseline event has no qualified previous target. Its non-current or
unresolved frames before acquisition remain in the raw evidence but do not make
the later verified target incoherent. The first deadline observation still has
the strict outcomes above. After the first current marker, an unqualified
unknown blocks verification and a definite non-current state fails. Changed
events retain strict mixed-state and prior-state checks throughout acquisition.

A definite unexpected state, impossible combination, previous state after the
deadline, or regression after correctness is a product failure. A target never
seen by the deadline is a failure when complete readable evidence makes that
absence decidable. Missing frames, reader refusals, unqualified transitions, an
unsupported target, incomplete timing/configuration identity, or an incomplete
verification interval produce `INCONCLUSIVE`.

These timestamps bind accepted host input and camera capture markers. They do
not measure DUT receipt, exposure integration, panel scanout, or the instant a
human first sees a pixel. A version 2 event that first becomes fully current on
the post-deadline observation reports both source markers and their interval-
censored bracket. Its `PASS` means the first observable deadline marker was
current within the declared source bound; it does not prove physical appearance
by exactly 100 ms.

## Reader V5

Reader V5 is a fixed-layout instrument calibrated from the registered `SCAN`
landmark. It first requires two lit display witnesses so a dark or occluded
screen cannot be interpreted as valid absence. It records literal states such
as `readable`, `absent`, `ambiguous`, and `unreadable`; a refusal is never
converted to absence.

The counter, five-digit primary frequency, band labels, main bars, arrows, and
mute badge use fixed geometric measurements. Frequency digits require canonical
seven-segment masks, a visible decimal, clear glyph interiors, and consistent
illuminated strokes within each digit. Main and secondary strength meters
require contiguous filled cells and reject partial strokes. Arrow decisions use
fixed probes plus each glyph interior; all three directions are measured before
the combined field is resolved. The reader retains a 4-by-4 arrow intensity
profile for narrow sequence classification, while the single-frame decision
continues to come from the literal probes and shape interior.

Secondary cards keep slot, text, direction, and strength associated in one
record. Their six-cell meters are located by a shared perimeter grid without
scoring any expected fill count. Cell interiors, quadrants, nearby background,
and contiguous fill order decide the bar count. Partial cells retain every
compatible count for possible sequence-level corroboration, while the raw frame
remains unresolved. A local contrast difference below eight intensity levels is
outside the meter's stated detection floor.

Only the small secondary-card text uses local Apple Vision OCR. Accepted text
must be visible and have at least 0.5 confidence. After removing whitespace and
normalizing visually identical Latin/Cyrillic band letters, it must exactly
match Ka, K, Ku, X, or L followed by two digits, a literal decimal, and three
digits. Product scope remains X, K, and Ka. The reader never inserts digits or
infers a missing decimal. No remote model, network request, or expected display
value participates in recognition. Because secondary text is a required field,
a decisive product result currently requires a local macOS Apple Vision and
Swift environment that passes the operational probe below.

## Qualification is part of the instrument

A decisive product result requires the exact running reader to match one
retained qualification bundle. The verifier resolves each referenced source
inside the bundle, verifies the actual file bytes against the declared SHA-256,
and independently rederives the retained field statuses, fault-control
comparisons, temporal decision lists, matrices, denominators, rates, minima,
and final allowlist decision from their item records. It also requires the
retained blindness and integrity audits to report success. A missing file, path
outside the bundle, hash mismatch, altered subtotal, or inconsistent derived
decision rejects qualification. The bundle must establish all of the following:

- exact hashes for `encounter_reader.py`, `encounter_ocr.swift`,
  `counter_reader.py`, `encounter_runtime_probe.py`, and
  `encounter_ocr_probe.b64`, as well as `encounter_qualification.py`,
  `encounter_expectation.py`, and every temporal implementation named by the
  active policy;
- exact Reader V5 runtime identity, including NumPy and Pillow versions, OCR
  source and compiled-binary identity;
- an operational OCR probe, not compilation alone: the compiled helper must
  read the fixed representative probe as `K24.150` at confidence 0.5 or higher;
- the exact camera name and full camera profile used by the run;
- at least 20 unique independently labeled original frames, every one labeling
  all seven fields, with at least 10 agreements per field, zero wrong reader
  assertions, and zero assertions against an unresolved reference; the complete
  reserved manifest, completed blind observations, selection, and every image
  are retained and byte-bound, and the verifier reruns the exact reader on every
  RGB image with the retained passing registration before rederiving the result;
- a separately blinded visible-secondary set with at least five unique,
  nonempty card displays that agree with the reader, zero wrong assertions, and
  retained byte-bound packet manifest, completed blind labels, sealed key,
  adjudication record, and source images;
- all ten required product fault controls; and
- exactly the temporal classifiers listed by the active visible-event policy,
  with no omitted or extra classifier.

The OCR probe proves that the local framework can execute a representative
request in this environment. It is not a DUT observation and is never used to
classify a bench frame.

The gate can verify retained bytes and rerun deterministic reader and comparison
code. It cannot reconstruct what a human observer was shown. The statements
that labels were completed before key access and that the observer did not
receive machine output or the hidden key remain provenance declarations;
qualification requires those declarations to state the required separation.

The ten fault controls prove that the comparator distinguishes an unchanged
source image; missing primary content; wrong main strength; wrong direction; a
stale primary frequency; a missing secondary card; wrong card association; an
unreadable camera image; a partial primary frequency; and a definite failure
that also contains an unknown arrow field. Each case retains a unique byte-bound
image, the common expected state, and the observed state. The verifier reruns
the exact reader on every RGB image with the retained passing registration,
requires every field's literal state and value to match the recorded
observation, reruns the product comparator, and requires an exact match with the
retained comparison. Each comparison checks all seven fields. The two refusal
cases must remain `INCONCLUSIVE`; the changed-content cases must name their
required differences. The mixed case must simultaneously retain the required
frequency and bar differences and unresolved arrows, proving that a known
failure is not erased by a second unknown field.

Qualification is exact rather than hereditary. A reader, OCR helper,
qualification verifier, comparator, or classifier edit; a runtime/library,
camera profile, temporal allowlist, or specification change; or a mismatched
evidence hash requires a matching qualification bundle before the product can
return `PASS` or `FAIL`.

## Temporal classifiers

Raw frame evidence is immutable. A temporal classifier may qualify only an
explicit contiguous run of raw unresolved frames, only for its declared fields,
and only when its exact ID and specification hash appear in the active policy.
The source frames must belong to that exact event. If multiple classifiers apply
to one frame, their field claims must be disjoint and together cover every raw
affected field. Duplicate, partial, extra, malformed, or off-event claims are
rejected and prevent `PASS`; a separately proved event failure still controls
the overall result.

The active policy currently allows only `v1-arrow-phase-edge-v2`. It accepts a
maximal ambiguous arrow run only when two readable source-consecutive support
frames exist on each side, the endpoints are the two permitted blink phases,
and exactly one direction changes. Endpoint separation must be at least 52 RMS
levels. Each ambiguous 4-by-4 profile for the changing direction must project
between -0.05 and 1.05 of the endpoint path with normalized residual no greater
than 0.15. A backward step may not exceed 0.05, total backward motion may not
exceed 0.10, and every unchanged direction must stay within an 8 RMS profile
diameter. The endpoint span may not exceed one 96 ms blink phase plus the
verified source interval. Any failed bound rejects the run. The unresolved raw
frames remain visible in the report.

Every policy-listed classifier must also carry a byte-bound blind validation.
The verifier checks the pre-pixel freeze, frozen classifier output, completed
observer labels, observer manifest and instructions, restricted hidden key,
seal, selection, and every referenced clip. It derives each true-admit,
false-admit, true-reject, and false-reject classification from the observer's
literal labels and frozen classifier decision, then rebuilds the decision lists,
confusion matrix, denominators, rates, numerical minima, and allowlist decision.
Qualification requires zero false admissions, at least five true admissions,
at least five true rejections, and an integrity pass. A supplied named-sentinel
audit must have zero violations.

The verifier independently checks each retained clip's bytes and size. The
recorded checks that its path matches the opaque ID, its source-frame count
matches the declared range, and its target run lies inside the clip remain
retained audit declarations and must all report success.

The secondary-card corroboration classifier remains diagnostic and is not
listed by the policy. Its output therefore cannot change a visible-event verdict.

## Product result and raw evidence

| Product result | Meaning |
| --- | --- |
| `PASS` | The reader is qualified, evidence integrity is complete, and every required supported event was current by its nominal deadline marker or the sole bounded observation immediately after it, then completed verification. |
| `FAIL` | The qualified evidence proves at least one supported event violated the contract. Other event unknowns remain visible. |
| `INCONCLUSIVE` | The testing product did not produce a defensible answer. This is a tool, evidence, scope, or qualification failure that requires repair. |

One proved event failure makes the overall product `FAIL`, even when another
event contains a local unknown. Unknown evidence cannot erase an observed
violation. Otherwise every required event must pass. Fatal identity, hash,
configuration, timing, qualification, adapter, decoder, or classifier-execution
errors prevent every decisive product claim and produce `INCONCLUSIVE`.

`raw_frame_result` is a separate strict diagnostic aggregate. It compares each
selected frame directly with its contemporaneous input expectation. A definite
difference in a held observation is raw `FAIL`. During a transition observation,
definite content is labeled as the previous input state or another transition
difference; it prevents raw `PASS` without creating a response-deadline failure.
Any other required nonmatch, refusal, conditional expectation, analysis error,
or incomplete decode also prevents raw `PASS`. Early transition frames can
therefore make the raw result inconclusive even when the qualified product
contract passes. The report retains those frames instead of hiding them.

The standalone analyzer exits 0 for `PASS`, 1 for `FAIL`, and 2 for
`INCONCLUSIVE`. `bench.sh` maps the product result to its bench meanings shown
above while preserving collection precedence.

## Recheck an immutable recording

Reanalysis never flashes hardware or starts a new capture. Use a new output
directory and supply the exact qualification manifest:

```sh
python3 scripts/bench/encounter_check.py \
  --run-dir path/to/run/replay \
  --inspect-transitions \
  --reader-qualification path/to/encounter-reader.json \
  --out path/to/new-encounter-review
```

The folder contains `result.json`, `report.md`, `report.html`, the frozen
selection, original selected PNGs, and retained analysis identity. An existing
output directory is rejected, so an earlier judgment cannot be overwritten.

`--inspect-transitions` retains the ordinary input-selected checkpoints, adds
every recorded frame from 50 ms before through 500 ms after authored packet
changes, adds fixed context around held checkpoints, and adds the exact product
event-window union. The 50/500 ms inspection bounds are diagnostic coverage;
only the tracked product window and deadline above carry product meaning.
Without an explicit range, analysis uses the entire recorded camera interval,
including the pre-roll before the first request and the tail after the final
request, so the first and final input events can retain their complete windows.
Selection stops before pixel reading if the input-selected checkpoint set would
exceed 5,000 observations or the combined dense review would exceed 20,000.
Use `--range START:END` to bound a larger recording; a product event is included
only when the range contains its complete available event window.
Explicit range values are nonnegative seconds from the first replay request;
only the automatic full-camera range can include recorded pre-roll at a negative
offset.

Without `--inspect-transitions`, the command returns the raw sampled result. It
selects a midpoint from every unchanged packet state, regular samples every two
seconds, and fixed probes at -50, +50, +150, and +350 ms around packet changes.
`--cadence` changes the regular interval. Selection uses literal packet bytes,
not the expected display or decoded pixels.

For a bounded consecutive-frame diagnostic, use an explicit range:

```sh
python3 scripts/bench/encounter_check.py \
  --run-dir path/to/run/replay \
  --transition-window 32.95:33.50 \
  --all-frames \
  --out path/to/new-frame-review
```

`--all-frames` uses every original video frame whose capture marker falls inside
the half-open range. It requires an explicit `--range` or
`--transition-window`, ignores cadence, de-duplicates overlapping ranges, and
stops before reading pixels above 5,000 frames. A `--transition-window` remains
diagnostic and imposes no response deadline.

The HTML report leads with the primary product verdict, qualification status,
event counts, and first reason needing attention. It then exposes the raw result,
each original image, expected states, literal reader output, comparisons, source
coverage, state spans, temporal-classification counts and errors, and the
unqualified candidate. Full accepted and rejected temporal records remain in
`result.json`.
The frame history does not smooth, vote, carry values forward, interpolate, or
retry after seeing pixels. Duplicate references to one source frame remain one
visual observation.

## Configuration and evidence identity

The analyzer hash-binds the recording's window result, replay scenario,
stimulus, delivery record, capture manifest, camera artifacts, source timing,
encoded-video timing verification, and runtime identity. Camera registration
must have passed. Every written frame must have one matching encoded frame and
verified duration/timestamp; timestamp errors, missing encoded frames, extra
encoded frames, and duration mismatches must all be zero.

Normal-runtime firmware records compact configuration snapshots. The analyzer
requires one stable boot, active slot, configuration revision, and covered
display settings. A setting that changes away and back still has a new revision.
Receipt of a serial line alone is insufficient: coverage also uses the recorded
reset-to-ready boundary, device uptime, and a conservative clock allowance.
Missing or contradictory settings remain unknown and can make a target
unsupported.

For an older recording or a raw diagnostic, `--configuration` may bind
independently verified static settings to that exact window. Its JSON must name
the exact `window_result_sha256` and `runtime_identity`, have
`status: "verified"`, state a nonempty evidence basis, and cover every selected
capture timestamp. Encounter expectations use `stealthEnabled`,
`priorityArrowOnly`, and `alertPersistenceSeconds`. The analyzer requires a
nonempty subset of those names and rejects extra names. The first two values
must be Boolean; persistence must be an integer from 0 through 5. A
visible-event product verdict additionally requires recorded normal-runtime
snapshots to surround the extended exact-window selection. A static file cannot
manufacture that recorded coverage or override contradictory recorded
configuration. Automatic camera pre-roll and recording-tail diagnostics do not
widen this product gate: the analyzer first brackets the authored input episode
from 10 ms before its first request through 312 ms after its last request, then
rechecks recorded configuration against only the frames in the actual exact
event windows.

A discrepancy locates the failed evidence boundary, not its cause. Host
delivery, firmware rendering, panel behavior, camera capture, and reader behavior
remain separate layers in `result.json`. The visible-event verdict establishes
only the supported contract and exact evidence identity printed in that result.
