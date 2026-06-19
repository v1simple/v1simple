#pragma once

#include <stdint.h>

struct WifiAutoTimeoutInput {
    uint8_t timeoutMins = 0;
    bool setupModeActive = false;
    unsigned long nowMs = 0;
    unsigned long setupModeStartMs = 0;
    unsigned long lastClientSeenMs = 0;
    unsigned long lastUiActivityMs = 0;
    int staCount = 0;
    unsigned long inactivityGraceMs = 0;
};

struct WifiAutoTimeoutResult {
    bool timeoutEnabled = false;
    unsigned long timeoutMs = 0;
    unsigned long lastActivityMs = 0;
    bool timeoutElapsed = false;
    bool inactiveEnough = false;
    bool shouldStop = false;
};

// Evaluates auto-timeout stop eligibility for WiFi AP mode.
class WifiAutoTimeoutModule {
public:
    WifiAutoTimeoutResult evaluate(const WifiAutoTimeoutInput& input) const;
};
