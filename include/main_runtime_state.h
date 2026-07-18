#pragma once

namespace MainRuntimePolicy {
constexpr unsigned long MaintenanceBootTimeoutMs = 10UL * 60UL * 1000UL;
}

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
    unsigned long maintenanceBootStartedMs = 0;
    unsigned long lastLoopUs = 0;
};
