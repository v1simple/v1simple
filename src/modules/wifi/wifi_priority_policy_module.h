#pragma once

#include <Arduino.h>

class V1BLEClient;
class WiFiManager;

// Returns true when WiFi processing should run this loop iteration.
bool isWifiProcessingEnabledPolicy(const WiFiManager& wifiManager);

// Manages BLE WiFi-priority transitions with hysteresis/hold semantics.
class WifiPriorityPolicyModule {
public:
    void reset();

    void apply(unsigned long nowMs,
               V1BLEClient& bleClient,
               WiFiManager& wifiManager);

private:
    unsigned long wifiPriorityLastTransitionMs_ = 0;
    bool pendingPriorityValid_ = false;
    bool pendingPriorityState_ = false;
    unsigned long pendingPrioritySinceMs_ = 0;
};
