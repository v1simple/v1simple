#pragma once

#include <Arduino.h>

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

    struct Providers {
        bool (*isBleConnected)(void* ctx) = nullptr;
        bool (*isBackpressured)(void* ctx) = nullptr;
        unsigned long (*getLastRxMillis)(void* ctx) = nullptr;
        void* bleContext = nullptr;
        void* queueContext = nullptr;
    };

    void begin(const Providers& hooks);
    void begin(const Providers& hooks, const Config& cfg);

    ConnectionRuntimeSnapshot process(unsigned long nowMs,
                                      unsigned long nowUs,
                                      unsigned long lastLoopUs,
                                      bool bootSplashHoldActive,
                                      unsigned long bootSplashHoldUntilMs,
                                      bool initialScanningScreenShown);

private:
    void reset();
    Providers providers{};
    Config config_;
    unsigned long lastTickUs_ = 0;
    bool runStartLogged_ = false;
};
