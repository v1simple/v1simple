# Display Visual Verification Harness

**Status:** Phase 4 bench-accepted for the hardened L2 machine scope
(`framebuffer-semantic-l1-l2-l3+flushshadow`). Final acceptance run
`ff433134dd53412e8b472a115b50e0ca` at firmware commit `6bb98ba` completed
44/44 steps and 9/9 transitions with 2,843 assertions, 14,371,959 checked
pixels, 71 flush-shadow comparisons, and 100/100 required L2 decodes. It
reported zero failures or errors, successful artifact finalization, and
successful cleanup/restoration. The run records verifier SHA-256
`ebe72e09d827e4aeb2e3326a51dd287a1cfa5a0bf8b794ba3670d973a131092b`,
settings fingerprint `0x717577AF`, and summary SHA-256
`faa3b5c7f5187f3fe9ab5ea187d33c7a7fd7f6e15c4bdc91426e332e1d82f372`.
**Scope:** physical-device execution remains bench-only evidence tooling and
does not replace native tests. The deterministic host verifier self-test is a
repository CI gate.
The final summary records `physical_panel_checked: false`: the machine verdict
proves the captured framebuffer semantics and flush-shadow bytes, not the panel
controller, glass, bezel, viewing conditions, or camera-visible output. The
operator reported that the physical display looked okay during the historical
v2 run. That remains useful qualitative evidence, not machine proof of the
final run or visual correctness beyond the assertions described here.

**Initial L2 exercised-corpus qualification, not final acceptance:** post-v2 run
`d1da756fea9441d48c8e3e669ac5fc7e` at firmware commit `dfa6788` and verifier
commit `2509f36` completed 44/44 steps and 9/9 transitions with 2,843
assertions, 14,371,959 checked pixels, 71 flush-shadow comparisons, 100 L2
decodes, zero failures or errors, successful artifact finalization, and
successful cleanup. Its verifier SHA-256 is
`f43c734cc2b0a2f39dfa5e2bc21b9a806a45c3ff1e7e008e9c1b2b200441c433`.
Those 100 decodes comprise 83 step decodes (39 numeric frequency strings plus
44 bogey-counter symbols) and 17 transition-target decodes. The old corpus had
only eight unique counter characters (`1`, `2`, `3`, `4`, `5`, `J`, `L`, and
`P`; 34/44 steps used `1`). An operator-supplied video was reviewed as
qualitative panel corroboration; local file `IMG_7626.MOV` is 74.113 seconds
with SHA-256
`c3096244620fa03ebe772c85ffbd4a392efc234f4a3bde1f0f877e92da7a1c05`.
It is not pixel-exact proof, is not tracked, and is not cryptographically bound
into the summary, whose `physical_panel_checked` value remains false.
That run exercised the initial decoder before recognized-character-domain
fail-closed hardening and remains supporting, pre-hardening evidence. Final run
`ff433134dd53412e8b472a115b50e0ca` exercised the newly authored complete
counter corpus and exact 100-decode workload on firmware `6bb98ba`, with the
hardened verifier SHA-256 above, and is now the authority for
`framebuffer-semantic-l1-l2-l3+flushshadow`. The v2 `da71b088...` acceptance
remains the historical authority for its narrower
`framebuffer-semantic-l1-l3+flushshadow` scope.
**V1 target:** deterministic machine verdicts for the existing display preview
sequence, without any screenshot baseline blessing.

## Problem

The native display suites compile against `test/mocks/display_driver.h`, where
the Arduino_GFX primitives are no-ops. Those tests can validate call sequences,
dirty-flag discipline, state transitions, and layout math, but they never observe
a rendered pixel.

That leaves a real blind spot. Regressions that garble frequency digits,
misplace an arrow, drop a signal bar, draw the wrong theme color, or leave stale
card pixels from a previous alert can pass every current gate.

## What Meaningful Means

This harness is only worth building if it fails for the right reasons. V1 is
considered meaningful only when all of these are true:

- The host has no hand-maintained preview-step mirror. Expected state comes from
  firmware.
- Pinning a step is a render barrier: when the route returns success, the
  framebuffer belongs to that exact step and includes a render sequence id.
- The host fails closed. Missing manifest fields, zero checked regions, unknown
  palette roles, unsupported framebuffer format, or skipped critical assertions
  are protocol errors, not passes.
- The run reports assertion counts, checked region counts, checked pixel counts,
  mask coverage, and step/transition coverage. A suspiciously empty run fails.
- L1, L2, and L3 verdicts require no goldens. Goldens can flag drift only after
  the semantic checks pass.
- The verifier has its own corruption tests. Synthetic framebuffers must prove
  it detects at least missing active bars, extra stale bars, wrong active colors,
  wrong inactive/background state, ghost card pixels, and framebuffer rotation
  mistakes.

## Verdict Model

Screenshot galleries and golden diffs are not the primary mechanism. A golden
diff detects change, not correctness, and a human can bless a broken baseline.
The harness must work like semantic UI assertions: the run itself fails with
named verdicts such as:

```text
FAIL step 034 ka_front_34700 primary.signal_bars:
expected main meter 8 lit bars (from preview strength 6), found 7; bar[7] is 96% dim/background
```

The HTML report is for triage after a machine verdict exists. It is not the
source of truth.

## Core Data Model

The device exports three machine-readable artifacts.

### Step Manifest

`/api/display/visual/steps` returns the complete preview table plus resolved
per-step expectations. The resolved form is critical: raw `PreviewStep` rows
contain carry-forward values (`NO_CHG`) and authored 0-6 per-alert strength,
while the rendered primary meter uses this display's local 8-slot scale.

Each step should include:

- Stable id and index, for example `034_ka_front_34700`.
- Raw preview inputs for traceability.
- Resolved carry-forward indicator state: mode char, profile slot, ALP state,
  OBD state, BLE state, main/mute volume, ALP gun override.
- Resolved primary alert state: band, direction, frequency MHz, muted/photo
  flags, expected frequency text, active band mask, active direction mask,
  flash mask, band flash mask, and expected main meter count on the 0-8 scale.
- Resolved secondary/third card expectations: slot order, band, direction,
  frequency text, live/graced state if applicable, and 0-6 card bar count.
- Resolved status expectations: bogey char, mode char, profile label/color role,
  mute icon state, ALP/OBD/BLE badge state, volume text state, and hidden/shown
  states driven by settings.

The host may derive simple display-independent facts, but it must not duplicate
carry-forward logic, preview bar scaling, band label substitutions, ALP override
text, or settings-driven color choices.

Resolution must also be single-sourced inside the firmware: `renderStep` and
the manifest generator call the same extracted helpers (carry-forward
application, 0-6 to 0-8 meter scaling, band label substitution, ALP override
text). A manifest generator that re-implements resolution is the same drift bug
moved one layer down.

Transport: manifests use the existing `json_stream_response.h` buffered-client
write pattern, but the large step table should not be built as one whole-table
`JsonDocument` (that would be a large short-lived heap allocation in
maintenance mode). Phase 1 either adds a small sibling incremental JSON writer
or performs a measure pass followed by a write pass, serializing one resolved
step at a time. The steps payload carries `stepCount` and a closing
`"complete": true` marker; the host treats a missing marker or a count mismatch
as a protocol error.

### Layout Manifest

`/api/display/visual/layout` returns semantic geometry in logical display
coordinates (`640x172`). Coarse zones from `display_layout.h` are useful for
bounds checks, but they are not enough for semantic verification. V1 needs tight
rects or polygons for the elements the verifier asserts.

The manifest should expose:

- Logical screen size and raw framebuffer transform.
- Coarse zones: frequency, bands, main signal bars, cards, top counter, status
  strip, arrows.
- Tight element geometry: each band label cell, each main signal bar, each card
  slot, each card meter bar, arrow cluster and per-arrow regions, top counter,
  mute icon, volume text area, profile area, ALP/OBD/BLE/GPS/WiFi/battery areas.
- Element role metadata: `background`, `active_fill`, `inactive_dim`,
  `outline`, `anti_aliased_text`, `text_coverage`, `optional_dynamic`, or
  `ignored`.
- Known overlap metadata. Examples: profile clear can overlap battery; top band
  cell can overlap GPS. Overlaps must be explicit so cleanliness checks do not
  hide real stale pixels.

Layout generation should come from renderer-owned constants/helpers, not from a
second host-side map. Where draw code currently keeps geometry as file-local
constants, implementation should extract small geometry helpers that both the
renderer and manifest generator call.

### Palette Manifest

The layout response also includes the active palette and settings-derived role
colors:

- Background, text, gray, dark gray, muted, persisted.
- Band colors, photo band color, frequency color, arrow colors.
- Computed 8-slot main meter ramp and 6-slot card ramp.
- ALP, OBD, BLE, WiFi, volume, RSSI, profile, and battery role colors.

The verifier compares pixels to roles, not hard-coded theme values.

### Manifest Binding

All three manifests and the framebuffer response carry the same binding fields:
manifest schema version, firmware version/SHA, and a settings fingerprint (a
hash over the settings that influence rendering: theme, custom colors,
hidden-element toggles). The host records the binding at run start and fails
the run if any later response reports a different value: a mid-run settings
change or device reboot invalidates every subsequent verdict.

## Framebuffer Contract

The device streams the raw Arduino_Canvas framebuffer without copying it. The
host converts it into logical display coordinates before checking layout rects.

Required response headers:

```text
X-FB-Raw-Width: 172
X-FB-Raw-Height: 640
X-FB-Logical-Width: 640
X-FB-Logical-Height: 172
X-FB-Format: RGB565LE
X-FB-Transform: canvas-rotation-1
X-Display-Render-Seq: <monotonic integer>
X-Display-Pinned-Step: <step index>
```

The transform must match the display code's documented mapping:

```text
logical(lx, ly) -> raw(px = 171 - ly, py = lx)
```

The host must fail if the byte length is not exactly
`raw_width * raw_height * 2`. The response sends an exact `Content-Length`
(no chunked encoding) so truncation is also detectable at the transport layer.

The transform is not hypothetical: it matches the rotation-1 mapping already
documented and used in `display.cpp` (`flushRegion`, canvas created as
`Arduino_Canvas(172, 640, ..., rotation=1)`) and in
`display_frequency_raster_cache.cpp` (`physX = rawStride - 1 - ly`). The
verifier's rotation self-test encodes the same mapping independently.

## Firmware Architecture

Add a dedicated service instead of overloading the existing color-preview routes:
`WifiDisplayVisualApiService`.

All routes require maintenance boot mode. State-changing routes also require
the existing `X-V1Simple-Request: maintenance-ui` write header and rate limiting.
Framebuffer reads are maintenance-only too; they should not be exposed during
normal driving runtime.

| Route | Method | Behavior |
| --- | --- | --- |
| `/api/display/visual/steps` | GET | JSON step manifest with raw and resolved expectations. |
| `/api/display/visual/layout` | GET | JSON layout, transform, palette, role colors, and manifest version. |
| `/api/display/visual/pin` | POST | Body `{"index":N,"clear":true|false}`. Validates range, synchronously renders the pinned frame, returns `renderSeq`. |
| `/api/display/visual/framebuffer` | GET | Streams raw RGB565LE canvas bytes for the last pinned frame plus the headers above. |
| `/api/display/visual/clear` | POST | Cancels visual-test pinning, clears preview-owned overrides, restores normal maintenance display state. |
| `/api/display/visual/flushshadow` | GET | Streams the render-sequence-bound RGB565LE shadow of bytes delivered to the panel while a visual-test pin is active. |

### Pinning Semantics

`DisplayPreviewModule::pinStep(index, clear)` must be deterministic:

- It validates `index`.
- It disables timed preview advance while pinned.
- It resolves carry-forward state by resetting carry variables and applying
  steps `0..index`; it does not render skipped steps.
- If `clear=true`, it starts from a clean display state: full background clear,
  element-cache invalidation, blink phase reset to image1/on phase, and a new
  render sequence id.
- If `clear=false`, it leaves the framebuffer and element caches intact, resets
  blink phase to image1/on phase, and renders the target once. This is the mode
  used for transition/ghosting assertions.
- It renders synchronously before returning. The route must not return before
  the framebuffer contains the pinned frame.
- It cleans preview-owned ALP/OBD/BLE/profile/volume overrides on clear/cancel.

`firstFrame=true` is not enough to freeze blink behavior. The display code's
blink state is controlled by `blinkPhase_` and `lastBlinkToggleMs_`, so the
visual-test path needs an explicit render-test hook that pins the phase for the
duration of the render or resets it immediately before the render.

Pinning must also own the display exclusively. Cooperative maintenance-mode
writers already gate on the preview-running flag: `wifi_visual_sync_module`
skips its WiFi icon refresh while `displayPreviewRunning` is set, so pinned
mode keeps that flag asserted and inherits the existing suppression convention.
Known dynamic writers to freeze (or verify frozen): WiFi icon sync and battery
glyph updates.

The render sequence id is owned by `V1Display` and increments on every flush,
not only on preview renders. The pin route returns the seq of the pinned
render; the framebuffer response reports the current seq; the host asserts they
match. A non-cooperative writer repainting between pin and capture bumps the
seq and fails the capture loudly instead of silently corrupting a verdict.

## Assertion Layers

### L1 - Structural Semantics

For every pinned step, the host checks declared element geometry against the
resolved step manifest.

Important predicates:

- Band cells: active expected bands have active band color coverage; inactive
  cells are the expected dim/resting label state, not background. Ku relabeling
  must be represented in the manifest if present.
- Direction arrows: active arrows have active color coverage; inactive arrows
  have the expected dim resting glyph; blink-off state is not used in visual
  test captures because the blink phase is pinned.
- Main signal bars: expected count is the resolved 0-8 display count, not the
  raw preview 0-6 value. Lit bars match the computed 8-slot ramp or muted color;
  unlit bars match the expected dim slot state.
- Frequency zone: expected text role is present (`34.700`, `LASER`, `--.---`,
  or ALP gun abbreviation). L1 checks coverage/bounds/color role; L2 also
  decodes numeric frequency strings exactly.
- Secondary card slots: expected slot presence, card background/border role,
  direction glyph, band/frequency coverage, and 0-6 meter bars. Empty slots must
  be clean.
- Status strip: top counter, mute icon, volume text, profile label, ALP/OBD/BLE
  badges, WiFi/GPS/battery if enabled, and hidden-state clears are checked
  against resolved expectations and settings.
- Palette conformance: flat-primitive regions may contain only allowed role
  colors. Anti-aliased text regions use lit-coverage and dominant-color checks
  because blended pixels are expected.
- Zone cleanliness: pixels outside all declared drawable/optional regions must
  be background. This catches stray draws, stale pixels, and broad accidental
  invalidation. Regions declared `optional_dynamic` are reported separately and
  must stay small.

Suggested initial thresholds:

- Filled primitive interior: at least 90% expected color or accepted dim role.
- Inactive/empty primitive interior: at least 98% expected inactive/background
  role.
- Anti-aliased text region: non-background coverage within a manifest-declared
  min/max range, and dominant non-background color matching the expected role.
- Cleanliness: zero non-background pixels outside declared regions, except
  explicitly masked pixels.

Thresholds are implementation constants in the host tool and should appear in
the report. Do not tune them silently to make a bench run pass.

Historical amendment (2026-07-10, after the first physical run; superseded by
the later expansion below): coverage floors alone
proved insufficient for band letters — run `8a599c91` passed six framebuffer
band-letter chops because manifest-declared 1% floors governed. The host now
owns band-cell floors (`BAND_MIN_COVERAGE = 0.35`, `BAND_GLYPH_MIN_SPAN =
0.90`; manifests may strengthen, never weaken), asserts the glyph bounding
box spans its tight cell on both axes, and — because one chop class keeps
full span and near-baseline coverage — every transition captures a clean
reference render and compares the dirty target pixel-exact over band cells,
arrows, and meter bars (`deterministic_reference_regions`). Frequency, status
text, and badge rasters were excluded from pixel-exact comparison at that
milestone because they varied
sub-glyph between clean pins of the same step (measured 480 px on the top
counter), which is a known pin-determinism gap. Rationale and measurements:
`docs/reviews/2026-07-10-display-visual-check-usefulness.md`.
Update 2026-07-11: the top-counter variance was diagnosed as OpenFontRender's
first-character bearing adjustment reading the stale FT_Face glyph slot
(render-history dependent under FTC caching), affecting both text placement
and text measurement. Fixed two ways: a tracked vendored-library patch
(`scripts/patch_openfontrender.py`) removes the first-character adjustment —
the project's layout math expects standard typographic placement, which
fixed-cell 7-segment text requires — and the counter layout additionally
takes its clamp bounds from the boot-primed cache. The pixel-exact
clean-reference scope now covers every asserted/non-ignored deterministic
element, frequency and status text included. Final Phase 3 acceptance run
`da71b088ef59435f9a6bff80a721fa95` at commit `dfa6788` produced identical
dirty-target and clean-reference frame hashes for all nine transitions and
confirmed the expanded scope with 44/44 steps, 9/9 transitions, 2,743
assertions over 12,955,264 pixels, 71 flush-shadow comparisons, zero failures
or errors, and successful cleanup and artifact finalization under the recorded
settings.

### L2 - Exact Numeric-Frequency And Bogey-Character Decoding

Implemented 2026-07-13 by segment sampling, not atlas templates. The initial
exercised-corpus qualification passed, followed by hardened-corpus acceptance
run `ff433134dd53412e8b472a115b50e0ca` on firmware `6bb98ba` with verifier
SHA-256 `ebe72e09d827e4aeb2e3326a51dd287a1cfa5a0bf8b794ba3670d973a131092b`.

The original plan was template matching against firmware-exported
`DisplayFrequencyDigitAtlas` cells. That was dropped as tautological: the
atlas IS the render cache, and matching pixels against the data that drew
them cannot catch a corrupted cache. Instead the host decodes the rendered
pixels structurally. Numeric-frequency ink is located, kerned-into-contact
digits are split at ink valleys, and declared-overlap edge ink is discarded.
The single-cell counter uses host-owned per-character ink bounds for the
qualified 60 px embedded V1SevenX renderer; seven segment zones are sampled,
the pattern maps to a character, and its four ink edges, center, and width/height must match
the qualified physical cell. Numeric NN.NNN frequency ink similarly has
host-owned outer bounds keyed by the first/last digit in the qualified 82 px
lane plus host-owned ink bounds for every digit and the decimal-point cell.
This is independent of live firmware caches, expected-value pixel templates,
and color, and prevents bbox normalization from hiding whole-raster or
single-interior-glyph translations and scale changes. The software fallback is not separately
qualified and renderer provenance is not manifest-exposed; a fallback raster
that happened to satisfy the same semantic pattern and physical bounds could
pass, so L2 does not prove which renderer produced a conforming raster.

Scope: numeric frequency strings (digits and dots) are decoded and
asserted against `resolved.primary.frequencyText`; the bogey counter symbol is
decoded and asserted against `resolved.status.bogeyChar`. The complete
recognized nonblank bogey-character domain (excluding the separate counter-dot
bit) is digits plus `&`, `u`, `J`, `L`, `C`, `U`, `#`, `c`, `d`, `F`, `P`,
`A`, `E`, and `b`. Blank must be exactly one space and is checked as L1
background/no-ink rather than counted as an L2 decode. Empty, multicharacter,
or other nonblank values are protocol errors, not silent L2 skips. The counter
dot is not manifest-exposed, so it is not yet
semantically verified; clean-reference and flush-shadow checks prove only
render-path/delivery coherence for those pixels. Alpha frequency texts
(`LASER` and ALP gun abbreviations) keep the coverage checks and are outside
segment decoding. Unknown rendered segment patterns decode to `?` and fail.

Validation has two levels. Ad hoc archived-capture replays of runs 8a599c91,
1533471d, and fa221725 produced 83 decodes each (39 numeric frequency strings
plus 44 counter symbols) with zero misreads; those replays are supporting local
evidence, not a tracked CI test. Device run
`d1da756fea9441d48c8e3e669ac5fc7e` added 17 transition-target decodes for 100
total and passed its entire exercised corpus. Independent fixed-mask fixtures
cover every legitimate symbol rather than deriving expected pixels from the
decoder's own map; a source-parity test binds that oracle to the production
`decodeBogeyCounterByte` switch, and every embedded V1SevenX protocol glyph is
rasterized and decoded in CI. All 168 one-segment additions/removals must
change the decoded value, none of the 103 noncanonical nonblank masks may
alias a protocol character, and every one-segment remnant fails an expected
`8`. Whole-segment corruptions and all 103 noncanonical masks are crossed with
axis translations from -5 through +5 px plus the tolerated one-pixel diagonal
neighborhood. Every embedded glyph is also probed at +/-5 px on both axes and at
0.50x, 0.75x, 1.25x, and 1.50x scale; the tested transforms, a mirrored `1`,
and both halves of a truncated narrow `1` fail. Numeric decoding rejects
shortened non-dot runs before bbox normalization, including a frequency `8`
erased to one vertical segment, and end-to-end fixtures reject whole-string
horizontal/vertical translation and uniform shrink. Separate fixtures move
and resize an interior narrow `1` while leaving the other five cells untouched,
including width-only compression; a separate fixture shrinks only the decimal
point. Exact horizontal bounds for the narrow `1` and exact two-axis dot bounds
make the per-cell geometry contract reject those disproportionate size losses. The
host transition fixture also
pins the exact 40-region clean-reference contract by name, order, count, and
uniqueness. The fixed 44-step preview corpus deliberately sweeps the complete
recognized nonblank bogey-character domain, excluding the dot bit; the host
rejects a short, reordered, or character-domain-incomplete device manifest before pinning.
It also requires all 44 counter steps to be nonblank, exactly 39 numeric
frequency steps, and the fixed six-value numeric corpus (`10.525`, `24.150`,
`24.199`, `33.800`, `34.700`, and `35.500`). The five alpha-only positions are
pinned to step indices 15, 16, 17, 30, and 31, so redistributing numeric and
alpha labels cannot preserve only the global count and silently change
transition coverage. For the selected steps and discovered transition targets,
the verifier precomputes the required L2 decode count and promotes any
actual/expected mismatch to `ERROR`; before a default full run pins the first
frame, that expected workload must equal 100. A full run must therefore report
`l2_decodes=100` and `l2_decodes_expected=100`. A native API
test checks the serialized response. Final run
`ff433134dd53412e8b472a115b50e0ca` observed that same contract on-device:
44/44 authored steps, 9/9 transitions, all 24 recognized nonblank counter
symbols, 39 numeric labels over the fixed six-value corpus, the five pinned
alpha positions, and 100/100 required L2 decodes. The
firmware-exported `/api/display/visual/atlas` route is not planned: matching
against the rendering cache would be tautological. Because the full
recognized-character-domain hardening followed
`d1da756fea9441d48c8e3e669ac5fc7e`, the final run is bound instead to firmware
`6bb98ba` and verifier SHA-256
`ebe72e09d827e4aeb2e3326a51dd287a1cfa5a0bf8b794ba3670d973a131092b`.
It passed with 2,843 assertions, 14,371,959 checked pixels, 71 flush-shadow
comparisons, zero failures/errors, successful cleanup/restoration, and
successful artifact finalization.

### L3 - Transition/Ghosting Checks

Transition checks pin two frames without a clean reset between them:

```text
pin A with clear=true
capture A
pin B with clear=false
capture B
assert previous-only pixels from A are gone or changed to B's expected inactive/background state
```

The curated v1 transition list should include:

- Multi-alert -> single alert: removed card slot is background.
- Single alert -> multi-alert: new card slot appears and does not overlap
  frequency or signal bars.
- High main bars -> low main bars: truncated bars become dim, not stale lit.
- Alert -> idle/clear equivalent, if a preview step covers it; otherwise keep
  out of v1 and add a future preview step.
- ALP badge on -> off and OBD/BLE badge on -> off.
- Frequency text wide -> narrow and alpha text -> numeric, to catch stale glyph
  edge pixels.
- Flashing band/arrow step -> non-flashing step, with blink phase pinned.

Each transition assertion names the source step, target step, semantic region,
and expected cleanup state.

Formally, each transition verdict is the full L1 predicate set evaluated on B's
dirty-path render, plus one targeted stale check: regions active in A and
expected inactive/background in B must match B's expected inactive state. This
reuses the L1 engine on a dirty framebuffer instead of introducing a second
assertion engine. What L3 adds is reachability (`clear=false` renders exercise
the cache/dirty-flag paths a clean render skips) and A-derived targeting of
where staleness would land.

### Flush-shadow verification (amendment 2026-07-11, Stages A+B shipped)

The L1-L3 verdict reads the framebuffer, but this project's documented
display incidents (clipped band labels from a too-small dirty window, Diag14
arrows steady on the panel with a correct framebuffer, wrong-region partial
blits) live in the framebuffer-to-panel hop. While a visual pin is active the
firmware mirrors every byte actually pushed to the panel — full flushes and
`flushRegion()` row blits alike — into a PSRAM shadow exposed at
`GET /api/display/visual/flushshadow` (same binding/sequence headers as the
framebuffer route, plus `X-FB-Shadow: 1`). The host asserts
`shadow == framebuffer` after every pin capture: a mismatch names pixels that
were painted but never flushed (dirty-window under-coverage) or flushed
wrong. The shadow is enabled by `pinStep()`, freed by `clearVisualPin()`, and
allocation failure fails closed via 503 rather than skipping the comparison.
The host runs the comparison after every pin (steps, transition sources,
dirty targets, clean references), requires render-sequence equality between
capture and shadow fetch, and reports the widened scope in the RESULT line
(`+flushshadow`) and summary (`flush_shadow_checked`, `flush_shadow_compares`).
Those fields are execution-derived: the flag/suffix remains absent if a run
fails before its first valid shadow comparison, rather than describing only
the requested option.
`--no-flush-shadow` is a recorded compatibility escape for older firmware.
This proves delivery to the panel interface, not the glass itself; bezel and
panel-controller defects still need the operator eyes-on-glass step or a
future camera stage.

### L4 - Golden Drift

Optional, off by default. After L1-L3 pass, pixel diffs against blessed captures
may flag unknown visual drift such as font shape changes. L4 failure alone does
not fail the run unless an operator explicitly opts into a drift gate.

Goldens are a convenience for investigation, not the source of truth.

## Host Tool

`tools/display_visual_check.py <device-ip>`

Responsibilities:

- Fetch and validate the step/layout/palette manifests.
- Pin and capture every step, plus curated transition pairs.
- Convert raw RGB565LE framebuffer bytes to logical `640x172` pixels.
- Run L1, L2, and L3 assertions.
- Emit line-oriented verdicts and a machine-readable JSON summary.
- Emit a single-file HTML report with failures first, annotated captures,
  expected regions, failing pixels, decoded role summaries, and raw PNGs for
  triage.

Required options:

```text
--step N
--filter <substring>
--transitions-only
--no-report
--output-dir <path>
--goldens
--update-goldens
--strict-masks
```

Exit codes:

- `0`: all enabled assertions passed.
- `1`: semantic assertion failures.
- `2`: device/protocol/tool error.

Dependencies: Python standard library plus Pillow. Host self-tests should use
`unittest` or the repo's existing Python test style, without adding a new test
framework just for this tool.

Access discipline: strictly sequential, single connection at a time. The
synchronous WebServer serves one client. Expected full-run budget: ~85 pins and
captures plus the transition list, roughly 2-4 minutes on bench WiFi. The tool
never issues concurrent requests.

## Masks

Masks are allowed only for regions proven nondeterministic after investigation.
The initial mask file is empty.

Rules:

- Each mask entry has a reason, date, evidence note, and owner.
- Masks cannot overlap mandatory semantic primitive interiors unless the run is
  explicitly started with `--allow-semantic-masks`.
- The report prints masked pixel count and percentage.
- `--strict-masks` fails if masks are present. This keeps mask debt visible.

## Validation Plan

1. Firmware unit tests:
   - `pinStep` validates range, clear/no-clear behavior, carry-forward
     resolution, preview override cleanup, blink phase determinism, and render
     sequence id changes.
   - Step manifest tests verify raw and resolved fields for representative
     preview rows, including `NO_CHG`, `AUTO_BC`, secondary/third alerts, ALP
     states, and 0-6 to 0-8 main meter scaling.
   - Layout manifest tests verify key rects against renderer helpers and reject
     out-of-bounds or all-screen declarations.
   - API handler tests verify maintenance gating, write header requirements,
     rate limiting on POST routes, response headers, invalid index behavior, and
     protocol errors when display/framebuffer is unavailable.

2. Host verifier self-tests:
   - Build synthetic logical framebuffers from a minimal manifest.
   - Confirm clean known-good frames pass.
   - Mutate one thing at a time and confirm named failures: missing bar, extra
     lit stale bar, wrong color, text coverage below threshold, stale card
     pixels, non-background outside declared regions, and wrong raw-to-logical
     rotation.

3. Repo checks:
   - `pio test` target coverage for new native tests, matching existing style.
   - `pio run -e waveshare-349` compile gate.
   - Existing semantic guard scripts stay green.
   - The physical-device runner stays out of `scripts/ci-test.sh`; its
     deterministic host self-test runs there without device access.

4. Bench acceptance:
   - First bench run requires no goldens.
   - The summary must show nonzero steps, nonzero transitions, nonzero semantic
     regions, and nonzero checked pixels.
   - Any failure must name the step/transition, semantic element, expected role,
     observed role/coverage, and capture id.

## Risks And Countermeasures

- Shared-source geometry tautology: L1 uses firmware-declared geometry, so a
  bug that moves draw code and manifest geometry identically can escape. Mitigate
  with manifest sanity tests, hard screen/overlap bounds, L3 cleanup assertions,
  and optional L4 drift review.
- Broad or lazy geometry declarations: reject all-screen semantic rects, report
  checked-region coverage, and require tight helper-owned geometry for asserted
  primitives.
- Blink nondeterminism: do not rely on `firstFrame`; add an explicit visual-test
  blink freeze/reset hook.
- False failures from inactive UI: inactive arrows, inactive band labels, and
  unlit bar slots are often dim drawings, not background. The manifest must
  encode inactive visual roles.
- Framebuffer orientation mistakes: headers and verifier self-tests cover the
  raw `172x640` to logical `640x172` transform.
- Server blocking: rendering a pinned frame in a route is acceptable because
  this is maintenance-only bench tooling. Keep the route comments explicit so it
  is not copied into normal runtime paths.
- L2 confidence: segment decoding does not consume live firmware font/cache
  data, fails closed for the complete recognized nonblank character domain
  (excluding the dot bit), and is covered by independent fixed-pattern and
  host-owned geometry fixtures. It still proves only the captured
  framebuffer regions and asserted strings; alpha frequency labels remain L1
  coverage assertions and renderer provenance is not asserted. Accepted run
  `ff433134dd53412e8b472a115b50e0ca` is bound to firmware `6bb98ba` and the
  recorded verifier SHA-256; those limitations remain.

## Phase Documentation Convention

Each phase writes its local review record under
`docs/reviews/display-visual-verification/`. That directory is intentionally
ignored by repository policy; durable acceptance facts belong in this tracked
design document and `CHANGELOG.md`:

```text
docs/reviews/display-visual-verification/
  phase-1/COMPLETION.md
  phase-2/COMPLETION.md
  phase-3/COMPLETION.md
  phase-4/COMPLETION.md
```

A phase is complete only when its `COMPLETION.md` exists and contains: scope
delivered vs planned, every file added/changed with a one-line purpose, exact
commands to build/test/run what the phase produced, gate evidence (which repo
checks ran and their results), deviations from this design with reasons, and
open items handed to the next phase or the bench operator. Working decisions
worth keeping go in `phase-N/NOTES.md`. The absence of `COMPLETION.md` is what
marks a phase incomplete — implementation prompts key off that file.

## Build Phases

### Phase 1 - Firmware Contract

- Extract shared resolution helpers (carry-forward application, 0-6 to 0-8
  meter scaling, band label substitution, ALP override text) used by both
  `renderStep` and manifest generation first, since everything binds to them.
- Add a small incremental JSON response helper, or a two-pass measure/write
  helper, for large manifests that cannot be materialized as one `JsonDocument`.
- Add `WifiDisplayVisualApiService`.
- Add visual-test pin/clear APIs to `DisplayPreviewModule`.
- Add `V1Display` accessors for raw framebuffer metadata, the render sequence
  id (incremented on every flush), and a visual-test blink phase hook.
- Add step/layout/palette manifest generation (streamed via
  `json_stream_response.h`), with manifest binding fields.
- Add API docs and native handler/unit tests.

Done when manifests validate locally, pinning is deterministic in native tests,
and `pio run -e waveshare-349` compiles.

### Phase 2 - Host Semantic Verifier

- Add framebuffer fetch/decode/rotation.
- Implement manifest validation.
- Implement L1 structural assertions for bands, arrows, main bars, cards,
  status badges, frequency coverage, palette conformance, and cleanliness.
- Add synthetic verifier self-tests and corruption matrix.

Done when synthetic tests prove the verifier fails on deliberate corruptions and
passes known-good synthetic frames.

### Phase 3 - Bench Report And Transitions

- Add curated transition runner and L3 cleanup assertions.
- Add HTML/JSON reports and PNG capture export.
- Run against a physical unit in maintenance mode and triage real failures.

Code-done when the transition runner, reports, and capture export are covered
by host self-tests and a bench-run checklist exists in the phase folder.
Bench-accepted when a physical run — an operator task, not an agent task —
produces either actionable failures or a non-empty pass summary without any
golden baseline; the operator records the outcome in `phase-3/COMPLETION.md`.
Phase 3 met that criterion on 2026-07-13 with run
`da71b088ef59435f9a6bff80a721fa95` at commit `dfa6788`. Its machine verdict is
authoritative only for `framebuffer-semantic-l1-l3+flushshadow`; the operator's
concurrent report that the glass looked okay remains a qualitative observation,
not proof of physical-panel correctness.

### Phase 4 - L2 Decoding And Deferred Enhancements

- L2 exact numeric-frequency and bogey-character decoding by independent
  seven-segment sampling is implemented. Full recognized-character-domain
  fixtures (excluding the counter-dot bit),
  fail-closed manifest handling, dynamic HTML scope/count reporting, and the
  host CI gate are bench-accepted by run
  `ff433134dd53412e8b472a115b50e0ca`, bound to firmware `6bb98ba` and verifier
  SHA-256 `ebe72e09d827e4aeb2e3326a51dd287a1cfa5a0bf8b794ba3670d973a131092b`.
- Export the expected bogey-counter dot state and add a semantic dot assertion.
- Theme sweep by changing display settings per pass.
- Optional L4 golden drift support.
- Serial capture fallback if WiFi transport becomes unreliable.
