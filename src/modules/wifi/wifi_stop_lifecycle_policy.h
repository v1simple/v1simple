#pragma once

#include <stdint.h>

namespace WifiStopLifecyclePolicy {

enum class RequestAction : uint8_t {
    REJECT = 0,
    ACKNOWLEDGE_PENDING,
    START_STAGED,
    STOP_IMMEDIATE,
};

struct RequestInput {
    bool setupModeApOn = false;
    bool setupModeStopping = false;
    bool stopAlreadyPending = false;
    bool manual = false;
    const char* reason = nullptr;
};

struct RequestDecision {
    RequestAction action = RequestAction::REJECT;
    const char* reason = nullptr;
    bool emergencyLowDma = false;
    bool escalatesPendingStop = false;
};

constexpr bool reasonEquals(const char* lhs, const char* rhs) {
    if (!lhs || !rhs) {
        return false;
    }
    while (*lhs != '\0' && *rhs != '\0') {
        if (*lhs != *rhs) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return *lhs == *rhs;
}

constexpr const char* normalizeReason(const char* reason, bool manual) {
    return (reason && reason[0] != '\0') ? reason : (manual ? "manual" : "unknown");
}

constexpr RequestDecision evaluateRequest(const RequestInput& input) {
    if (!input.setupModeApOn && !input.setupModeStopping) {
        return {};
    }

    const char* reason = normalizeReason(input.reason, input.manual);
    const bool emergencyLowDma = reasonEquals(reason, "low_dma");
    const bool forceImmediate = emergencyLowDma || reasonEquals(reason, "poweroff");

    if (input.stopAlreadyPending && !forceImmediate) {
        return {RequestAction::ACKNOWLEDGE_PENDING, reason, false, false};
    }
    if (forceImmediate) {
        return {RequestAction::STOP_IMMEDIATE, reason, emergencyLowDma, input.stopAlreadyPending};
    }
    return {RequestAction::START_STAGED, reason, false, false};
}

struct PhaseInput {
    bool idle = true;
    bool stopHttpServer = false;
    uint32_t nowMs = 0;
    uint32_t phaseStartMs = 0;
    uint32_t settleMs = 0;
};

constexpr bool shouldExecutePhase(const PhaseInput& input) {
    if (input.idle) {
        return false;
    }
    if (!input.stopHttpServer && (input.nowMs - input.phaseStartMs) < input.settleMs) {
        return false;
    }
    return true;
}

} // namespace WifiStopLifecyclePolicy
