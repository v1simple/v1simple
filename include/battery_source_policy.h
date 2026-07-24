#pragma once

#include <stdint.h>

// ─── Battery power-source classification policy (pure, hardware-free) ───
//
// PWR_BUTTON_GPIO (GPIO16) is dual-purpose on this board:
//   HIGH -> running from the internal cell, PWR button released
//   LOW  -> external/USB rail attached, OR the PWR button is being held
//
// A single instantaneous read therefore cannot separate "USB attached" from
// "button pressed". This policy turns a stream of sampling rounds into a
// stable classification under three rules:
//
//   1. TWO-ROUND AGREEMENT. A classification CHANGE requires two rounds,
//      separated in time, that agree. One disagreeing round leaves the
//      previous classification intact. Sampling a burst inside one instant
//      (bug #17 defect A) is not majority voting — it just reads the same
//      transient five times.
//
//   2. FAIL TOWARD BATTERY. Unknown, suppressed and unconfirmed states all
//      resolve to BATTERY. Reporting USB while actually running on the cell
//      suspends low-battery protection, so USB is the dangerous answer and is
//      never a default. A PWR-button wake holds GPIO16 LOW through the first
//      classification, which is exactly the case that used to report USB for
//      ~1s (bug #17 defect B).
//
//   3. ADC HEALTH IS NOT AN INPUT. Nothing here reads or depends on the ADC.
//      A failed ADC must degrade voltage reporting only and must never disable
//      power-button handling (bug #17 defect C).
//
// All time math uses the rollover-safe form:
//     static_cast<uint32_t>(now - then) >= span
// never `now >= then + span`.

namespace battery_source_policy {

enum class Source : uint8_t {
    Unknown = 0, // no confirmed classification yet — resolves to battery
    Battery = 1,
    Usb = 2,
};

/// Resolve a classification to the boolean the firmware acts on.
/// Only a positively confirmed USB classification reports "not on battery".
constexpr bool resolveOnBattery(Source source) {
    return source != Source::Usb;
}

constexpr const char* sourceName(Source source) {
    return source == Source::Battery ? "battery" : (source == Source::Usb ? "usb" : "unknown");
}

// A source transition can coincide with USB CDC re-enumeration, so the first
// log line may be electrically correct but unavailable to the host. Schedule a
// small, bounded set of steady-state evidence replays after each change. This
// changes only observability; the source decision above remains authoritative.
struct EvidenceReplayConfig {
    uint32_t intervalMs = 1000;
    uint8_t repetitions = 5;
};

struct EvidenceReplayState {
    uint8_t remaining = 0;
    bool clockSeeded = false;
    uint32_t lastEmissionMs = 0;
    uint32_t usbConfirmationElapsedMs = 0;
};

inline void armEvidenceReplay(EvidenceReplayState& state, uint32_t nowMs, uint32_t usbConfirmationElapsedMs,
                              const EvidenceReplayConfig& config = EvidenceReplayConfig{}) {
    state.remaining = config.repetitions;
    state.clockSeeded = true;
    state.lastEmissionMs = nowMs;
    state.usbConfirmationElapsedMs = usbConfirmationElapsedMs;
}

constexpr bool evidenceReplayDue(const EvidenceReplayState& state, uint32_t nowMs,
                                 const EvidenceReplayConfig& config = EvidenceReplayConfig{}) {
    return state.clockSeeded && state.remaining > 0 &&
           static_cast<uint32_t>(nowMs - state.lastEmissionMs) >= config.intervalMs;
}

inline bool takeEvidenceReplay(EvidenceReplayState& state, uint32_t nowMs,
                               const EvidenceReplayConfig& config = EvidenceReplayConfig{}) {
    if (!evidenceReplayDue(state, nowMs, config)) {
        return false;
    }
    state.remaining--;
    state.lastEmissionMs = nowMs;
    return true;
}

struct Config {
    /// Spacing between classification cycles once the classification is settled.
    uint32_t cycleIntervalMs = 1000;
    /// Minimum spacing between round 1 and its confirmation round. Must be long
    /// enough that a contact bounce or a button edge cannot span both rounds.
    uint32_t roundSpacingMs = 25;
    /// A LOW pin must persist at least this long before USB is accepted. Sized
    /// above the 2000 ms power-off hold so a deliberate button hold can never
    /// be mistaken for an external supply.
    uint32_t usbConfirmMs = 3000;
    /// Samples taken per round by the caller.
    uint8_t samplesPerRound = 5;
};

struct State {
    Source classification = Source::Unknown;
    Source pendingVerdict = Source::Unknown;
    bool hasPendingVerdict = false;
    /// True when the next round is the confirmation half of a pending change
    /// and should therefore run at roundSpacingMs instead of cycleIntervalMs.
    bool fastFollowUp = false;
    bool clockSeeded = false;
    uint32_t lastRoundMs = 0;
    /// When the current run of USB-looking rounds began.
    bool hasUsbCandidate = false;
    uint32_t usbCandidateSinceMs = 0;
};

struct Observation {
    uint8_t highSamples = 0;
    uint8_t totalSamples = 0;
    /// Caller-declared suppression: a PWR-button press/hold is in flight, so
    /// the LOW pin says nothing about the power source. Suppressed rounds can
    /// never form half of a two-round agreement.
    bool buttonInteraction = false;
};

enum class Outcome : uint8_t {
    Suppressed = 0,           // button held, or an empty round — nothing learned
    Confirmed = 1,            // round matched the settled classification
    AwaitingConfirmation = 2, // round disagreed; confirmation round scheduled
    Disagreed = 3,            // confirmation round contradicted round 1; held
    UsbHoldoff = 4,           // rounds agree on USB but the LOW has not persisted
    Changed = 5,              // two spaced rounds agreed; classification changed
};

struct Result {
    Outcome outcome = Outcome::Suppressed;
    Source classification = Source::Unknown;
    bool onBattery = true;
    bool changed = false;
    // Firmware-clock duration of the LOW run that qualified a USB change.
    // Zero for all non-USB changes and non-changing observations.
    uint32_t usbConfirmationElapsedMs = 0;
};

/// Majority vote over one round. Returns Unknown for an empty round.
/// An exact tie (only reachable with an even samplesPerRound) resolves to
/// Battery, keeping rule 2 intact at the boundary.
constexpr Source classifyRound(const Observation& observation) {
    if (observation.totalSamples == 0) {
        return Source::Unknown;
    }
    return (static_cast<uint32_t>(observation.highSamples) * 2u >= static_cast<uint32_t>(observation.totalSamples))
               ? Source::Battery
               : Source::Usb;
}

/// Is another sampling round due? The very first round is always due so a cold
/// boot classifies immediately instead of reporting a default for a full cycle.
constexpr bool roundDue(const State& state, uint32_t nowMs, const Config& config) {
    if (!state.clockSeeded) {
        return true;
    }
    const uint32_t span = state.fastFollowUp ? config.roundSpacingMs : config.cycleIntervalMs;
    return static_cast<uint32_t>(nowMs - state.lastRoundMs) >= span;
}

/// The boolean the rest of the firmware should act on right now.
constexpr bool onBattery(const State& state) {
    return resolveOnBattery(state.classification);
}

namespace detail {

inline void clearPending(State& state) {
    state.hasPendingVerdict = false;
    state.pendingVerdict = Source::Unknown;
}

inline void clearUsbCandidate(State& state) {
    state.hasUsbCandidate = false;
    state.usbCandidateSinceMs = 0;
}

} // namespace detail

/// Feed one sampling round into the classifier.
///
/// The caller is responsible only for reading the pin `samplesPerRound` times
/// and reporting how many came back HIGH; every decision lives here.
inline Result observe(State& state, uint32_t nowMs, const Observation& observation, const Config& config = Config{}) {
    if (!state.clockSeeded) {
        state.clockSeeded = true;
    }
    state.lastRoundMs = nowMs;
    state.fastFollowUp = false;

    Result result;
    result.classification = state.classification;
    result.onBattery = resolveOnBattery(state.classification);

    const Source verdict = observation.buttonInteraction ? Source::Unknown : classifyRound(observation);

    if (verdict == Source::Unknown) {
        // A held button (or an empty round) is not evidence. Drop any
        // half-finished agreement and any accumulated USB persistence so a
        // suppressed round can never contribute to a change.
        detail::clearPending(state);
        detail::clearUsbCandidate(state);
        result.outcome = Outcome::Suppressed;
        return result;
    }

    if (verdict == Source::Battery) {
        // Any HIGH reading breaks the run of LOW readings that USB requires.
        detail::clearUsbCandidate(state);
    } else if (!state.hasUsbCandidate) {
        state.hasUsbCandidate = true;
        state.usbCandidateSinceMs = nowMs;
    }

    if (verdict == state.classification) {
        detail::clearPending(state);
        result.outcome = Outcome::Confirmed;
        return result;
    }

    if (!state.hasPendingVerdict) {
        state.hasPendingVerdict = true;
        state.pendingVerdict = verdict;
        state.fastFollowUp = true;
        result.outcome = Outcome::AwaitingConfirmation;
        return result;
    }

    if (state.pendingVerdict != verdict) {
        // The two rounds disagree: hold the previous classification. The newer
        // round becomes the candidate so a genuine change still settles quickly,
        // but it still needs its own confirmation round before it is accepted.
        state.pendingVerdict = verdict;
        state.fastFollowUp = true;
        result.outcome = Outcome::Disagreed;
        return result;
    }

    if (verdict == Source::Usb && static_cast<uint32_t>(nowMs - state.usbCandidateSinceMs) < config.usbConfirmMs) {
        // Two rounds agree on USB, but the LOW has not outlasted a plausible
        // button hold yet. Keep the pending verdict and re-check next cycle.
        result.outcome = Outcome::UsbHoldoff;
        return result;
    }

    const uint32_t usbConfirmationElapsedMs =
        verdict == Source::Usb ? static_cast<uint32_t>(nowMs - state.usbCandidateSinceMs) : 0;
    state.classification = verdict;
    detail::clearPending(state);
    detail::clearUsbCandidate(state);
    result.classification = verdict;
    result.onBattery = resolveOnBattery(verdict);
    result.changed = true;
    result.usbConfirmationElapsedMs = usbConfirmationElapsedMs;
    result.outcome = Outcome::Changed;
    return result;
}

// ─── Power-button gating ──────────────────────────────────────────────────
//
// Bug #17 defect C: the PWR button used to be gated on hasBattery(), which
// returns false whenever ADC init failed. A board with a dead ADC could not be
// powered off at all. ADC health and cached-voltage validity are carried here
// as explicit inputs *and deliberately ignored*, so a regression that reroutes
// the button through them fails a test instead of shipping.

struct ButtonGateInputs {
    bool managerInitialized = false;
    Source classification = Source::Unknown;
    bool adcHealthy = false;          // ignored on purpose — see note above
    bool batteryVoltageValid = false; // ignored on purpose — see note above
};

/// May a PWR-button hold drive a shutdown right now?
///
/// Depends only on the manager being initialised and on not being positively
/// classified as USB. Never on ADC state: a failed ADC degrades voltage
/// reporting only, never input handling.
constexpr bool powerButtonHandlingEnabled(const ButtonGateInputs& inputs) {
    return inputs.managerInitialized && resolveOnBattery(inputs.classification);
}

} // namespace battery_source_policy
