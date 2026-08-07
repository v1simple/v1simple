#include "wifi_auto_timeout_module.h"

WifiAutoTimeoutResult WifiAutoTimeoutModule::evaluate(const WifiAutoTimeoutInput& input) const {
    WifiAutoTimeoutResult result;
    result.timeoutEnabled = (input.timeoutMins != 0);
    if (!result.timeoutEnabled || !input.setupModeActive) {
        return result;
    }
    if (input.maintenanceBootMode) {
        result.maintenanceSuppressed = true;
        return result;
    }

    result.timeoutMs = static_cast<unsigned long>(input.timeoutMins) * 60UL * 1000UL;
    result.lastActivityMs =
        (input.lastClientSeenMs > input.lastUiActivityMs) ? input.lastClientSeenMs : input.lastUiActivityMs;

    result.timeoutElapsed = (input.nowMs - input.setupModeStartMs) >= result.timeoutMs;
    result.inactiveEnough = (result.lastActivityMs == 0)
                                ? ((input.nowMs - input.setupModeStartMs) >= input.inactivityGraceMs)
                                : ((input.nowMs - result.lastActivityMs) >= input.inactivityGraceMs);
    result.shouldStop = result.timeoutElapsed && result.inactiveEnough && (input.staCount == 0);
    return result;
}

WifiNoClientTimeoutResult WifiAutoTimeoutModule::evaluateNoClient(const WifiNoClientTimeoutInput& input) const {
    WifiNoClientTimeoutResult result;
    if (input.clientPresent || input.staConnectInProgress) {
        result.refreshLastSeen = true;
        return result;
    }
    if (input.maintenanceBootMode) {
        result.refreshLastSeen = true;
        result.maintenanceSuppressed = true;
        return result;
    }
    if (input.lastAnyClientSeenMs == 0) {
        return result;
    }

    result.timeoutMs = input.autoStarted ? input.autoTimeoutMs : input.manualTimeoutMs;
    result.shouldStop = result.timeoutMs > 0 && (input.nowMs - input.lastAnyClientSeenMs) >= result.timeoutMs;
    return result;
}
