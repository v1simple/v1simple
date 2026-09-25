# Behavior traces for maintenance

These notes explain the five failures repaired in `daf32ed`: how input reaches
its final consumer, what the repair changes, and the exact limits of the tests.
Use them to give a bounded agent enough context to handle one behavior without
rediscovering the repository. They are source navigation and executable
contracts; source and observed behavior remain authoritative.

Read [AGENTS.md](../../AGENTS.md) and
[Valentine's philosophy](../VALENTINE_PHILOSOPHY.md) first. This guide covers
these five paths, not every feature or operating condition in the product.

## Choose the behavior

| Symptom or change | Trace to read | What the fix closes |
|---|---|---|
| USB profile replacement or an unrelated slot edit changes volume/dark-mode overrides | [USB profiles](USB_PROFILES.md) | v4 carries the complete slot modifier state through export, validation, replacement and modeled NVS reload. Legacy imports have explicit defaults. |
| Older Gen2 detailed alert rows disappear, including before version discovery | [V1 protocol: detailed alerts](V1_PROTOCOL.md) | Documented D6 and D8 alert destinations reach the alert table; origin, checksum and payload validation remain enforced. |
| Custom-sweep apply times out even though a reply arrived during the send | [V1 protocol: AutoPush](V1_PROTOCOL.md) | The three refresh requests and final commit use a boundary captured before sending; older queued replies remain ineligible. |
| A new unidentified ALP engagement keeps the preceding gun name | [ALP sessions](ALP_SESSIONS.md) | The final display setter retains a known gun only within the same nonzero session generation. |
| Correct visible fields fail after an admitted camera translation/scale | [Camera fields](CAMERA_FIELDS.md) | Every pixel reader consumes the registered pose; genuinely wrong content still fails. |

Each trace follows the entrypoint, admission rules, state owner, downstream
consumer, terminal outcome and alternate paths that can overwrite the result.
Its regression section distinguishes production code from test substitutes.

## Where the proof ends

A passing layer establishes that layer's stated behavior:

| Evidence | Establishes | Requires separate evidence |
|---|---|---|
| Real parser/queue with injected bytes | Admission, ordering and decoded state under those inputs | Physical BLE callback scheduling and detector-version interoperability |
| Real settings import/reload with mocked storage | Schema rules, serialized state and modeled transaction/reload behavior | Physical USB transfer, flash durability and post-reboot device readback |
| Real AutoPush owner with injected transport replies | Request ordering, freshness, verification and terminal owner state | Actual detector writes and outer durable-operation recapture |
| ALP runtime, pipeline and real display setter | Session identity, composed owner, retained text state and expiry | Physical UART reception, panel pixels and speaker behavior |
| Procedural camera pixels | Reader geometry and mismatch controls | Actual optics, rendered font, acquisition and video decoding |
| Retained original video plus timing and stimulus witnesses | The observed fields and transitions in that recording | Other modes, omitted stimuli, RF performance or vehicle environments |

For example, a replay with zero persistence and continuing idle packets cannot
qualify a nonzero hold that must expire without another packet. A replay that
contains no ALP events cannot qualify the ALP repair. Storage readback does not
establish that a detector applied the stored settings.

## Focused verification

From the repository root, serialize PlatformIO invocations:

```sh
python3 scripts/run_native_tests_serial.py --env native-sanitized \
  test_usb_profile_document test_usb_profile_protocol \
  test_ble_queue_alert_integration test_auto_push_module test_alp_display_session
python3 scripts/test_usb_profiles.py
V1_BEHAVIOR_PYTHON="$(./scripts/bench_python.sh --visual)"
PYTHONPATH=scripts "$V1_BEHAVIOR_PYTHON" -m unittest scripts.test_read_replay_fields
"$V1_BEHAVIOR_PYTHON" scripts/test_camera_preflight.py
```

These commands exercise host fixtures. They do not flash or run a physical
bench. Choose the relevant subset for a later small change; the individual
traces name the exact regressions. Keep full release qualification for release
work. A tool that fails before running cases supplies no product result.

### Verification while writing this guide

On 2026-09-25, with production code at `daf32ed` and the two test improvements
described in the traces:

- Five native suites passed **164 cases**: USB document 32, USB protocol 21,
  alert queue/parser 6, AutoPush 102, and ALP composition 3. Changed fixtures were
  rerun after their edits.
- USB host tests passed **26 cases**; pixel-reader tests passed **9 cases**;
  camera preflight's direct runner passed its **10 test functions**.
- The six-case alert suite failed three D6 cases with only the pre-fix parser
  substituted in an isolated copy, including the new normal B4E0 input path.
- A fresh read of five eligible frames from a retained original video matched
  all **20 primary fields**. Cards were not applicable in those samples. This
  checked actual decoding, not a new capture or a complete replay review.

These are dated results, not substitutes for verifying a later source change.

## Bounded agent assignment

Copy this and fill in the bracketed fields. Assign one behavior per agent;
share a production file only when one agent owns its edits.

```text
Investigate [specific observed symptom] in [read-only / repair] mode.
Read AGENTS.md, docs/VALENTINE_PHILOSOPHY.md and [one trace path].
Use the trace's owner functions and tests to start; recheck them in this checkout.
Follow the actual entrypoint to the final consumer, including validation,
queued work, persistence/reload, presentation setters or result publication
that can change the outcome. Read each relevant caller and downstream owner.

Allowed edit scope: [named production/test files, or none].
Preserve [specific unaffected contract and compatibility cases from the trace].
Use [specific existing suite/command] and add a regression only when it closes
a consequential missing boundary. Use the real owning code across the failure
boundary. Identify every hardware/service/storage substitute.

Before repairing, reproduce a factual failure through the normal entrypoint
and record expected versus actual output. Then show the same trigger passing
and the unchanged controls still passing. If this cannot be demonstrated,
report the unproven condition and the next exact observation needed.

Return: cause and owner; input-to-output path; changed behavior; actual test
results; what the fix closes; what remains untested. Identify an out-of-scope
owner before handing its work back. Update the trace if the contract changed.
Keep private device evidence outside public files. Device operations need
their own stated task scope; do not infer them from a host-test request.
```

The trace replaces repeated discovery, not engineering judgment. A report is
useful when another person can identify the failing boundary, reproduce it,
and understand the limits of the result. More findings or more checks are not
the objective.
