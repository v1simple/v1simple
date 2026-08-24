#pragma once

#include <stdint.h>

namespace MainRuntimePolicy {

// Maintenance sessions use a ten-minute idle window and a thirty-minute
// absolute limit. UI requests extend the idle deadline; the absolute limit
// ensures that background polling from an unattended browser cannot keep the
// device out of normal runtime indefinitely.
//
// WiFiManager provides recent activity as a predicate rather than a timestamp.
// The main loop samples that predicate and records the observation time used by
// this policy. Request-rate heuristics are unsuitable because page loads,
// per-page polling, and multiple tabs all change the request cadence.

// Base maintenance-session budget and idle window.
constexpr unsigned long MaintenanceBootTimeoutMs = 10UL * 60UL * 1000UL;

// Hard ceiling on total session length, measured from session start. Never
// reset. No amount of UI activity can push a session past this.
constexpr unsigned long MaintenanceBootMaxSessionMs = 30UL * 60UL * 1000UL;

// Sampling window for WiFiManager's recent-activity predicate.
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
    // True when UI activity has pushed the deadline past the original
    // elapsed-since-start deadline.
    bool extended = false;
    uint32_t elapsedSinceStartMs = 0;
    uint32_t elapsedSinceActivityMs = 0;
    uint32_t remainingMs = 0;
    // Anchor T for which remainingMs == idleWindowMs - (nowMs - T). The main
    // loop republishes it through /api/status so the UI countdown reflects
    // idle extensions and the absolute cap without changing the response
    // schema. For an extended session, the reported "uptime" means elapsed
    // countdown budget rather than literal session age. A live session never
    // returns 0 because the status payload reserves it as a sentinel.
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
        // The status payload uses zero remaining time and a zero anchor to
        // represent an inactive maintenance session.
        return decision;
    }

    decision.elapsedSinceStartMs = static_cast<uint32_t>(input.nowMs - input.sessionStartedMs);

    uint32_t elapsedSinceActivity = decision.elapsedSinceStartMs;
    if (input.lastUiActivityMs != 0) {
        elapsedSinceActivity = static_cast<uint32_t>(input.nowMs - input.lastUiActivityMs);
    }
    // Reject stale and racing samples. Unsigned subtraction maps a future
    // timestamp to a large elapsed value, so the same clamp handles both.
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
    bool maintenanceBootActive = false;
    // Countdown anchor published through /api/status; not the immutable
    // session start.
    unsigned long maintenanceBootStartedMs = 0;
    // Immutable maintenance session start. Anchors the absolute cap and never
    // moves for the life of the session.
    unsigned long maintenanceBootSessionStartedMs = 0;
    // Last maintenance-loop observation of UI activity, latched from
    // WiFiManager::isUiActive(). 0 means "no UI request seen this session".
    unsigned long maintenanceLastUiActivityMs = 0;
    unsigned long lastLoopUs = 0;
};
