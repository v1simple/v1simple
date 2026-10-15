#pragma once

#include <stdint.h>

namespace MainRuntimePolicy {

// UI activity extends the idle window, but browser polling cannot extend the
// absolute session limit. Activity is sampled from WiFiManager's predicate.
constexpr unsigned long MaintenanceBootTimeoutMs = 10UL * 60UL * 1000UL;
constexpr unsigned long MaintenanceBootMaxSessionMs = 30UL * 60UL * 1000UL;
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
    // /api/status countdown anchor: remainingMs == idleWindowMs - (nowMs - T).
    // This may differ from session start; zero is reserved for inactive sessions.
    uint32_t deadlineAnchorMs = 0;
};

// Unsigned elapsed differences preserve deadlines across millis() rollover.
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
    // Countdown anchor published through /api/status.
    unsigned long maintenanceBootStartedMs = 0;
    // Immutable start for the absolute session cap.
    unsigned long maintenanceBootSessionStartedMs = 0;
    // Last sampled UI activity; zero means none observed this session.
    unsigned long maintenanceLastUiActivityMs = 0;
    unsigned long lastLoopUs = 0;
};
