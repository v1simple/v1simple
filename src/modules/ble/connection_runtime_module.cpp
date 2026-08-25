#include "connection_runtime_module.h"

#include "ble_client.h"
#include "modules/ble/ble_queue_module.h"

void ConnectionRuntimeModule::begin(V1BLEClient& ble, BleQueueModule& queue) {
    begin(ble, queue, Config{});
}

void ConnectionRuntimeModule::begin(V1BLEClient& ble, BleQueueModule& queue, const Config& cfg) {
    ble_ = &ble;
    queue_ = &queue;
    config_ = cfg;
    reset();
}

void ConnectionRuntimeModule::reset() {
    lastTickUs_ = 0;
    runStartLogged_ = false;
}

ConnectionRuntimeSnapshot ConnectionRuntimeModule::process(unsigned long nowMs, unsigned long nowUs,
                                                           unsigned long lastLoopUs, bool bootSplashHoldActive,
                                                           unsigned long bootSplashHoldUntilMs,
                                                           bool initialScanningScreenShown) {
    ConnectionRuntimeSnapshot snapshot;

    const bool connectedNow = ble_ && ble_->isConnected();

    snapshot.bootSplashHoldActive = bootSplashHoldActive;
    snapshot.initialScanningScreenShown = initialScanningScreenShown;

    if (snapshot.bootSplashHoldActive && static_cast<int32_t>(nowMs - bootSplashHoldUntilMs) >= 0) {
        snapshot.bootSplashHoldActive = false;
        if (!connectedNow) {
            snapshot.requestShowInitialScanning = true;
        } else {
            snapshot.initialScanningScreenShown = true;
        }
    }

    snapshot.connected = connectedNow;

    const unsigned long sinceTickUs = nowUs - lastTickUs_;
    lastTickUs_ = nowUs;

    snapshot.backpressured = queue_ && queue_->isBackpressured();
    snapshot.skipNonCore = (sinceTickUs > config_.tickGapMaxUs) || snapshot.backpressured;
    snapshot.overloaded = (lastLoopUs >= config_.overloadLoopUs) || snapshot.skipNonCore;

    const unsigned long lastRxMs = queue_ ? queue_->getLastRxMillis() : 0;
    snapshot.receiving = lastRxMs != 0 && (nowMs - lastRxMs) < config_.receivingHeartbeatMs;

    if (!runStartLogged_) {
        const bool bleReady = snapshot.connected;
        const bool timeReady = (nowMs >= config_.runStartTimeoutMs);
        if (bleReady || timeReady) {
            runStartLogged_ = true;
            const char* trigger = bleReady ? "ble_connected" : "timeout_30s";
            Serial.printf("RUN_START trigger=%s millis=%lu\n", trigger, nowMs);
        }
    }

    return snapshot;
}
