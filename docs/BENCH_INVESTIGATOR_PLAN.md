# Bench Investigator V1 Plan

## Product outcome

After a bench run, one command investigates the run's firmware code, logs,
metrics, traces, stimulus, and camera evidence and writes a machine-readable
report that helps a developer fix real defects.

```text
python3 tools/bench_investigate.py <bench-run-directory> \
  --local-provider ollama --model qwen3-vl:8b
```

Run Ollama as a loopback-only, cloud-disabled service with enough context for a
coding agent. This is the configuration verified on the connected bench machine:

```text
OLLAMA_NO_CLOUD=1 OLLAMA_NOHISTORY=1 OLLAMA_HOST=127.0.0.1:11434 \
  OLLAMA_CONTEXT_LENGTH=98304 OLLAMA_FLASH_ATTENTION=1 \
  OLLAMA_KV_CACHE_TYPE=q4_0 ollama serve
```

The runner also rejects Ollama model tags ending in `:cloud` or `-cloud`. Larger
context windows consume more memory; a higher-precision KV cache can be selected
on machines where it fits.

`bench.sh` invokes that local form after collection and scoring. Its provider and
model can be changed with `BENCH_INVESTIGATOR_LOCAL_PROVIDER` and
`BENCH_INVESTIGATOR_MODEL`, but the automatic path never passes `--hosted` and
never falls back to a hosted service. Investigation is diagnostic: it never
changes the bench verdict or becomes a release gate.

Hosted review is a separate, direct opt-in by the person running it:

```text
python3 tools/bench_investigate.py <bench-run-directory> \
  --hosted --model gpt-5.6-sol
```

Only that explicit `--hosted` command authorizes sending evidence to the hosted
model. It may transmit selected raw run artifacts and source excerpts plus camera
contact sheets. Its read-only repository scope can also see other readable files
under the checkout, including ignored artifacts, so inspect that scope before
opting in.

## Report contract

The command atomically replaces `<run>/investigation.json`. The report contains:

- `execution_status`: runner-owned state recording whether model execution
  completed, failed, or returned partial results, without claiming investigation
  completeness or expressing a product pass/fail verdict;
- `source`: the run's recorded identity, the source revision actually inspected,
  and whether the attribution is exact, commit-reconstructed, current-only, or
  unavailable;
- `coverage`: each artifact, code selector, and video interval actually reviewed,
  plus skipped inputs, read errors, clock mappings, and timing uncertainty;
- `findings`: observed defects or strongly supported defect leads, with expected
  and observed behavior, causal status (`confirmed` or `probable`), counterevidence,
  resolvable evidence selectors, tight code locations, and a concrete fix;
- `unresolved`: important observations whose cause remains unknown, ranked
  hypotheses, and the specific next observation that would distinguish them;
- `model`: the model/tool version and hashes of its repository instructions.

Artifact selectors include a run-relative path, content hash, and the applicable
JSON Pointer, NDJSON line/event, CSV row/key, log line, or video PTS/frame interval.
Code selectors include revision, repository-relative path, symbol, and a tight
line range. The runner resolves cited paths and ranges before publication. An
invalid citation downgrades only its affected finding and is recorded in
`coverage`; it does not discard other useful results. If the run identity cannot
be matched to inspected source, code attribution remains a hypothesis.

A no-finding report is useful only when `coverage` shows what was truly examined.
Existing scores and camera grades are leads; the investigator must follow them
back to raw records, video, stimulus, and owning code before making a finding.

## Small implementation

### 1. Model-led runner

Add `tools/bench_investigate.py` as the single entry point. It will:

- accept current bench layouts and gracefully expose unfamiliar or partial
  artifacts rather than rejecting the run for incompleteness;
- give the installed `codex exec` a read-only, ephemeral repository session and
  a strict output schema;
- let the model search the repository, Git history, and all readable run files;
- validate resolvable report references and atomically publish the result;
- always publish an honest error/partial report when the backend fails;
- retain compact model identity and inspection diagnostics; the explicit
  local-debug option records structured counts, never raw transcript content.

V1 uses the concrete installed Codex CLI. Local mode selects Ollama or LM Studio
with `--local-provider` and supplies the CLI's local-model mode internally; V1
does not add a generic provider abstraction. Hosted mode exists only behind the
separate `--hosted` argument shown above.

### 2. Semantic video review

Video meaning is decided by the model together with stimulus, trace, log, and
code evidence. The runner provides generic frame/contact-sheet extraction, not
a fixed ROI, color threshold, seven-segment recognizer, reference image, or
closed transition catalog.

The first review covers the whole video with a generic full-frame temporal/change
scan plus bounded overview frames. The scan only locates candidate intervals; it
does not decide whether pixels are correct. The model may request higher-rate PTS
intervals based on any evidence it finds, and the runner repeats extraction and
review within explicit resource bounds. The report records reviewed and unsampled
PTS intervals, the anchor used to relate them to other clocks, and the uncertainty.
Expected display state comes from independently decoded stimulus and owning code,
never solely from parser-derived encounter data.

### 3. Minimal causal evidence

Current artifacts cannot prove notification-to-pixel timing or distinguish
several competing firmware causes. The thin investigator runs on existing
evidence first. Each addition below must then resolve a concrete uncertainty in
that report, extend an existing log or manifest where practical, and remain
ordinary evidence rather than a completeness requirement or validator:

- a host `bench_timeline.ndjson` with monotonic timestamps for serial commands
  and responses; pre/post-run `QSTATUS` round trips; replay requests and
  CoreBluetooth acceptance; and the camera recorder's first-appended-frame host
  time mapped to PTS zero;
- replay acceptance records carrying a preassigned stimulus sequence, emission
  ordinal, global TX sequence, characteristic, payload digest/bytes, and host
  time;
- a bounded, nonblocking qualification trace carrying session identity, ordered
  DUT time, RX/event sequence, characteristic, matching payload digest, byte
  length/unit, parse outcome, published state/alert revision, owning stage, and
  loss count;
- display commits tied to the qualification session and causal revisions, with
  the complete semantic render inputs actually supplied to the renderer;
- the DUT time in `QSTATUS`, so pre/post round trips bound host↔DUT offset and
  drift rather than asserting an exact clock;
- host hashes for the artifacts handed to successful firmware/filesystem/replay
  build and upload steps, plus a device-reported resident image/build identifier
  that can expose dirty or `--no-upload` mismatches;
- collection of the panic sidecar when it exists.

Loss or missing identity makes only the affected causal claim unknown; it never
changes the bench verdict. Each event stage is named so the model can locate its
owning symbol in source.

### 4. Scenario flexibility

The investigator reads the recorded stimulus values and timing rather than
hard-coded phases. Bench also gains managed-bench semantics with external
encounter data; stimulus and timing callbacks are made scenario-independent so
external playback retains the same readiness and evidence behavior. The raw
resolved scenario content, emitted-request ledger, and independent content hash
are retained in the run; a private source path is not. Any normalized view is an
optional model aid, never an input-admission schema. Investigation logic contains
no fixed five-second start, sample rate, alert sequence, or expected display value.

### 5. Bench integration and learning

After the scorer returns, `bench.sh` runs the explicitly selected local provider
even if scoring failed or did not create `bench_result.json`, then exits with the
original score status. A missing or failed local backend is visible but cannot
trigger a hosted fallback or alter that status. The new report is advisory and
replaceable.

Repository instructions contain evidence meanings, known clock limitations, and
the standard for a defensible finding, but no hard-coded issue catalog. After a
real defect is fixed and an after-fix run confirms it, a compact case may be
preserved only when retrieval demonstrably improves a later investigation. A
case is advisory and can never replace checking current evidence.

## Verification by product result

V1 is demonstrated by these outcomes:

- Orchestration tests cover safe run discovery, source-identity mismatch,
  selector resolution, partial inputs, model failure publication, iterative
  video requests, and atomic report replacement without encoding defect types.
- A real-model fixture containing an unnamed shared-oracle defect—stimulus bytes
  disagree while parser, encounter, display, and video agree—produces an
  actionable finding grounded in the independent stimulus, raw evidence, and
  owning code.
- A changed external replay scenario is preserved and interpreted without an
  investigator code change.
- A brief off-transition visual defect is surfaced by full-frame temporal/change
  scanning and decided through semantic follow-up review, with PTS and timing
  uncertainty reported.
- The latest existing run produces actionable findings or concrete unresolved
  causes, not merely artifact validation or copied scores.
- A fresh connected-DUT/camera run automatically produces `investigation.json`
  with actual evidence coverage and findings or concrete unresolved causes, and
  demonstrates the added causal/timing evidence on real hardware. A backend-error
  report alone does not satisfy this outcome.

## Explicit non-goals

- No new release gate, approval, owner, baseline, promotion, or immutable report.
- No fixed picture matcher or reference-image oracle.
- No catalog of hard-coded findings that substitutes for investigation.
- No claim of exact cause or timing beyond the recorded identity, clocks, and
  causal identifiers.
- No generic model-provider framework, fine-tuning system, or speculative case
  library in V1.
