#include "wifi_auto_timeout_module.h"

WifiAutoTimeoutResult WifiAutoTimeoutModule::evaluate(const WifiAutoTimeoutInput& input) const {
    WifiAutoTimeoutResult result;
    result.timeoutEnabled = (input.timeoutMins != 0);
    if (!result.timeoutEnabled || !input.setupModeActive) {
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
