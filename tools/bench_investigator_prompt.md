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

The runner supplies the complete run-relative artifact inventory, sizes,
runner-owned SHA-256 hashes, exhaustive frame-index summaries, and a per-kind
field dictionary. Use `python3 tools/bench_evidence.py list <run>` to inspect the
catalog, `records <run>` for bounded raw records, `frames <run>` for frame-index
finding aids, and `source` for exact-revision source slices. Use the exact run
directory argument in the runner context. Query subcommands are read-only; never
invoke the `build` subcommand in this evidence session.

Do not stream a large timeline, log, trace, table, or binary with `cat`, `sed`,
`tail`, or equivalent commands. Query across record kinds, fields, clocks, and
tight time windows. Before synthesis, obtain at least one bounded raw record and
resolvable raw selector from every available record-backed evidence class:
stimulus; host notification or log; receive, parse, or publication trace;
display commit; and metrics. For aligned camera or physical frame evidence,
semantically review at least one supplied cell and use a resolvable attached-cell
video selector. If a class is unavailable or cannot be queried, name it and the
limitation in coverage notes instead of substituting an inventory or index
summary. This one-record-per-class minimum proves access and selector grounding
only; it is not run-wide behavior triage. Before selecting a primary lead, use
bounded chronological queries spanning the available run to compare stage order,
causal identifiers, revision or identity combinations, and loss across
record-backed classes. Rank candidates from those cross-source discrepancies
before narrowing around a metric or video window. The `records` query accepts
literal `--where field=value` predicates, same-record field comparisons such as
`--compare 'left!=right'`, and bounded adjacent raw records through `--context N`.
Each returned record is labeled as a predicate `match` or adjacent `context` and
has its own selector. Choose fields from the run's dictionary; do not assume a
fixed schema. Compare anomalies generically:
magnitudes and outliers, ordering inversions between adjacent recorded stages,
and disagreement between derived
summaries and raw records. Do not use fixed timestamps, packet values,
expected-transition catalogs, or a known answer. Follow the strongest
cross-source anomalies through raw support, counterevidence, and the tightest
owning-code location available. If no defect is established, preserve a grounded
observation as unresolved or report the honestly limited coverage.

Index summaries and index rows are finding aids, never primary evidence. Every
published artifact selector must target the raw run-relative path, hash, and
line/row coordinates returned by the query tool. Do not cite any file below
`investigation_index/`.

Each `records` result supplies one citation-ready selector basis. Copy its kind,
path, hash, coordinates, and keys unchanged; only its description may be
rewritten. Cite multiple returned records separately. Never widen or merge
coordinate ranges, add keys, or combine keys from different results. Selector
keys are conjunctive and must all match one selected raw record.

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

Query raw evidence before trusting derived summaries. Depending on what exists,
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
- owning firmware, replay, collection, import, and scoring code queried only at
  the recorded source revision. Do not run Git directly or inspect the current
  worktree, later revisions, commit messages, or later history.

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

For the strongest cross-source anomalies, reconstruct a causal path only as far
as evidence supports it, for example:

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

Before synthesis, complete one bounded event-to-frame bridge for the single
highest-ranked time-localized candidate, even if you ultimately reject that
candidate. This is a causal-depth requirement, not an artifact-review quota.
Select an unambiguous runner-provided `event_to_video_bridges` entry. Query its
`status=verified` entry. First query the candidate's own raw artifact in its
recorded clock with `records --path EVENT_PATH --clock EVENT_CLOCK --start --end`
and any required clock-segment selectors. Preserve the complete
`query.clock_conversion` and its mapping uncertainty; its `host_earliest_ns`
through `host_latest_ns` is the complete converted host interval. Then query the
entry's raw timing artifact in `host_monotonic_ns` across that host interval
expanded on each side by exactly the entry's recorded `frame_margin_ns`; this
margin brackets point-sampled camera rows and does not replace the candidate's
original host interval or uncertainty. Preserve every returned
`results[].host_time` interval. For the event, timing, and native-frame bounded
queries, follow `next_offset` until it is null whenever `truncated` is true. Never
derive or cite an enclosing video-PTS
interval from incomplete results; report truncation as an exact missing edge.
Derive video PTS only from complete returned raw timing records that contain both
`video_pts_value` and `video_pts_timescale`, query every corresponding native
frame row, and request a bounded video interval if supplied
cells do not cover it. Cite the candidate record, recorded clock mapping, recorded
timing verification, raw camera-timing records, and reviewed attached cells. If
any edge is unavailable, name that exact missing edge in `coverage.notes` rather
than substituting an approximate anchor or silently abandoning the bridge.

Derive expected behavior independently from the recorded stimulus values and
timing plus the owning protocol and renderer code. Never encode or assume fixed
replay phases, a five-second start, a sample rate, fixed checkpoint values, or a
known alert sequence.

## Review video semantically

Use the supplied whole-video overview before drawing conclusions from selected
frames. Interpret displayed meaning with stimulus, trace, log, and code evidence.
Do not use a fixed crop, fixed ROI, reference image, pixel/color threshold,
seven-segment template, or closed catalog of expected transitions as the oracle.

The full-frame temporal index scores every decoded source frame at native cadence
and has contiguous zero-based frame rows with measured source PTS. It is
exhaustive automatic change scoring, not exhaustive semantic video review. The
context gives its frame count, native rate, score distribution, and bounded
top-change windows; use the `frames` query for exact index rows. Never turn
automatic scoring into a claim that all displayed meaning was reviewed.
A change score measures whole-frame pixel difference only. Its rank is not defect
likelihood, semantic importance, or causal priority. Treat the ranked windows as
coverage targets unless independent raw evidence makes one of their intervals a
candidate. Normal semantics in a ranked window is local counterevidence only; it
does not reject unrelated record-order candidates elsewhere in the run.

For every top-change window in every indexed main video, either semantically
review the supplied sheet cells and cite them in `coverage.video_intervals`, or
identify the window rank and PTS interval explicitly as unreviewed in
`coverage.notes`. Checking only the overview does not account for the indexed
windows.

The runner-owned attachment manifest binds each supplied image index and durable
run-relative sheet path/hash to its canonical source video path/hash, represented
interval, and row-major ordered cells. Temporary extraction filenames are not
evidence and must never appear in a selector. A cell with
`source_pts_measured: true` carries its measured `source_pts_seconds` and
zero-based decoded `source_frame_index`; those values may be cited at
native-frame granularity. Otherwise, `nominal_requested_pts_seconds` is only a
requested sampling label. Always preserve `pts_uncertainty_seconds` and
`pts_uncertainty_interval`, and never use a nominal label for exact timing or
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
- `ndjson`: one-based physical line range plus the query-returned keys, if any;
- `csv`: one-based data-record range after comments/header plus the query-returned
  keys, if any;
- `log`: one-based physical line range;
- `video`: source-video PTS interval, `attachment_index`, and `cell_indices`; use
  frame indices only when the manifest says source positions were measured.

Keep ranges tight. The selector description states exactly what the selected
record proves. Use `findings[].evidence` and `unresolved[].evidence` for run
artifacts; use their `code` arrays for repository source. Code selectors always
name the inspected revision, path, symbol, line range, and `selection_sha256`.
The `source` query returns a `code_selector_basis`; copy its revision, path, line
range, and `selection_sha256` unchanged, then add the symbol and description. If
you need a different line range, query that exact range instead of reusing a
digest. The symbol is the model's owner label; the runner resolves revision,
path, line range, and selected-line digest. For artifacts, the
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
