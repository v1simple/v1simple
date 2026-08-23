#pragma once

#include <stdint.h>

#include "wifi_process_cadence_module.h"

struct WifiRuntimeContext {
    uint32_t nowMs = 0;
    bool skipLateNonCoreThisLoop = false;
    bool bleBackpressure = false;
    bool overloadLateThisLoop = false;
    bool bleConnectBurstSettling = false;
    bool displayPreviewRunning = false;
    bool bootSplashHoldActive = false;
};

// Orchestrates WiFi process cadence and visual sync. Normal runtime never
// starts WiFi: maintenance entry reboots first, so there is no start path here.
class WifiRuntimeModule {
  public:
    struct Providers {
        bool (*shouldRunWifiProcessingPolicy)(void* ctx) = nullptr;
        void* wifiPolicyContext = nullptr;
        bool (*readWifiLifecyclePending)(void* ctx) = nullptr;
        void* wifiLifecycleContext = nullptr;

        uint32_t (*perfTimestampUs)(void* ctx) = nullptr;
        void* perfContext = nullptr;
        WifiProcessCadenceDecision (*runWifiCadence)(void* ctx, const WifiProcessCadenceContext& cadenceCtx) = nullptr;
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
        void (*runWifiVisualSync)(void* ctx, uint32_t nowMs, bool wifiVisualActiveNow, bool displayPreviewRunning,
                                  bool bootSplashHoldActive) = nullptr;
        void* wifiVisualSyncContext = nullptr;
    };

    void begin(const Providers& hooks);
    void process(const WifiRuntimeContext& ctx);

  private:
    static constexpr uint32_t WIFI_PROCESS_MIN_INTERVAL_US = 2000;
    Providers providers{};
};
