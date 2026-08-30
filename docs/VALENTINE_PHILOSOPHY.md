# Design Philosophy — Valentine's Law

The design filter and behavior contract for all display, mute, and alert
behavior in V1Simple.

**This document is the maintainer's interpretation.** Part I quotes Mike
Valentine's published principles, with sources. Parts II–III and the named
deviation — "Valentine's Law" included — are how this project chooses to read
and apply those principles: our name, our rules, our trade-offs. The name
credits the source material; it does not claim his endorsement, and where this
document goes beyond what he published, the error is ours.

---

## Attribution and scope

This document blends two things and keeps them visibly separate:

1. **Mike Valentine's published principles**, quoted from Valentine Research's own
   pages under fair use. Sources are listed below.
2. **This project's rules**, which are our reading and extension of those
   principles. They are labelled **"Ours."**

This is an independent project. It is **not affiliated with, endorsed by, or sponsored
by Valentine Research, Inc.** "Valentine's Law" is *our* name for a design discipline
inspired by Mike's publicly stated approach. The phrasing is ours, not a Valentine
Research slogan. Keep it labelled that way so no one later mistakes it for one. See
`README.md` and `THIRD_PARTY_NOTICES.md` for trademark and attribution notices.

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

VR built the priority rule into the wire so accessories defer while alert
processing retains priority.

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
fade mute and sounds again** — new threat, new sound.

---

## Part III — Where the Law is enforced

The Law is not a slogan in a doc. It is annotated at each place a shortcut could
silently drop, delay, or downgrade a real threat.

| Principle | Where it lives | What it enforces |
|---|---|---|
| Never downgrade a live threat (#6, corollary) | `src/modules/display/render_frame_composer.cpp`, `synthesizeAlpPrimaryState()` | A live ALP laser event composes with `muted = false` before downstream rendering. |
| Direction is truth (#2) | `src/display_arrow.cpp`, the ALP color-override block | Laser-direction color overrides never suppress the V1's own radar-band direction arrows. Each source keeps its authoritative direction. |
| The display must not lie by going stale (#7) | `src/display_update.cpp`, the region-union partial-flush dispatch | Blink, arrow-visibility, and signal-bar changes bypass the partial route, while mode transitions force a full redraw. |
| Fidelity to the V1 (#1, #7) | `test/test_protocol_spec_conformance/test_protocol_spec_conformance.cpp`, user-bytes section | Pins the V1 profile command bits. A wrong row means a profile push could silently disable a detection band. |

---

## Named deviation

**Ours.** **ALP warm-up display suppression — deliberate.**
`src/modules/alp/alp_runtime_module.cpp` flags unconfirmed laser sessions as
`WARM_UP` and withholds them from the display based on the boot envelope,
preamble window, and heartbeat mode bytes.

The gate treats boot-envelope, preamble, and heartbeat-mode patterns as
unconfirmed startup traffic. Confirmation can come from a gun ID or LID deploy;
heartbeat-mode confirmation remains subject to the explicit boot-envelope and
mode gates in the runtime module.

`test/test_alp_runtime` covers the warm-up flag conditions, confirmation gates,
35-second envelope, and display suppression while a session remains flagged.

---

## Sources

Valentine Research, quoted under fair use as nominative reference.

- About Radar Detectors — https://www.valentine1.com/v1-info/about-radar-detectors/
- Evaluating Radar Detector Tests — https://www.valentine1.com/v1-info/tech-reports/evaluating-radar-detector-tests/

Specification, as distributed in VR's official repositories:

- ESP Specification 3.015 §2 p.7 (request priority), p.26 (blink method and rate),
  p.27 (`infDisplayData` layout), Table 9.1 p.34 (bar graph map)
