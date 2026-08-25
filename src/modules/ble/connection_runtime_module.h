#pragma once

#include <Arduino.h>

class BleQueueModule;
class V1BLEClient;

struct ConnectionRuntimeSnapshot {
    bool connected = false;
    bool receiving = false;
    bool backpressured = false;
    bool skipNonCore = false;
    bool overloaded = false;
    bool bootSplashHoldActive = false;
    bool initialScanningScreenShown = false;
    bool requestShowInitialScanning = false;
};

class ConnectionRuntimeModule {
  public:
    struct Config {
        unsigned long tickGapMaxUs = 25000;
        unsigned long overloadLoopUs = 25000;
        unsigned long receivingHeartbeatMs = 2000;
        unsigned long runStartTimeoutMs = 30000;
    };

    void begin(V1BLEClient& ble, BleQueueModule& queue);
    void begin(V1BLEClient& ble, BleQueueModule& queue, const Config& cfg);

    ConnectionRuntimeSnapshot process(unsigned long nowMs, unsigned long nowUs, unsigned long lastLoopUs,
                                      bool bootSplashHoldActive, unsigned long bootSplashHoldUntilMs,
                                      bool initialScanningScreenShown);

  private:
    void reset();
    V1BLEClient* ble_ = nullptr;
    BleQueueModule* queue_ = nullptr;
    Config config_;
    unsigned long lastTickUs_ = 0;
    bool runStartLogged_ = false;
};
