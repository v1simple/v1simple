#pragma once

#include <stdint.h>

namespace WifiReconnectPolicy {

enum class BootAction : uint8_t {
    DEFER_FOR_V1 = 0,
    PROCEED,
};

struct BootInput {
    bool v1Connected = false;
    bool setupModeStartAnchored = false;
    uint32_t nowMs = 0;
    uint32_t setupModeStartMs = 0;
    uint32_t bootGraceMs = 0;
    bool deferredLogged = false;
};

struct BootDecision {
    BootAction action = BootAction::PROCEED;
    bool deferredLogged = false;
    bool logDeferred = false;
    bool logResumed = false;
};

constexpr BootDecision evaluateBoot(const BootInput& input) {
    const bool withinBootGrace =
        input.setupModeStartAnchored && static_cast<uint32_t>(input.nowMs - input.setupModeStartMs) < input.bootGraceMs;
    if (!input.v1Connected && withinBootGrace) {
        return {
            BootAction::DEFER_FOR_V1,
            true,
            !input.deferredLogged,
            false,
        };
    }
    return {
        BootAction::PROCEED,
        false,
        false,
        input.deferredLogged,
    };
}

enum class AttemptAction : uint8_t {
    WAIT_NO_CREDENTIALS,
    WAIT_AUTO_STARTED_IDLE,
    WAIT_FAILURE_LIMIT,
    WAIT_INTERVAL,
    GIVE_UP,
    ATTEMPT,
};

struct AttemptInput {
    bool credentialsConfigured = false;
    bool autoStarted = false;
    bool apClientPresent = false;
    bool uiActivitySeen = false;
    int failures = 0;
    int maxFailures = 0;
    uint32_t retryNowMs = 0;
    uint32_t lastAttemptMs = 0;
    uint32_t retryIntervalMs = 0;
};

struct AttemptDecision {
    AttemptAction action = AttemptAction::WAIT_NO_CREDENTIALS;
    int nextFailures = 0;
};

constexpr AttemptDecision evaluateAttempt(const AttemptInput& input) {
    AttemptDecision decision;
    decision.nextFailures = input.failures;

    if (!input.credentialsConfigured) {
        decision.action = AttemptAction::WAIT_NO_CREDENTIALS;
        return decision;
    }
    if (input.autoStarted && !input.apClientPresent && !input.uiActivitySeen) {
        decision.action = AttemptAction::WAIT_AUTO_STARTED_IDLE;
        return decision;
    }
    if (input.failures >= input.maxFailures) {
        decision.action = AttemptAction::WAIT_FAILURE_LIMIT;
        return decision;
    }

    const bool retryDue = input.lastAttemptMs == 0 ||
                          static_cast<uint32_t>(input.retryNowMs - input.lastAttemptMs) > input.retryIntervalMs;
    if (!retryDue) {
        decision.action = AttemptAction::WAIT_INTERVAL;
        return decision;
    }

    decision.nextFailures = input.failures + 1;
    decision.action = decision.nextFailures >= input.maxFailures ? AttemptAction::GIVE_UP : AttemptAction::ATTEMPT;
    return decision;
}

} // namespace WifiReconnectPolicy
