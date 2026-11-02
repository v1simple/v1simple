# Camera registration through the visible-field verdict

[Behavior guide](README.md) · [Display contract](../VALENTINE_PHILOSOPHY.md)

## Contract and repaired failure

The reader must decode the observed display in the camera pose that preflight
admitted. Expected replay text must never enter pixel decoding. A readable wrong
field is a mismatch; an unreadable or unsupported observation cannot establish a
pass. This implements the philosophy's requirement to measure honestly.

Before `daf32ed`, preflight could correctly register a moved display, but the
field reader still used its original camera rectangles. A 60-pixel downward
translation of correct K/front content was read as X/side in the retained
reproduction. The repair maps the registered image into the reader's calibration
coordinates before reading **all** fields, including cards. It closes that
registration-to-reader disconnect.

## Complete path and owners

Paths below are relative to the repository; follow the named symbols rather
than relying on line numbers.

| Stage | Actual owner and handoff |
|---|---|
| Start capture | [`run_window.py`](../../scripts/bench/run_window.py), `collect_live`, calls `run_camera_preflight` before starting replay. This collector can build/upload/reset a board; it is not a read-only verification command. |
| Admit pose | [`camera_preflight.py`](../../scripts/bench/camera_preflight.py), `run_camera_preflight`, starts one capture session and reads its session still. [`camera_registration.py`](../../scripts/bench/camera_registration.py), `calibrate_display_crop` → `detect_display_crop_registration`, decodes the still at 960×540, checks SCAN landmark structure, scale/aspect and crop containment, then publishes `dynamic_similarity.crop_fractions`. Failure aborts capture. |
| Retain evidence | [`camera_capture.py`](../../scripts/bench/camera_capture.py), `stop` and `_verify_video_timing`, finish video and timing verification. `collect_live` records firmware identity, runtime qualification and replay artifacts. [`bench.sh`](../../bench.sh) prints raw `COMPLETE` before visual interpretation. |
| Admit a reading | [`read_replay_fields.py`](../../scripts/bench/read_replay_fields.py), `compare_run`, requires COMPLETE, qualified runtime, CAPTURED camera, verified timing, monotonic written-frame timestamps and PASS registration. A declared capture manifest must match the replay capture, pass file-integrity checks and supply the exact video/timing/preflight paths consumed. Older undeclared captures retain the single-video path. |
| Select a sample | `expectation` supports unmuted K/Ka/X primary radar. `compare_run` requires the same primary expectation in the preceding/current/following stimulus. It selects request time +180 ms using the camera timing sidecar; selection and decoded-frame checks each allow at most 20 ms deviation. This is a stable-state sample, not a latency measurement. |
| Apply geometry | `registered_pixels` requires 1280×720 RGB, PASS `dynamic_similarity`, and four finite crop values with positive dimensions wholly inside the image. Pillow inverse affine mapping with bilinear sampling maps to `READER_REFERENCE_CROP`. The recorded video remains unchanged. |
| Read pixels | `read_pixels` → frequency/counter seven-segment readers, band/direction regions and two card text/direction readers. The function receives only normalized RGB; it never receives the stimulus. |
| Compare and publish | `compare_run` compares decoded values with expectations. `main` writes a separate JSON report; [`bench.sh`](../../bench.sh) keeps it beside raw `replay/` and returns PASS=0, FAIL=1 or INCONCLUSIVE=2. A visual verdict does not rewrite raw collection status. |

For a declared manifest, the reader uses the existing `camera_artifacts.py`
loader, file verifier and path resolver. It validates the manifest schema,
identity digest, replay suite and window capture ID; checks sizes and hashes of
all recorded artifacts; then reads the video, timing and preflight paths named
by that verified manifest. Verifying one file cannot authorize reading a stale
file at the former default path. A missing, malformed or mismatched declaration
cannot silently fall back to legacy reading. These errors become INCONCLUSIVE
with exit status 2 in both direct-script and package-module invocation.

A report's `source.cameraIntegrity` is `verified` for this path and
`legacy-unverified` when the window has no manifest declaration. The latter
preserves older recordings' existing visual checks without claiming their files
passed integrity verification. Hash verification detects changed recorded bytes;
it does not authenticate an actor who rewrites every artifact and its manifest,
or protect concurrent mutation after verification. Capture artifacts are owned
as immutable inputs throughout reading.

Registration estimates translation and uniform scale from the SCAN landmark.
The reader derives X/Y mapping coefficients from that registered crop; this is
not a perspective, rotation or arbitrary camera-layout solver.

The reader also verifies the retained display contract against the window's
recorded size and hash, then verifies and consumes the exact stimulus bytes named
by that contract. A missing contract or changed expectation file cannot receive
a visual pass, even when the camera files remain intact. This prevents a stale
or edited expectation file from being graded under the original contract hash.
It does not add authentication against coordinated rewriting of all metadata.

## What a verdict establishes

- **PASS:** every compared supported sample is readable and matches, with at
  least one match for each primary field. Missing band/arrow pixels may be
  classified `blinkOff` when the stimulus declares blinking.
- **FAIL:** at least one decoded supported field or required card differs.
  A mismatch takes precedence even if other observations are unreadable.
- **INCONCLUSIVE:** prerequisites fail, fields are unreadable without a supported
  blink explanation, or a primary field has no matches. Main converts the
  specified file/decode/value errors to this result.

The card check requires expected text/direction identities to be present with
the needed multiplicity. It does not reject every extra card, prove card order,
or prove absence when no cards are expected. Primary Photo, muted, laser, ALP,
idle, unsupported counters and other excluded states are outside this reader's
claim. Blink-off allowance does not measure blink cadence. One frame per
eligible step does not establish every-frame correctness or packet-free expiry.

## Executable boundary checks

From the repository root:

```sh
V1_BEHAVIOR_PYTHON="$(./scripts/bench_python.sh --visual)"
PYTHONPATH=scripts "$V1_BEHAVIOR_PYTHON" -m unittest scripts.test_read_replay_fields
"$V1_BEHAVIOR_PYTHON" scripts/test_camera_preflight.py
```

The helper prepares the pinned local Python dependencies if absent. These test
commands use fixtures and do not start a physical capture.

[`test_read_replay_fields.py`](../../scripts/test_read_replay_fields.py):

| Regression | Boundary exercised / unchanged control |
|---|---|
| `test_comparison_reads_registered_translation_and_scale` | Real `compare_run` → `registered_pixels` → all pixel readers → verdict. Baseline, +60 Y, −80 X/−60 Y and 0.8 scale/+80 X/+80 Y retain correct primary fields and an MRCT/SIDE card. |
| `test_registered_comparison_still_detects_wrong_visible_band` | A shifted image visibly containing X while replay requests K still returns FAIL with the observed X. Normalization cannot repair an actual content mismatch. |
| `test_comparison_requires_usable_registration_transform` | Missing, zero-size, out-of-frame and NaN crop data cannot receive a reading. |
| `test_incomplete_raw_capture_cannot_receive_visual_pass` | Incomplete collection is refused before pixel comparison. |
| `test_post_capture_report_has_distinct_exit_statuses` | Report result maps to the external command's three distinct statuses. |
| `test_manifest_integrity_preserves_valid_and_wrong_visible_fields` | Intact declared capture still passes, wrong visible content still fails, and legacy reports explicitly remain unverified. |
| `test_declared_capture_rejects_corrupt_or_missing_files_before_decoding` | Same-size timing/video/preflight/still changes and missing files are rejected before frame decoding. |
| `test_declared_manifest_cannot_silently_become_legacy` | Missing or malformed declarations cannot bypass integrity checks. |
| `test_manifest_identity_schema_and_replay_suite_are_required` | Window identity, manifest digest/schema and replay suite must agree. |
| `test_reader_consumes_verified_manifest_paths_instead_of_default_paths` | Renamed verified inputs are read despite poisoned files at the old paths. |
| `test_replay_contract_and_stimulus_changes_cannot_receive_a_visual_pass` | Missing or changed expectations and contract files are rejected before decoding. |
| `test_replay_reader_uses_verified_stimulus_path_and_rejects_missing_contract` | The retained expectation path is consumed; absent contract ownership cannot become a legacy pass. |
| `test_integrity_errors_are_inconclusive_in_script_and_package_commands` | Fresh processes exercise both invocation modes and turn integrity errors into an INCONCLUSIVE report/exit 2. |

The comparison fixture substitutes only `frame_at` inside the comparison path;
it fabricates capture metadata, timing and expectations and supplies procedural
pixels. Primary segment drawing is independent of the decoder. Card glyphs use
the reader's templates, so these fixtures alone do not independently qualify
the real rendered font. They do not exercise camera hardware or video decoding.

[`test_camera_preflight.py`](../../scripts/test_camera_preflight.py) separately
checks the real registration detector on synthetic SCAN images, including
translation/scale, unreadable landmarks and refused crops. Admission/lifecycle
tests substitute the camera and decoder; `test_collect_refusal_never_opens_product_path_and_pass_continues_once`
checks ordering with substituted product operations. Run the file directly;
these are functions invoked by `main`, not unittest discovery cases.

During the repair, bypassing `registered_pixels` made the three moved-pose
comparison subcases fail (FAIL, FAIL, INCONCLUSIVE); the baseline still passed.
The corrected reader also passed transformed retained camera pixels. Those are
specific pose controls, not proof across every admitted optical condition.

To exercise actual video decoding without touching hardware, run against an
existing complete recording and keep the new report outside its raw directory:

```sh
"$V1_BEHAVIOR_PYTHON" scripts/bench/read_replay_fields.py "$REPLAY_DIR" \
  --output "$REPORT_PATH" --progress
```

Set both paths to local retained evidence. `--limit N` is useful while diagnosing
one boundary, but a limited report qualifies only those N eligible samples.
Keep private recordings, paths and device metadata out of public changes.

## Future changes

Start with the matching failing observation and these owner functions. For a
geometry change, retain an unchanged-pose control, a moved correct image and a
moved wrong-content image. For a rendering/font change, use original camera
pixels as well as synthetic reader tests. Report the decoded value, frame time,
expected value and exact supported scope; an unreadable frame is not a proven
firmware defect. Update the scope above if the reader gains a new field or mode.
