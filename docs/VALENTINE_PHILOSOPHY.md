# Design Philosophy — Valentine's Law

*The guiding philosophy for V1 Simple. Every design and implementation
decision — display priority, muting behavior, alert persistence, connectivity —
is meant to pass through this filter first.*

---

## Attribution & scope

This document blends two sources:

1. **Mike Valentine's published philosophy** about how a radar detector should
   serve a driver, drawn from Valentine Research's own writing. Direct quotes
   below are his; each is cited to a source page.
2. **This project's design decisions**, i.e. how we translate that philosophy
   into firmware for a standalone display + BLE proxy that rides on a Valentine
   One Gen2. Where a principle is *our* reading or extension rather than Mike's
   words, it is labelled **"In this project."**

This is an independent, community-built project. It is **not affiliated with,
endorsed by, or sponsored by Valentine Research, Inc.** "Valentine One," "V1,"
and "Valentine's Law" as used here are our shorthand for a design discipline
inspired by Mike Valentine's publicly stated approach; the phrasing of the
"Law" (below) is *ours*, not a verbatim Valentine Research slogan. See
`README.md` and `THIRD_PARTY_NOTICES.md` for trademark and attribution notices.

---

## Part I — Mike Valentine's principles (the source)

Everything downstream rests on a handful of ideas Mike has argued publicly for
decades.

### 1. Radar is line-of-sight physics; the detector buys you time

Traffic radar "has to hit your car before it can determine your speed," and it
"can't see around corners or through hills." A good detector wins by finding the
*scatter* — the glow beyond the hill — "a long time before the actual beam hits
your car." The whole value proposition is **early, truthful warning against an
ambush**, because "enforcers are ambush experts."
*(Source: About Radar Detectors; Evaluating Radar Detector Tests.)*

### 2. Direction is intelligence — the Radar Locator

The V1's defining feature is that it *locates* the threat with front and rear
antennas. Mike's rule for reading it: "If it points to the side, the bogey is
nonthreatening — radar can't get you from the side. If the Locator points ahead
or behind, try for visual identification." Direction is not decoration; it is
how the driver separates a real threat from a reflection.
*(Source: About Radar Detectors.)*

### 3. Urgency is tiered — not every alert deserves the same alarm

X-band is "a catch-all band … heavily populated by" door openers and is often
seen at long, harmless distances (the "Beep"). K and Ka are detected closer and
are "much more likely to be radar," so the V1 "makes a different sound ('Brap')
to warn you of these more urgent threats." **The signal's loudness should match
the threat's urgency.**
*(Source: About Radar Detectors.)*

### 4. Quantity is a clue — the Bogey Counter

Multiples usually mean junk: "most microwave door sensors have at least two
transmitters," and a counter "counting up quickly to four or more" is "likely a
nest of door sensors." But "single bogeys must be regarded as threats until you
see them or put them safely behind you."
*(Source: About Radar Detectors.)*

### 5. False alarms are inherent to the physics — you cannot design them away for free

"Since all radar detectors are simply radios tuned to the microwave
frequencies used by traffic radar, they automatically sound their alert whenever
they encounter signals on those frequencies … Every response indicates a threat,
a bogey." The instrument's honest job is to *report the RF environment*, not to
pretend it is quieter than it is.
*(Source: About Radar Detectors.)*

### 6. The cardinal sin is a missed real threat — **this is the root of the Law**

Mike's sharpest warning is about detectors that suppress too aggressively:

> "Some detectors, to avoid false alarms, ignore short, weak signals. Which
> means they ignore weak Instant On radar too. You don't want to find that out
> *after* you buy."
> *(Source: Evaluating Radar Detector Tests.)*

Suppressing a false alarm and missing a real one are **the same action** seen
from two sides. When they conflict, the miss is the unacceptable outcome.

### 7. The driver's judgment is the final arbiter — the device *informs*

"How can you tell the difference between radar and what people commonly refer to
as false alarms? Your judgment is the only way." The detector's duty is to give
the driver complete, honest, well-prioritized information — band, direction,
strength, count — and then get out of the way. It must never quietly make the
"it's probably nothing" decision *for* the driver.
*(Source: About Radar Detectors.)*

### 8. Be honest about limits, and measure what actually matters

Mike is candid that a bad windshield or metallized tint can gut performance —
"Even V1 can't protect you through those windshields" — and that vanity metrics
mislead: multi-mile alerts are "irrelevant … detector users don't get nailed at
five miles, or seven." His engineering yardstick: "If measurements aren't
repeatable, they aren't measurements," and a test is only meaningful if it
"simulate[s] *real* traps you'll face."
*(Source: About Radar Detector Range; Evaluating Radar Detector Tests.)*

---

## Part II — Valentine's Law (this project's distillation)

**In this project** we compress the above — especially principle #6 — into one
rule that the code refers to by name as *Valentine's Law*:

> **The only thing worse than detecting a false signal is failing to detect real
> radar. Every design decision — display priority, muting behavior, alert
> persistence — passes through this filter first.**

With one operational corollary for the highest-urgency threat we render:

> **Render laser alerts with the same urgency tier as Ka — immediately,
> prominently.** A muted or downgraded render on a live threat silently lowers
> urgency at the exact moment the Law says we must not.

> **Note on wording:** these two sentences are *our* maxims, phrased to be
> quotable in code review. They are a faithful compression of Mike's cited words
> (principles #3 and #6), not verbatim Valentine Research quotations. Keep them
> labelled as ours so no one later mistakes them for a manufacturer slogan.

### What the Law is *not*

It is **not** an excuse to spam the driver. Principles #4 and #7 pull the other
way: surface everything real, but keep it legible and let the driver judge.
The Law breaks ties in favor of *showing the threat*; it does not license noise
for its own sake.

---

## Part III — How the Law is enforced in this codebase

The Law is not a slogan in a doc; it is annotated at each place a shortcut could
silently drop a real threat. Current touch-points:

| Principle | Where it lives | What it enforces |
|---|---|---|
| Never downgrade a live threat (#6, corollary) | `src/modules/display/render_frame_composer.cpp` — the "Valentine philosophy enforcement" block | A live ALP laser event is force-unmuted at the composer so every downstream consumer (status strip, frequency, arrows, bands) renders the same full-urgency frame. A refactor can't reintroduce mute on the laser path. |
| Direction is truth (#2) | `src/display_arrow.cpp` — the "Valentine's Law note" | Laser-direction color overrides (front→red, rear→yellow) never suppress the V1's own radar-band direction arrows; each source keeps its authoritative direction. |
| The display must never lie by going stale (#7) | `src/display_update.cpp` ("Bounded-drift safety (Valentine's Law)"); `src/display.h` and `include/display_drawn_region.h` ("bounded-drift Valentine's Law argument") | Partial-flush optimizations are bounded so the worst case is a single stale frame; the next annotated frame repaints, and mode transitions force a full redraw. The panel never stays stale across frames. |
| Fidelity to the V1 (#1, #7) | `test/test_ble_client/test_ble_client.cpp` — "Valentine's Law surface" | Pins the V1 command-packet frame bytes; a mismatch would have commands "silently ignored or misinterpreted by the Valentine hardware." |

**In this project**, the rule of thumb when adding one of these annotations: if a
future maintainer could "optimize" your code in a way that hides, delays, or
misdirects a real alert, leave a `Valentine's Law` note explaining why the
slower/safer path is intentional.

---

## Part IV — The Priority Stack (the Law, operationalized)

The runtime priority order in the project instructions is Valentine's Law applied
to a real-time system. Higher tiers may never be blocked or starved by lower ones:

1. **V1 connectivity** — must stay connected. *You cannot warn about a threat you
   never received.*
2. **BLE ingest/drain** — lowest-latency path; never block. *Latency here is a
   missed Instant-On alert (principle #6).*
3. **Display updates** — responsive; never block BLE. *The driver's honest,
   current picture (principles #2, #7).*
4. **Metrics collection** — bounded time; degrade gracefully.
5. **Wi-Fi / Web UI** — off by default; maintenance mode only.
6. **Logging / persistence** — best-effort; drops OK, corruption not; never block
   the above.

*(The instructions file — `.github/instructions/`, kept local/gitignored — is the
source of this stack. Audio alerts, where present, sit just below display as a
best-effort tier that must not block BLE.)*

---

## Part V — The decision checklist

Before merging a change, run it through the Law:

1. **Does this risk hiding, delaying, or misdirecting a real alert?** If yes,
   stop. Choose the safer path or add a `Valentine's Law` guard + test.
2. **Does it touch Tier 1 (connectivity) or Tier 2 (BLE)?** Then it must not
   increase disconnects or add latency/jitter; justify it and add tests.
3. **Does it downgrade urgency** (mute, dim, defer, suppress) on any live-threat
   path? Only if the *threat* is genuinely gone — never as a side effect of a
   rendering or performance shortcut.
4. **Is the driver still the one deciding?** Surface the information; don't make
   the "probably nothing" call for them.
5. **Can you measure the claim?** If a change is "for performance" or "fewer
   false alarms," show a repeatable measurement — "if measurements aren't
   repeatable, they aren't measurements."

When in doubt, favor **showing the threat**. That is the whole point of the
instrument.

---

## Sources

Mike Valentine / Valentine Research (nominative reference, quoted under fair use):

- About Radar Detectors — https://www.valentine1.com/v1-info/about-radar-detectors/
- About Radar Detector Range — https://www.valentine1.com/v1-info/tech-reports/about-radar-detector-range/
- Evaluating Radar Detector Tests — https://www.valentine1.com/v1-info/tech-reports/evaluating-radar-detector-tests/
- Tech Reports index — https://www.valentine1.com/v1-info/tech-reports/

In-repo references (the Law as annotated in code):

- `src/modules/display/render_frame_composer.cpp` — "Valentine philosophy enforcement"
- `src/display_arrow.cpp` — "Valentine's Law note"
- `src/display_update.cpp` — "Bounded-drift safety (Valentine's Law)"
- `src/display.h`, `include/display_drawn_region.h` — "bounded-drift Valentine's Law argument"
- `test/test_ble_client/test_ble_client.cpp` — "Valentine's Law surface"
- `CONTRIBUTORS.md` — origin of the "Valentine-philosophy framing"
