#pragma once

#include <stdint.h>

#include "../obd/obd_ble_arbitration.h"
#include "../obd/obd_runtime_module.h"

class ConnectionCycleLifecycle {
  public:
    virtual ~ConnectionCycleLifecycle() = default;
    virtual void stopObdScan() = 0;
    virtual void cancelObdConnect() = 0;
    virtual void stopProxyAdvertising() = 0;
    virtual void disconnectProxyPhones() = 0;
    virtual bool isObdScanStopped() const = 0;
    virtual bool isObdConnectIdle() const = 0;
    virtual bool isProxyFullyStopped() const = 0;
};

enum class CycleState : uint8_t {
    SCAN_V1 = 0,
    V1_SETTLING = 1,
    OBD_SCAN = 2,
    OBD_CONNECT = 3,
    OBD_SETTLED = 4,
    PROXY_OPEN = 5,
    STEADY = 6,
    TEARDOWN = 7,
};

struct CycleContext {
    uint32_t nowMs = 0;
    bool bootReady = false;
    bool v1GattConnected = false;
    bool autoPushEnabled = false;
    bool v1VerifyPushMatchEdge = false;
    uint32_t v1LastEventMs = 0;
    bool obdEnabled = false;
    bool obdSavedAddressValid = false;
    bool obdConnected = false;
    ObdConnectionState obdState = ObdConnectionState::IDLE;
    bool obdHasValidSpeedSample = false;
    bool proxyEnabled = false;
    bool proxyAdvertising = false;
    bool proxyClientConnected = false;
    bool proxyClientConnectedOnceThisBoot = false;
    uint32_t obdScanWindowMs = 0;
    uint32_t obdRetryIntervalMs = 0;
    uint32_t proxyOpenWindowMs = 0;
    uint32_t v1SettleQuietMs = 0;
    uint32_t v1SettleFallbackMs = 0;
    uint32_t cycleTeardownAckTimeoutMs = 0;
};

class ConnectionCycleCoordinatorModule {
  public:
    void begin(ConnectionCycleLifecycle& lifecycle);
    void reset();
    void update(const CycleContext& ctx);

    bool obdScanAllowed() const;
    bool obdConnectAllowed() const;
    bool obdRetryAllowed(uint32_t nowMs) const;
    bool proxyAdvertisingAllowed() const;
    bool proxyKeepConnectionAllowed() const;

    ObdBleArbitrationRequest arbitrationRequest() const;

    CycleState state() const { return state_; }
    uint32_t timeInStateMs(uint32_t nowMs) const;
    uint32_t totalTransitionCount() const { return totalTransitionCount_; }
    uint32_t lastTeardownDurationMs() const { return lastTeardownDurationMs_; }
    uint32_t totalObdRetryAttempts() const { return totalObdRetryAttempts_; }
    void recordObdRetryAttempt(uint32_t nowMs);

  private:
    enum class TeardownStep : uint8_t {
        Idle = 0,
        WaitObdStop = 1,
        WaitProxyStop = 2,
    };

    void transitionTo(CycleState newState, uint32_t nowMs);
    void enterTeardown(uint32_t nowMs);
    void updateTeardown(uint32_t nowMs);
    void updateTimingConfig(const CycleContext& ctx);
    ConnectionCycleLifecycle* lifecycle_ = nullptr;
    CycleState state_ = CycleState::SCAN_V1;
    uint32_t stateEnteredMs_ = 0;
    bool stateEnteredMsValid_ = false;
    uint32_t totalTransitionCount_ = 0;
    uint32_t lastObdAttemptMs_ = 0;
    uint32_t teardownStepStartedMs_ = 0;
    TeardownStep teardownStep_ = TeardownStep::Idle;
    bool wasV1Connected_ = false;
    bool lastProxyClientConnected_ = false;
    bool lastV1Connected_ = false;
    bool v1VerifyPushMatched_ = false;
    uint32_t v1VerifyPushMatchedAtMs_ = 0;
    uint32_t lastTeardownDurationMs_ = 0;
    uint32_t totalObdRetryAttempts_ = 0;
    uint32_t obdScanWindowMs_ = 0;
    uint32_t obdRetryIntervalMs_ = 0;
    uint32_t proxyOpenWindowMs_ = 0;
    uint32_t v1SettleQuietMs_ = 0;
    uint32_t v1SettleFallbackMs_ = 0;
    uint32_t teardownAckTimeoutMs_ = 0;
};
