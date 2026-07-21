#pragma once

#include <stdint.h>

struct WifiAutoTimeoutInput {
    uint8_t timeoutMins = 0;
    bool setupModeActive = false;
    // Maintenance boots exist to serve the web UI and are already bounded by
    // the maintenance-boot reboot timeout; the idle auto-timeout must never
    // stop WiFi underneath a deliberate session (there is no user-visible
    // recovery besides waiting for that reboot).
    bool maintenanceBootMode = false;
    unsigned long nowMs = 0;
    unsigned long setupModeStartMs = 0;
    unsigned long lastClientSeenMs = 0;
    unsigned long lastUiActivityMs = 0;
    int staCount = 0;
    unsigned long inactivityGraceMs = 0;
};

struct WifiAutoTimeoutResult {
    bool timeoutEnabled = false;
    bool maintenanceSuppressed = false;
    unsigned long timeoutMs = 0;
    unsigned long lastActivityMs = 0;
    bool timeoutElapsed = false;
    bool inactiveEnough = false;
    bool shouldStop = false;
};

struct WifiNoClientTimeoutInput {
    bool maintenanceBootMode = false;
    bool clientPresent = false;
    bool staConnectInProgress = false;
    bool autoStarted = false;
    unsigned long nowMs = 0;
    unsigned long lastAnyClientSeenMs = 0;
    unsigned long manualTimeoutMs = 0;
    unsigned long autoTimeoutMs = 0;
};

struct WifiNoClientTimeoutResult {
    bool refreshLastSeen = false;
    bool maintenanceSuppressed = false;
    unsigned long timeoutMs = 0;
    bool shouldStop = false;
};

// Evaluates auto-timeout stop eligibility for WiFi AP mode.
class WifiAutoTimeoutModule {
  public:
    WifiAutoTimeoutResult evaluate(const WifiAutoTimeoutInput& input) const;
    WifiNoClientTimeoutResult evaluateNoClient(const WifiNoClientTimeoutInput& input) const;
};
