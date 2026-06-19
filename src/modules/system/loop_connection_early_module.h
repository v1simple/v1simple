#pragma once

#include <stdint.h>

#include "modules/ble/connection_runtime_module.h"
#include "modules/display/display_orchestration_module.h"

struct LoopConnectionEarlyContext {
    uint32_t nowMs = 0;
    uint32_t nowUs = 0;
    uint32_t lastLoopUs = 0;
    bool bootSplashHoldActive = false;
    uint32_t bootSplashHoldUntilMs = 0;
    bool initialScanningScreenShown = false;
};

struct LoopConnectionEarlyResult {
    bool bootSplashHoldActive = false;
    bool initialScanningScreenShown = false;

    bool bleConnectedNow = false;
    bool bleBackpressure = false;
    bool skipNonCoreThisLoop = false;
    bool overloadThisLoop = false;
    bool bleReceiving = false;
};

// Orchestrates early loop connection/runtime state and display early sync.
class LoopConnectionEarlyModule {
public:
    struct Providers {
        ConnectionRuntimeSnapshot (*runConnectionRuntime)(
            void* ctx,
            uint32_t nowMs,
            uint32_t nowUs,
            uint32_t lastLoopUs,
            bool bootSplashHoldActive,
            uint32_t bootSplashHoldUntilMs,
            bool initialScanningScreenShown) = nullptr;
        void* connectionRuntimeContext = nullptr;

        void (*showInitialScanning)(void* ctx) = nullptr;
        void* scanningContext = nullptr;

        bool (*readProxyConnected)(void* ctx) = nullptr;
        void* proxyConnectedContext = nullptr;
        int (*readConnectionRssi)(void* ctx) = nullptr;
        void* connectionRssiContext = nullptr;
        int (*readProxyRssi)(void* ctx) = nullptr;
        void* proxyRssiContext = nullptr;

        void (*runDisplayEarly)(void* ctx, const DisplayOrchestrationEarlyContext& displayEarlyCtx) = nullptr;
        void* displayEarlyContext = nullptr;
    };

    void begin(const Providers& hooks);
    LoopConnectionEarlyResult process(const LoopConnectionEarlyContext& ctx);

private:
    Providers providers{};
};
