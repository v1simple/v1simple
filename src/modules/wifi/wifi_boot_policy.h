#pragma once

#include <stdint.h>

// ─── WiFi boot-start gating policy (pure functions, no RTOS deps) ───
// Mirrors the runtime logic in main.cpp so the decisions can be
// verified in native tests without hardware.

namespace WifiBootPolicy {

/// Should a deferred WiFi start proceed this loop iteration?
///
/// Returns true exactly when ALL conditions are met:
///   1. WiFi hasn't already been started (done == false)
///   2. Either:
///      a. BLE connected AND settle window elapsed, OR
///      b. Boot timeout exceeded (user may need web UI to diagnose)
///   3. DMA heap check passes (canStartDma == true)
///
/// Caller is responsible for tracking the "done" flag and ensuring
/// this is called at most once per loop iteration.
inline bool shouldAutoStartWifi(bool alreadyStarted,
                                bool bleConnected,
                                uint32_t msSinceV1Connect,
                                uint32_t settleMs,
                                uint32_t msSinceBoot,
                                uint32_t bootTimeoutMs,
                                bool canStartDma) {
    if (alreadyStarted) return false;

    // BLE connected path: wait for settle window.
    bool bleSettled = bleConnected && (msSinceV1Connect >= settleMs);

    // Timeout path: allow WiFi even without BLE after bootTimeoutMs.
    bool timeout = (msSinceBoot >= bootTimeoutMs);

    if (!bleSettled && !timeout) return false;

    return canStartDma;
}

#ifdef UNIT_TEST
/// Should wifiManager.process() be called this iteration?
///
/// Returns false when WiFi is entirely off — prevents stray work that
/// shows up as wifiMax_us / wifiConnectDeferred in wifi-off profiles.
/// NOTE: Not called in production code; retained for test coverage.
inline bool shouldProcessWifi(bool setupModeActive,
                              bool wifiClientConnected,
                              bool wifiAutoStartDone) {
    (void)wifiAutoStartDone;
    // If AP or STA is active, always process.
    if (setupModeActive) return true;
    if (wifiClientConnected) return true;

    return false;
}
#endif // UNIT_TEST

}  // namespace WifiBootPolicy
