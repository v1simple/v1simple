#pragma once

#include <Arduino.h>

// Owns WiFi icon refresh cadence/state so main loop keeps no static UI state.
class WifiVisualSyncModule {
public:
    void reset();

    void process(unsigned long nowMs,
                 bool wifiVisualActiveNow,
                 bool displayPreviewRunning,
                 bool bootSplashHoldActive,
                 void (*drawAndFlush)(void* ctx),
                 void* ctx);

private:
    bool lastWifiVisualActive_ = false;
    unsigned long lastWifiIconRefreshMs_ = 0;
};
