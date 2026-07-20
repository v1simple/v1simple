#pragma once

#include <stdint.h>

namespace MainRuntimePolicy {

// ─── Maintenance-session lifetime policy ──────────────────────────────
//
// A maintenance boot exists purely to serve the web UI. Two failure modes
// bound the design:
//
//   1. A session must never die under a user who is mid-task (bug #18: the
//      timeout used to be pure elapsed-since-start, so a person working in
//      the UI was kicked out at exactly ten minutes).
//   2. A session must never live forever (bug #1 kept this timeout as the
//      TERMINAL ESCAPE for a wedged session; removing the bound removes the
//      last safety net, and the device is useless as a radar-detector
//      accessory while it sits in maintenance mode with BLE/V1 down).
//
// The shape chosen is an IDLE-TIMEOUT WINDOW WITH AN ABSOLUTE CAP, not a
// plain "extend once to a larger ceiling":
//
//   * Idle window (MaintenanceBootTimeoutMs, unchanged at 10 minutes) is
//     measured from the last observed UI request. A session with no UI
//     traffic at all therefore still expires exactly ten minutes after it
//     started, which is byte-for-byte the old behaviour. It also reclaims
//     the device promptly in the common case where somebody clicks once and
//     then closes the laptop.
//   * Absolute cap (MaintenanceBootMaxSessionMs) is measured from session
//     start and is never reset, extended or refreshed by anything. It is the
//     terminal escape. A plain ceiling-on-first-activity would instead hold
//     the device for the full ceiling after a single click, which is strictly
//     worse for the common case.
//
// ─── Why the cap has to do the heavy lifting ──────────────────────────
//
// The only activity signal in the firmware is WiFiManager::markUiActivity(),
// which is called from WiFiManager::checkRateLimit() — i.e. on EVERY HTTP
// request, including the web UI's own /api/status poll (every 3 s from
// interface/src/lib/stores/runtimeStatus.svelte.js). There is deliberately no
// second, parallel activity signal.
//
// Consequence: an abandoned-but-open browser tab keeps marking activity with
// nobody in front of it, so the idle window alone cannot distinguish "user
// working" from "tab left open". That is exactly why the extension is capped
// rather than sliding indefinitely, and why the cap is deliberately modest
// (3x the base budget, not hours): the worst case for an abandoned tab is
// MaintenanceBootMaxSessionMs, after which the device reboots into normal
// runtime regardless of how much HTTP chatter it is still receiving.
//
// Rejected alternatives, recorded so they are not re-litigated:
//   * Inferring "real" activity from request *rate* (poll floor is 1 req/3 s,
//     so anything denser is human) — fragile: several pages poll their own
//     endpoints at their own cadence, a page load issues a burst of GETs, and
//     multiple tabs multiply the floor.
//   * A dedicated presence/keepalive route the UI only hits on real input —
//     correct, but needs a new route in src/wifi_routes.cpp plus the
//     route/response contract gate. Recorded as the proper follow-up.

// Base maintenance-session budget, and now also the idle window. Retained at
// exactly ten minutes so an idle session's behaviour is unchanged.
constexpr unsigned long MaintenanceBootTimeoutMs = 10UL * 60UL * 1000UL;

// Hard ceiling on total session length, measured from session start. Never
// reset. No amount of UI activity can push a session past this.
constexpr unsigned long MaintenanceBootMaxSessionMs = 30UL * 60UL * 1000UL;

// WiFiManager exposes UI activity only as an "was there a request within this
// window" predicate (isUiActive), never as a timestamp. The maintenance loop
// samples that predicate with this short window and latches the observation
// time, so the latched activity timestamp tracks the real request to within
// one second of a ten-minute budget.
constexpr unsigned long MaintenanceUiActivityProbeMs = 1000UL;

struct MaintenanceSessionInput {
    uint32_t nowMs = 0;
    // Immutable session start. 0 means "no session".
    uint32_t sessionStartedMs = 0;
    // Last observed UI request. 0 means "none observed this session".
    uint32_t lastUiActivityMs = 0;
    uint32_t idleWindowMs = static_cast<uint32_t>(MaintenanceBootTimeoutMs);
    uint32_t maxSessionMs = static_cast<uint32_t>(MaintenanceBootMaxSessionMs);
    bool sessionActive = false;
};

struct MaintenanceSessionDecision {
    bool shouldReboot = false;
    bool idleWindowExpired = false;
    bool maxSessionReached = false;
    // True when UI activity has actually pushed the deadline out past the
    // original elapsed-since-start deadline. Logging/diagnostics only.
    bool extended = false;
    uint32_t elapsedSinceStartMs = 0;
    uint32_t elapsedSinceActivityMs = 0;
    uint32_t remainingMs = 0;
    // Timestamp T such that the session ends at exactly (T + idleWindowMs),
    // i.e. remainingMs == idleWindowMs - (nowMs - T).
    //
    // CONTRACT (do not break these two lines independently):
    //   src/main.cpp assigns this to MainRuntimeState::maintenanceBootStartedMs
    //   every maintenance loop, and src/main_runtime_wiring.cpp publishes
    //   `maintenanceBootUptimeMs = millis() - maintenanceBootStartedMs` in
    //   /api/status. Keeping the field on the anchor is what makes the web
    //   UI's `maintenanceBootTimeoutMs - maintenanceBootUptimeMs` arithmetic
    //   equal remainingMs exactly, including while the deadline is extended
    //   and while the absolute cap is clamping it — with no change to the
    //   /api/status response shape.
    //
    //   The cost is that maintenanceBootUptimeMs is no longer literally
    //   "session uptime" once a session has been extended; it is
    //   (idleWindowMs - remainingMs). The countdown in
    //   interface/src/routes/+layout.svelte is its only production consumer.
    //   test/test_main_runtime_maintenance_policy pins both halves so the
    //   coupling cannot rot silently.
    //
    // Never 0 for a live session: 0 is the status payload's "no session"
    // sentinel.
    uint32_t deadlineAnchorMs = 0;
};

/// Decide whether a maintenance session has expired, and how much time is
/// left. Pure: time comes in as a parameter, nothing here calls millis().
///
/// All time math is elapsed-difference based
/// (`static_cast<uint32_t>(now - then) >= interval`) so it survives the
/// 49-day millis() rollover. No timestamp is ever added to an interval.
inline MaintenanceSessionDecision evaluateMaintenanceSession(const MaintenanceSessionInput& input) {
    MaintenanceSessionDecision decision;
    if (!input.sessionActive || input.sessionStartedMs == 0) {
        // No session: remainingMs 0 and anchor 0, which the status payload
        // already treats as "not in maintenance boot".
        return decision;
    }

    decision.elapsedSinceStartMs = static_cast<uint32_t>(input.nowMs - input.sessionStartedMs);

    uint32_t elapsedSinceActivity = decision.elapsedSinceStartMs;
    if (input.lastUiActivityMs != 0) {
        elapsedSinceActivity = static_cast<uint32_t>(input.nowMs - input.lastUiActivityMs);
    }
    // Activity can never predate the session (the latch is cleared at session
    // start), so anything older than the session start is a stale or racing
    // sample. Clamping also absorbs a "future" timestamp, which would
    // otherwise wrap to ~49 days and force an instant reboot.
    if (elapsedSinceActivity > decision.elapsedSinceStartMs) {
        elapsedSinceActivity = decision.elapsedSinceStartMs;
    }
    decision.elapsedSinceActivityMs = elapsedSinceActivity;
    decision.extended = elapsedSinceActivity < decision.elapsedSinceStartMs;

    decision.idleWindowExpired = elapsedSinceActivity >= input.idleWindowMs;
    decision.maxSessionReached = decision.elapsedSinceStartMs >= input.maxSessionMs;
    decision.shouldReboot = decision.idleWindowExpired || decision.maxSessionReached;

    const uint32_t idleRemaining = decision.idleWindowExpired ? 0u : (input.idleWindowMs - elapsedSinceActivity);
    const uint32_t capRemaining = decision.maxSessionReached ? 0u : (input.maxSessionMs - decision.elapsedSinceStartMs);
    decision.remainingMs = (idleRemaining < capRemaining) ? idleRemaining : capRemaining;

    // remainingMs <= idleRemaining <= idleWindowMs, so this cannot underflow.
    const uint32_t consumedMs = input.idleWindowMs - decision.remainingMs;
    const uint32_t anchor = static_cast<uint32_t>(input.nowMs - consumedMs);
    decision.deadlineAnchorMs = (anchor == 0) ? 1u : anchor;
    return decision;
}

} // namespace MainRuntimePolicy

struct MainRuntimeState {
    bool bootReady = false;
    unsigned long bootReadyDeadlineMs = 0;
    bool bootSplashHoldActive = false;
    unsigned long bootSplashHoldUntilMs = 0;
    bool initialScanningScreenShown = false;
    unsigned long activeScanScreenDwellMs = 0;
    unsigned long v1ConnectedAtMs = 0;
    bool alpSignalActive = false;
    bool wifiManualStartIntentLatched = false;
    bool wifiAutoStartDone = false;
    bool maintenanceBootActive = false;
    // Deadline ANCHOR, not the session start: the session always ends at
    // (maintenanceBootStartedMs + MainRuntimePolicy::MaintenanceBootTimeoutMs).
    // Republished every maintenance loop from
    // MaintenanceSessionDecision::deadlineAnchorMs — see the contract note on
    // that field before touching this or the /api/status emit.
    unsigned long maintenanceBootStartedMs = 0;
    // Immutable maintenance session start. Anchors the absolute cap and the
    // boot-time diagnostics; never moves for the life of the session.
    unsigned long maintenanceBootSessionStartedMs = 0;
    // Last maintenance-loop observation of UI activity, latched from
    // WiFiManager::isUiActive(). 0 means "no UI request seen this session".
    unsigned long maintenanceLastUiActivityMs = 0;
    unsigned long lastLoopUs = 0;
};
