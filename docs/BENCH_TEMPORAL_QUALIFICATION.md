# Blind temporal reader qualification

This workflow turns one clean `--qualification-capture` into blind schema-v2
evidence for the main-bar redraw, muted-badge fill, and stable-frequency sweep
classifiers. It publishes those classifier versions only when all three pass the
code-owned rubric with zero false admissions, at least five true admissions,
and at least five definite true rejections each. The existing field, secondary-card,
fault-control, and arrow qualifications are resolved from the current manifest
and reused after their references and hashes are verified; no dated evidence
path or provenance JSON is entered by hand.

Run every command from the repository root.

Run `freeze`, `prepare`, and `finalize` from a native macOS shell with Apple
Vision access. A restricted or sandboxed shell can compile the retained OCR
binary yet receive `nilError` from Vision. The workflow treats that as an OCR
runtime-probe failure and stops; it does not substitute another reader or
weaken the check.

## 1. Freeze the clean implementation

Commit the implementation and test changes first. Both `freeze` and the bench
refuse a dirty tree. Then create a new ignored campaign directory:

```sh
ARTIFACT_ROOT="${BENCH_ARTIFACT_ROOT:-.artifacts/bench}"
CAMPAIGN="$ARTIFACT_ROOT/qualification/temporal-v2-$(git rev-parse --short HEAD)"
python3 scripts/bench/encounter_qualification_workflow.py freeze --out "$CAMPAIGN"
```

`freeze` compiles and probes the OCR helper, records the exact reader,
classifier, specification, bench, camera, and observer-rubric identities, and
confirms that none of the three candidate classifiers is allowlisted. It does
not open a camera recording.

## 2. Make one reserved camera capture

With the Global Shutter Camera and target connected, run:

```sh
./bench.sh --replay --camera --qualification-capture
```

The successful terminal result is
`QUALIFICATION-CAPTURED (pixels withheld; visible product NOT_EVALUATED)`.
Keep the exact run path printed after `evidence:`. The input to the next command
is that run's `replay` directory, which contains `qualification_capture.json`
and `window_result.json`.

## 3. Build the blind observer packets

```sh
python3 scripts/bench/encounter_qualification_workflow.py prepare \
  --campaign "$CAMPAIGN" \
  --run-dir "/absolute/path/printed/by/bench/replay"
```

`prepare` authenticates the clean capture, retains a hash-inventoried replay
tree (capture manifest, exact video, source-frame timing and replay inputs),
runs the frozen analyzer once, keeps the analyzer result and pre-pixel
`analysis-selection.json`, and includes every admitted and rejected candidate.
It makes one observer packet per classifier at:

```text
$CAMPAIGN/prepared/classifiers/<classifier-id>/source/observer_packet/
```

Give the observer only those three `observer_packet` directories. Each contains
`README.txt`, `manifest.json`, `observations.json`, and opaque slow-playback
clips. It contains no machine decision, rejection reason, hidden key, expected
literal, packet value, or comparison result. The sibling `restricted`
directory must remain unavailable to the observer.

For every item, the observer frame-steps the clip using the manifest mappings:

- `clip_source_video_indices[N]` is the reserved-run video index at zero-based
  clip frame `N`. The clip's full pane is lossless and is verified pixel for
  pixel against those frames in the retained source video.
- `full_run_clip_frame_indices` gives the complete optical transition and maps
  exactly to `full_run_video_indices`.
- `target_run_clip_frame_indices` maps the classifier-claimed subset to
  `target_run_video_indices`. For a frequency sweep, this subset can start
  after the full optical run begins. A frame inside the full run is never
  endpoint support merely because it lies outside the claimed subset.
- The frames outside the full run provide endpoint context. The observer follows the
  exact allowed literals and eligibility rules in `README.txt` and replaces
  each template `null` with an allowed literal or value. A dynamic endpoint may
  remain JSON `null` only where that field's rubric explicitly allows it. After
  every clip is complete and before seeing any hidden material,
  the observer sets the three `blind_protocol` values to `true`, `false`, and
  `false` exactly as `README.txt` directs. The item order and opaque IDs stay
  unchanged; the observer does not attest to that protocol if it is untrue.

The comparison derives three ground-truth states from each completed literal.
`ELIGIBLE` is a fully supported positive. `DEFINITE_NEGATIVE` requires clear
support on both endpoints, HIGH confidence, and a fully resolved visual reason
that violates the admission rule. Anything unresolved is `INDETERMINATE`.
A machine rejection of an indeterminate clip is `ABSTAIN`: it is retained in
the bundle but cannot satisfy the five-negative minimum or enter scored
accuracy and specificity. A machine admission of either a definite negative
or an indeterminate clip remains a false admission and fails qualification.

## 4. Finalize and publish only a passing bundle

After all three observation files are complete, run:

```sh
python3 scripts/bench/encounter_qualification_workflow.py finalize \
  --campaign "$CAMPAIGN"
```

`finalize` first validates all observation documents, lossless clip pixels,
retained capture files, sidecar frame identities, current code, and carried
static/arrow evidence. It repeats the analyzer once from the retained replay
inputs and requires the same selection and temporal output. Normal bench runs
use the resulting hash and invariant checks; they do not repeat this expensive
qualification analysis.

Only after those checks pass does `finalize` durably mark the campaign
`CONSUMED` and open the restricted analyzer result and hidden key. It then
derives all comparisons, denominators, rates, clip audits, and allowlist
decisions in memory and publishes the three comparison bundles as one atomic
directory. The carried arrow evidence is accepted only while the exact
`encounter_arrow_transition.py` dependency remains unchanged; a regression
proves the temporal wrapper forwards arrow records without mutation.

The normal qualification verifier checks the complete prospective manifest. Only a
`QUALIFIED` result publishes both
`scripts/bench/visible_event_policies.json` and
`.artifacts/bench/qualification/encounter-reader.json`. A write or final
verification failure restores both prior files. Publication installs the
manifest first so an interruption remains fail closed under the old policy. A
durable transaction marker lets the same `finalize` command complete an
interrupted publish idempotently.

Any malformed, missing, changed, under-minimum, or false-admission evidence
stops here without allowlisting the classifiers. A failure before `CONSUMED`
can be corrected without revealing the key. Any failure after `CONSUMED`,
including a failed minimum or failed publish recovery, requires a new freeze,
reserved capture, and blind observation campaign. Generated provenance is not
edited to rescue a consumed campaign.

The default manifest follows the bench environment: `BENCH_ARTIFACT_ROOT`
changes the artifact root, and `BENCH_ENCOUNTER_QUALIFICATION` overrides the
manifest path directly. `--base-manifest` and `--manifest` remain explicit CLI
overrides.

## 5. Commit the qualified policy, then run the product

Review the reported matrices and the policy diff. Commit the qualified policy
so the decisive bench run starts from a clean tree and has one exact source
hash. Then run the ordinary product path:

```sh
./bench.sh --replay --camera
```

The product outcome is the final `PASS`, `FAIL`, or `INCONCLUSIVE` visible
encounter result. The qualification campaign proves the three exact reader
classifier versions; the normal run still has to establish collection,
runtime, camera, and visible-event behavior for that run.
