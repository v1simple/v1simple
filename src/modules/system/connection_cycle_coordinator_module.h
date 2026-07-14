#pragma once

#include <stdint.h>

#include "../obd/obd_ble_arbitration.h"
#include "../obd/obd_runtime_module.h"

enum class CycleState : uint8_t {
    SCAN_V1 = 0,
    V1_SETTLING = 1,
    OBD_SCAN = 2,
    OBD_CONNECT = 3,
    OBD_SETTLED = 4,
    PROXY_OPEN = 5,
    WIFI_OPEN = 6,
    STEADY = 7,
    TEARDOWN = 8,
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
    bool wifiEnabled = false;
    bool wifiActive = false;
    bool wifiManualStartIntentLatched = false;
    uint32_t obdScanWindowMs = 0;
    uint32_t obdRetryIntervalMs = 0;
    uint32_t proxyOpenWindowMs = 0;
    uint32_t wifiOpenTimeoutMs = 0;
    uint32_t v1SettleQuietMs = 0;
    uint32_t v1SettleFallbackMs = 0;
    uint32_t cycleTeardownAckTimeoutMs = 0;
};

class ConnectionCycleCoordinatorModule {
  public:
    struct Providers {
        void (*stopObdScan)(void* ctx) = nullptr;
        void* stopObdScanContext = nullptr;

        void (*cancelObdConnect)(void* ctx) = nullptr;
        void* cancelObdConnectContext = nullptr;

        void (*stopProxyAdvertising)(void* ctx) = nullptr;
        void* stopProxyAdvertisingContext = nullptr;

        void (*disconnectProxyPhone)(void* ctx) = nullptr;
        void* disconnectProxyPhoneContext = nullptr;

        bool (*isObdScanStopped)(void* ctx) = nullptr;
        void* isObdScanStoppedContext = nullptr;

        bool (*isObdConnectIdle)(void* ctx) = nullptr;
        void* isObdConnectIdleContext = nullptr;

        bool (*isProxyFullyStopped)(void* ctx) = nullptr;
        void* isProxyFullyStoppedContext = nullptr;
    };

    void begin(const Providers& hooks);
    void reset();
    void update(const CycleContext& ctx);

    bool obdScanAllowed() const;
    bool obdConnectAllowed() const;
    bool obdRetryAllowed(uint32_t nowMs) const;
    bool proxyAdvertisingAllowed() const;
    bool proxyKeepConnectionAllowed() const;
    bool wifiAutoStartAllowed() const;
    bool shouldPreemptProxyForManualWifiStart() const;

    ObdBleArbitrationRequest arbitrationRequest() const;

    CycleState state() const { return state_; }
    uint32_t timeInStateMs(uint32_t nowMs) const;
    uint32_t totalTransitionCount() const { return totalTransitionCount_; }
    uint32_t lastTeardownDurationMs() const { return lastTeardownDurationMs_; }
    uint32_t totalObdRetryAttempts() const { return totalObdRetryAttempts_; }
    uint32_t totalWifiManualPhoneKicks() const { return totalWifiManualPhoneKicks_; }
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

    Providers providers{};
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
    bool manualWifiPreemptRequested_ = false;
    uint32_t lastTeardownDurationMs_ = 0;
    uint32_t totalObdRetryAttempts_ = 0;
    uint32_t totalWifiManualPhoneKicks_ = 0;
    uint32_t obdScanWindowMs_ = 0;
    uint32_t obdRetryIntervalMs_ = 0;
    uint32_t proxyOpenWindowMs_ = 0;
    uint32_t wifiOpenTimeoutMs_ = 0;
    uint32_t v1SettleQuietMs_ = 0;
    uint32_t v1SettleFallbackMs_ = 0;
    uint32_t teardownAckTimeoutMs_ = 0;
};
