# Design Philosophy — Valentine's Law

The design filter and behavior contract for all display, mute, and alert
behavior in V1Simple.

**This document is the maintainer's interpretation.** Part I quotes Mike
Valentine's published principles, with sources. Parts II–V and the named
deviation — "Valentine's Law" included — are how this project chooses to read
and apply those principles: our name, our rules, our trade-offs. The name
credits the source material; it does not claim his endorsement, and where this
document goes beyond what he published, the error is ours.

---

## Attribution and scope

This document blends two things and keeps them visibly separate:

1. **Mike Valentine's published principles**, quoted from Valentine Research's own
   pages under fair use. Every quote below carries its source URL, and every URL was
   re-verified on 2026-08-05.
2. **This project's rules**, which are our reading and extension of those
   principles. They are labelled **"Ours."**

This is an independent project. It is **not affiliated with, endorsed by, or sponsored
by Valentine Research, Inc.** "Valentine's Law" is *our* name for a design discipline
inspired by Mike's publicly stated approach. The phrasing is ours, not a Valentine
Research slogan. Keep it labelled that way so no one later mistakes it for one. See
`README.md` and `THIRD_PARTY_NOTICES.md` for trademark and attribution notices.

Sourcing rule for this file: a principle stays only while its citation resolves and
supports it. This document was deleted once (`1e44f2f`, B14) for carrying claims it
could not ground. Anything added here needs a live citation or the **Ours** label.

---

## Part I — Mike Valentine's principles

### 1. Radar is line-of-sight; the detector buys you time

Radar "has to hit your car before it can determine your speed," and it "can't see
around corners or through hills." The V1 wins by finding the scatter — "it can find
these scatters a long time before the actual beam hits your car." Enforcers "hide
behind a hill or a bridge abutment"; a detector that can't "find the glow behind the
hill" can't warn you.
*(About Radar Detectors; Evaluating Radar Detector Tests.)*

### 2. Direction is intelligence — the Radar Locator

"Look first at the Radar Locator. If it points to the side, the bogey is
nonthreatening — radar can't get you from the side. If the Locator points ahead or
behind, try for visual identification." Direction is how the driver separates a real
threat from a reflection.
*(About Radar Detectors.)*

### 3. Urgency is tiered

"X-band alerts ('Beep') are often found at long distances. K and Ka bands are usually
detected at closer range, and alerts on those frequencies are much more likely to be
radar. So Valentine One makes a different sound ('Brap') to warn you of these more
urgent threats." Loudness matches urgency.
*(About Radar Detectors.)*

### 4. Quantity is a clue — the Bogey Counter

"Most microwave door sensors have at least two transmitters"; a counter "counting up
quickly to four or more" means "you've likely found a nest of door sensors." But
"single bogeys must be regarded as threats until you see them or put them safely
behind you."
*(About Radar Detectors.)*

### 5. False alarms are inherent to the physics

"Since all radar detectors are simply radios tuned to the microwave frequencies used
by traffic radar, they automatically sound their alert whenever they encounter signals
on those frequencies… Every response indicates a threat, a bogey." The instrument's
job is to report the RF environment, not to pretend it is quieter than it is.
*(About Radar Detectors.)*

### 6. The cardinal sin is a missed real threat — the root of the Law

> "Some detectors, to avoid false alarms, ignore short, weak signals. Which means they
> ignore weak Instant On radar too. You don't want to find that out *after* you buy."
> *(Evaluating Radar Detector Tests.)*

Suppressing a false alarm and missing a real one are the same action seen from two
sides. When they conflict, the miss is the unacceptable outcome.

### 7. The driver judges; the instrument reports

"How can you tell the difference between radar and what people commonly refer to as
false alarms? Your judgment is the only way." The device's duty is complete, honest,
well-prioritized information — band, direction, strength, count — and then getting out
of the way. It must never quietly make the "it's probably nothing" call *for* the
driver.
*(About Radar Detectors.)*

### 8. Measure honestly, and refuse vanity metrics

"If measurements aren't repeatable, they aren't measurements." Long-range alerts are
"irrelevant… detector users don't get nailed at five miles, or seven." And on reading
a test that maxed out its own range: "Throw the red flag when you see bar graphs
running the full length of the grid." A test is only meaningful if it simulates
"*real* traps you'll face."
*(Evaluating Radar Detector Tests.)*

### 9. Alert processing outranks accessory service — in the protocol itself

Not from the marketing pages but from VR's own specification, which is the more
binding statement of the same value:

> "The Valentine One will attempt to process all incoming requests as quickly as
> possible. However, alert processing remains the Valentine One's highest priority, so
> some requests may be rejected or delayed."
> *(ESP Specification 3.015 §2, p.7 — hence `infV1Busy`, `respRequestNotProcessed`,
> and TS Holdoff.)*

VR built the priority rule into the wire so accessories learn to wait rather than the
V1 learning to hurry. Our tier system is the same rule from the accessory side.

---

## Part II — Valentine's Law

**Ours.** A compression of principles #6 and #3, phrased to be quotable in review:

> **The only thing worse than detecting a false signal is failing to detect real
> radar. Every design decision — display priority, muting behavior, alert
> persistence — passes through this filter first.**

With one operational corollary for the highest-urgency threat we render:

> **Render laser alerts at the same urgency tier as Ka — immediately, prominently.**
> A muted or downgraded render on a live threat silently lowers urgency at the exact
> moment the Law says we must not.

One enumerated deviation exists — ALP warm-up suppression; see **Named
deviation** at the end of this document.

### What the Law is not

It is **not** a license to spam the driver. Principles #4 and #7 pull the other way:
surface everything real, keep it legible, let the driver judge. The Law breaks ties in
favor of *showing the threat*. It does not license noise for its own sake.

### The screen/speaker contract

**Ours, and load-bearing.** Two output channels, one authority each.

**Screen — the Law governs normal-operation presentation absolutely, and a live
alert owns it.** The rendered frame is the driver's source of truth. Ownership
when outputs compete:

1. A live ALP laser alert holds the primary display; concurrent V1 radar renders
   as a card alongside it, while the duplicate V1 laser row is suppressed.
2. Absent a live ALP alert, the display follows the V1.
3. Live beats persisted — a live V1 alert takes the screen from a persisted ALP
   display.
4. A live alert preempts normal-runtime interactive presentation owners: the
   local settings sliders close and a display preview is cancelled. Neither may
   hold the screen against a live alert.
5. When alerts clear, the display returns to the normal view. The sliders are
   never auto-restored; the driver can reopen them.

**Speaker — the user's authority is absolute.** The internal speaker is a
convenience, not the contract; a driver is never required to have it on. User-
and profile-set volumes are honored exactly as configured, **zero included** —
the firmware does not decide what someone else's volume should be. Comfort
behavior (`VolumeFadeModule`, `SpeedMuteModule`) operates *within* those
configured levels: speed mute is a standing user choice, so below the threshold
alerts suppress to the configured level with no exceptions, and volume fade
settles to its configured level after its delay. Within whatever volume the
user allows, a **newly detected, distinct V1 priority alert releases any active
fade mute and sounds again** — new threat, new sound. (An opt-in laser/Ka
breakthrough of speed mute is a possible future feature; it is not part of this
contract.)

This split is what makes audio comfort features legitimate under principle #7 —
the information never leaves the screen — and it is why any future feature that
hides something *on the panel* is a different and much harder argument.

---

## Part III — Where the Law is enforced

The Law is not a slogan in a doc. It is annotated at each place a shortcut could
silently drop, delay, or downgrade a real threat.

| Principle | Where it lives | What it enforces |
|---|---|---|
| Never downgrade a live threat (#6, corollary) | `src/modules/display/render_frame_composer.cpp`, `synthesizeAlpPrimaryState()` | A live ALP laser event composes with `muted = false`, so every downstream renderer paints the same full-urgency frame. A refactor cannot reintroduce mute on the laser path. |
| Direction is truth (#2) | `src/display_arrow.cpp`, the ALP color-override block | Laser-direction color overrides never suppress the V1's own radar-band direction arrows. Each source keeps its authoritative direction. |
| The display must not lie by going stale (#7) | `src/display_update.cpp`, the region-union partial-flush dispatch | Partial-flush optimizations are bounded so the worst case is one stale frame; unreliable small-window paths (blink, arrow visibility, signal bars) force a full push, and mode transitions force a full redraw. |
| Fidelity to the V1 (#1, #7) | `test/test_protocol_spec_conformance/test_protocol_spec_conformance.cpp`, user-bytes section | Pins the V1 profile command bits. A wrong row means a profile push could silently disable a detection band. |

**The rule of thumb when adding one:** if a future maintainer could "optimize" your
code in a way that hides, delays, or misdirects a real alert, leave a `Valentine's
Law` note explaining why the slower or safer path is intentional.

---

## Part IV — The priority stack

The tier system is the Law applied to a real-time system. Higher tiers may
never be blocked or starved by lower ones.

1. **V1 connectivity** — you cannot warn about a threat you never received.
2. **BLE ingest/drain** — latency here is a missed Instant-On alert (#6).
3. **Display update** — the driver's honest, current picture (#2, #7).
4. **Audio** — best-effort; must not block the above.
5. **Metrics** — bounded time, degrade gracefully.
6. **Wi-Fi / web UI** — maintenance mode only, off by default.
7. **Logging / persistence** — drops OK, corruption not; never blocks the above.

---

## Part V — The review checklist

1. **Does this risk hiding, delaying, or misdirecting a real alert?** If yes, stop.
   Take the safer path or add a `Valentine's Law` guard and a test.
2. **Does it touch Tier 0?** Then it must not add latency, jitter, or disconnects.
   Justify it with a measurement.
3. **Does it downgrade urgency** — mute, dim, defer, suppress — on a live-threat path?
   Only if the threat is genuinely gone. Never as a side effect of a rendering or
   performance shortcut. If it is audio-only and stays within the user's configured
   volumes, say so explicitly — the speaker is the user's channel, the screen is not.
4. **Is the driver still deciding?** Surface the information; don't make the "probably
   nothing" call for them.
5. **Can you measure the claim?** "If measurements aren't repeatable, they aren't
   measurements."

When in doubt, favor **showing the threat**. That is the whole point of the instrument.

---

## Named deviation

**Ours.** **ALP warm-up display suppression — deliberate.**
`src/modules/alp/alp_runtime_module.cpp` flags some laser sessions as `WARM_UP`
and withholds them from the display, on heuristics (boot envelope, preamble
window, heartbeat mode bytes). It is the one place the firmware decides a live
laser event is not worth showing, so it is enumerated here rather than quietly
kept.

**Why it stands:** bench reads of the wire show false laser signals during ALP
start-up, and the window is short. Principle #5 obliges us to report the RF
environment — but a signal the accessory emits at itself while booting is not
the environment. The unmarking paths keep it honest: an observed LID deploy or
a DLI/LID engage promotes the session to real immediately.

**Pinned:** `test/test_alp_runtime` holds the conformance suite — the flag
conditions (no-heartbeat boot, warm-up heartbeat mode, idle-mode trigger,
preamble window), every promotion path (gun ID, LID deploy, 02→04 engage,
targeted heartbeat), the 35 s envelope expiry, and the display suppression
itself (`hasLaserEvent()` stays false while flagged). A refactor cannot widen
the window without failing those tests.

---

## Sources

Valentine Research, quoted under fair use as nominative reference. All re-verified
2026-08-05.

- About Radar Detectors — https://www.valentine1.com/v1-info/about-radar-detectors/
- Evaluating Radar Detector Tests — https://www.valentine1.com/v1-info/tech-reports/evaluating-radar-detector-tests/
- The V1 Difference — https://www.valentine1.com/the-v1-difference/
- About Us — https://www.valentine1.com/about-us/

Specification, as distributed in VR's official repositories:

- ESP Specification 3.015 §2 p.7 (request priority), p.26 (blink method and rate),
  p.27 (`infDisplayData` layout), Table 9.1 p.34 (bar graph map)
