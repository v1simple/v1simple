#pragma once

#include <Arduino.h>

struct ConnectionStateCadenceContext {
    unsigned long nowMs = 0;
    unsigned long displayUpdateIntervalMs = 50;
    unsigned long scanScreenDwellMs = 0;
    bool bleConnectedNow = false;
    bool bootSplashHoldActive = false;
    bool displayPreviewRunning = false;
};

struct ConnectionStateCadenceDecision {
    bool displayUpdateDue = false;
    bool holdScanDwell = false;
    bool shouldRunConnectionStateProcess = false;
};

// Governs when connectionStateModule.process() is allowed to run in loop().
class ConnectionStateCadenceModule {
public:
    void reset();
    void onScanningScreenShown(unsigned long nowMs);
    ConnectionStateCadenceDecision process(const ConnectionStateCadenceContext& ctx);

private:
    unsigned long lastDisplayUpdateMs_ = 0;
    unsigned long scanScreenEnteredMs_ = 0;
    bool scanScreenDwellActive_ = false;
    bool lastBleConnectedForScanDwell_ = false;
};
