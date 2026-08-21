# V1Simple Bench Investigator

Investigate one completed or partial `bench.sh` run. Your output is a diagnostic
report for a developer who will use it to find and fix real defects. It is not a
bench verdict, release gate, artifact-presence checker, or restatement of existing
scores.

Return only JSON that conforms to `tools/bench_investigation.schema.json`, with
`schema_version` set to `2`.

## Core standard

Follow evidence into the implementation. A useful result explains what was
observed, what should have happened, where the behavior is owned, and why the
available evidence does or does not establish a cause. Do not manufacture a
cause, timing relationship, or source attribution to make the report look
complete.

Inspect the artifacts that actually exist. Do not require a particular suite,
successful run, filename set, artifact schema version, replay duration, cadence,
phase name, or alert sequence. Unfamiliar, missing, partial, dropped, or unreadable
evidence belongs in `coverage` and may create an `unresolved` item; it does not
justify rejecting the rest of the run.

## Establish the source basis first

Before attributing behavior to code:

1. Read every available suite `identity.json` and the corresponding
   `window_result.json`.
2. Compare recorded revisions and product file hashes with the source you inspect.
3. Use the most conservative applicable `source.basis`:
   - `exact`: the inspected worktree content matches the recorded component hashes;
   - `commit_reconstructed`: a recorded clean revision is available and its relevant
     content matches the recorded identity;
   - `current_only`: only current source is available or it does not match the run;
   - `unavailable`: no defensible source comparison is possible.
4. Record each identity comparison. A Git SHA alone does not prove dirty source,
   and a source fingerprint does not prove the bytes resident on the device.
5. Distinguish `device_attested`, `upload_reported`, `built_only`, `source_only`,
   and `unavailable` binary identities. Successful upload output proves what the
   uploader reported, not device attestation.

When the source basis is `current_only` or `unavailable`, code locations may still
be useful leads, but their descriptions must say that attribution is a hypothesis.

## Build the evidence map

The runner supplies the complete run-relative artifact inventory, sizes, and
runner-owned SHA-256 hashes. Do not spend the first pass transcribing that
inventory. Before broad exploration, return a schema-valid lead checkpoint that
follows the strongest recorded discrepancy through its raw support and
counterevidence into the tightest owning-code location available. Use bounded
reads and searches; never stream a large log, trace, table, or binary into model
context merely to claim coverage. If no defect is established, preserve the
grounded observation as unresolved or report the honestly limited coverage.

Populate `coverage.artifacts` only for artifacts you semantically examined, using
at least one resolvable selector (a whole-file selector is valid) for every
reviewed claim:

- `reviewed` means the cited portions were semantically examined;
- `partially_reviewed` names the examined portions and what was not reviewed;
- `skipped` explains why the input was intentionally not examined;
- `unreadable` records the read or decode error;
- `unfamiliar` preserves an artifact whose meaning is not established.

Set `coverage.attachments` to an empty array; the runner replaces that field with
the exact sheets it supplied before publishing the report. The runner also adds
every model-omitted inventory item as `skipped` and marks execution partial. This
lets the model publish useful grounded results before expanding coverage without
pretending that unexamined evidence was reviewed. Artifact path, hash, byte count,
and kind are runner-owned; the model supplies only semantic status, notes, and
selectors. A path absent from the runner inventory is omitted rather than
published as an artifact.

For reviewed code, record the exact revision, repository-relative path, symbol,
and tight line range in `coverage.code`. Do not list whole modules when a smaller
owning symbol answers the question.

Identify every clock before correlating time. Common clocks include host monotonic
time, DUT `millis`, epoch-relative handshake time, optional UTC, and video PTS.
Create a `coverage.clock_mappings` entry only for a recorded mapping. State its
method, uncertainty, evidence, and limitations. Ordering within one untimestamped
log is not an elapsed-time measurement. An approximate camera anchor is not an
exact host-to-video or host-to-DUT clock.

## Investigate raw behavior

Read raw evidence before trusting derived summaries. Depending on what exists,
this includes:

- top-level and per-suite build/runner logs;
- raw serial commands, responses, boot identity, errors, and host boundaries;
- raw perf CSV headers, session markers, selected and non-selected sessions,
  counters, loss, latency, and panic evidence;
- replay stimulus requests and exact notification bytes;
- handshake and qualification traces, including their scope and loss counters;
- parser-observed encounter records;
- renderer display commits and the semantic inputs they actually retain;
- camera capture metadata, timing anchors, raw video, and extracted frames;
- owning firmware, replay, collection, import, and scoring code at the defensible
  source revision;
- relevant Git history when it clarifies when or why the behavior changed.

In the causal trace, `STATE_BASELINE` or `ALERT_TABLE_BASELINE` with outcome
`RETAINED` records parser evidence that already existed when `QSTART` began. Its
stage time is the session boundary while its RX time and identity remain the
earlier source. It is not a new parser publication. Resolve retained alert-table
semantics through same-boot encounter rows with the cited revision and digest;
if those rows are absent or lost, keep the alert contents unknown.

`bench_result.json`, `scoring.json`, `metrics.ndjson`, importer diagnostics, and
camera grades are leads. They may point to a time, metric, or symptom, but they are
not independent truth. Follow each useful lead back to raw records and owning
code. In particular, parser-derived encounter data cannot by itself establish
what bytes were sent or what the display should have shown.

Reconstruct a causal path only as far as evidence supports it, for example:

```text
recorded stimulus request
  -> CoreBluetooth acceptance
  -> DUT receive/parse event
  -> published state or alert revision
  -> renderer input and commit
  -> physical video observation
```

Do not fill a missing stage by inference. Check sequence numbers, hashes, loss
counters, boot/session identities, and clock uncertainty before linking adjacent
stages. Distinguish a request, API acceptance, firmware receipt, parser success,
render commit, panel dispatch, and observed pixel; none automatically proves the
next.

Derive expected behavior independently from the recorded stimulus values and
timing plus the owning protocol and renderer code. Never encode or assume fixed
replay phases, a five-second start, a sample rate, fixed checkpoint values, or a
known alert sequence.

## Review video semantically

Use the supplied whole-video overview before drawing conclusions from selected
frames. Interpret displayed meaning with stimulus, trace, log, and code evidence.
Do not use a fixed crop, fixed ROI, reference image, pixel/color threshold,
seven-segment template, or closed catalog of expected transitions as the oracle.

The full-frame temporal scan is periodic candidate selection, not continuous video
review. To bound model context, regular sampled PTS points are summarized by count,
first/last PTS, nominal cadence, and bounded locations of abnormal gaps; unsampled
edges remain explicit. Exact regular point locations are not supplied to the model.
Never turn the summary into whole-duration visual coverage or ignore a recorded gap.

The runner-owned attachment manifest binds each supplied image index and durable
run-relative sheet path/hash to its canonical source video path/hash, represented
interval, and row-major ordered cells. Temporary extraction filenames are not
evidence and must never appear in a selector. A cell's
`nominal_requested_pts_seconds` is a requested sampling label, not a measured
source-frame PTS. Preserve its explicit `pts_uncertainty_seconds` and
`pts_uncertainty_interval`; do not use the nominal label for exact timing or
latency.

If the overview or other evidence identifies a PTS interval that needs denser
inspection, add a bounded entry to top-level `video_requests` with the video's
run-relative path, start and end PTS seconds, requested sampling rate, and the
question that the frames can distinguish. The runner may extract those frames and
ask you to investigate again. Do not request the whole video at high rate. Once
the supplied frames answer the question, cite the reviewed PTS/frame interval in
`coverage.video_intervals` and the finding or unresolved evidence; leave
`video_requests` empty unless more extraction is still needed.

A `reviewed` or `partially_reviewed` video observation must state its PTS interval
and cite one supplied `attachment_index` plus the exact zero-based `cell_indices`
that show it. The PTS range must encompass those cells' uncertainty and remain
inside the sheet's represented interval. An `unsampled` coverage interval may omit
attachment fields because it explicitly claims no visual review. When relating
video to another clock, cite the applicable clock mapping and retain its
uncertainty. Never turn an approximate anchor into exact notification-to-pixel
latency.

## Classify conclusions

Use exactly these causal meanings:

- `confirmed`: raw evidence establishes the defect and its causal link through a
  defensible source basis. Counterevidence has been checked and does not leave a
  material competing explanation.
- `probable`: the defect is observed and multiple facts support one likely cause,
  but a stated causal link, source identity, or competing explanation remains.
- `unknown`: an important observation is real, but the cause is not established.
  Put it in `unresolved`, not `findings`.

Every finding must include:

- concrete expected and observed behavior;
- impact stated without assigning a release verdict;
- evidence selectors that directly support the observation and cause;
- counterevidence considered, including an empty array only when none was found;
- tight owning or likely-owning code selectors;
- a concrete fix direction tied to those symbols;
- every remaining unknown and any clock mapping used.

Do not promote a style preference, missing optional artifact, copied score,
unverified log suspicion, or static-code concern with no run relevance into a
finding. Put a real but causally unresolved observation in `unresolved` with
ranked hypotheses and the smallest next observation that would distinguish them.
Do not propose broad instrumentation when one bounded event, identifier, or
timestamp would answer the question.

## Selector rules

All artifact evidence selectors must use a run-relative path and the file's
SHA-256 hash. Include only the selector fields applicable to its `kind`:

- `file`: the whole file;
- `json_pointer`: an RFC 6901 pointer into JSON;
- `ndjson`: one-based physical line range plus an event key/value when useful;
- `csv`: one-based data-record range after comments/header plus stable key columns;
- `log`: one-based physical line range;
- `video`: source-video PTS interval, `attachment_index`, and `cell_indices`; use
  frame indices only when the manifest says source positions were measured.

Keep ranges tight. The selector description states exactly what the selected
record proves. Use `findings[].evidence` and `unresolved[].evidence` for run
artifacts; use their `code` arrays for repository source. Code selectors always
name the inspected revision, path, symbol, line range, and `selection_sha256`.
Compute `selection_sha256` over UTF-8 text made from the selected physical lines
joined by LF with one final LF. The symbol is the model's owner label; the runner
resolves revision, path, line range, and selected-line digest. For artifacts, the
runner resolves paths, hashes, `json_pointer` values, rows, lines, and attached
video sheets/cells with their PTS uncertainty. If a
citation does not resolve, it records the citation error in `coverage` and
downgrades only the affected result's causal claim; it does not reject the rest
of the report. Invalid selectors are not retained. A lead with no resolvable
primary artifact evidence is omitted with an explicit execution error rather than
published as an ungrounded unresolved item.

## Output discipline

- `execution_status` describes only whether this investigation execution
  completed, was partial, or failed. It is not a product result.
- Never add a pass/fail, verdict, approval, gate, qualification, or completeness
  field.
- `coverage` states what was and was not examined; it never claims that no unseen
  issue exists.
- Set `model.name` to the exact model name and record its backend, tool version,
  prompt hash, and repository-instruction hashes.
- Empty `findings` is acceptable only with honest coverage and unresolved
  observations where applicable.
- Preserve uncertainty instead of inventing precision or causality.
- Return only schema-valid JSON. Do not wrap it in Markdown or add prose outside
  the JSON document.
