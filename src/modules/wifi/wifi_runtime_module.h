#pragma once

#include <stdint.h>

#include "wifi_process_cadence_module.h"

struct WifiRuntimeContext {
    uint32_t nowMs = 0;
    uint32_t v1ConnectedAtMs = 0;
    bool enableWifi = true;
    bool bleConnected = false;
    bool canStartDma = false;
    bool wifiAutoStartAllowed = false;
    bool wifiAutoStartDone = false;
    bool wifiManualStartIntentLatched = false;
    bool skipLateNonCoreThisLoop = false;
    bool bleBackpressure = false;
    bool overloadLateThisLoop = false;
    bool bleConnectBurstSettling = false;
    bool displayPreviewRunning = false;
    bool bootSplashHoldActive = false;
};

struct WifiRuntimeResult {
    bool wifiAutoStartDone = false;
    bool wifiManualStartIntentLatched = false;
};

// Orchestrates deferred WiFi auto-start, process cadence, and visual sync.
class WifiRuntimeModule {
public:
    struct Providers {
        void (*runWifiAutoStartProcess)(void* ctx,
                                        uint32_t nowMs,
                                        uint32_t v1ConnectedAtMs,
                                        bool enableWifi,
                                        bool bleConnected,
                                        bool canStartDma,
                                        bool wifiAutoStartAllowed,
                                        bool& wifiManualStartIntentLatched,
                                        bool& wifiAutoStartDone) = nullptr;
        void* wifiAutoStartContext = nullptr;

        bool (*shouldRunWifiProcessingPolicy)(void* ctx) = nullptr;
        void* wifiPolicyContext = nullptr;
        bool (*readWifiLifecyclePending)(void* ctx) = nullptr;
        void* wifiLifecycleContext = nullptr;

        uint32_t (*perfTimestampUs)(void* ctx) = nullptr;
        void* perfContext = nullptr;
        WifiProcessCadenceDecision (*runWifiCadence)(
            void* ctx, const WifiProcessCadenceContext& cadenceCtx) = nullptr;
        void* wifiCadenceContext = nullptr;
        void (*setWifiTransitionAdmission)(void* ctx, bool allowTransitionWork) = nullptr;
        void* wifiTransitionAdmissionContext = nullptr;
        void (*runWifiManagerProcess)(void* ctx) = nullptr;
        void* wifiManagerProcessContext = nullptr;
        void (*recordWifiProcessUs)(void* ctx, uint32_t elapsedUs) = nullptr;
        void* wifiProcessPerfContext = nullptr;

        bool (*readWifiServiceActive)(void* ctx) = nullptr;
        void* wifiServiceContext = nullptr;
        bool (*readWifiConnected)(void* ctx) = nullptr;
        void* wifiConnectedContext = nullptr;
        uint32_t (*readVisualNowMs)(void* ctx) = nullptr;
        void* visualNowContext = nullptr;
        void (*runWifiVisualSync)(void* ctx,
                                  uint32_t nowMs,
                                  bool wifiVisualActiveNow,
                                  bool displayPreviewRunning,
                                  bool bootSplashHoldActive) = nullptr;
        void* wifiVisualSyncContext = nullptr;
    };

    void begin(const Providers& hooks);
    WifiRuntimeResult process(const WifiRuntimeContext& ctx);

private:
    static constexpr uint32_t WIFI_PROCESS_MIN_INTERVAL_US = 2000;
    Providers providers{};
};
