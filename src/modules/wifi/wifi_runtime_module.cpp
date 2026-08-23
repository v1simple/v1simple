#include "wifi_runtime_module.h"

void WifiRuntimeModule::begin(const Providers& hooks) {
    providers = hooks;
}

void WifiRuntimeModule::process(const WifiRuntimeContext& ctx) {
    const bool allowTransitionWork = !ctx.skipLateNonCoreThisLoop && !ctx.overloadLateThisLoop &&
                                     !ctx.bleBackpressure && !ctx.bleConnectBurstSettling;

    if (providers.setWifiTransitionAdmission) {
        providers.setWifiTransitionAdmission(providers.wifiTransitionAdmissionContext, allowTransitionWork);
    }

    const bool lifecyclePending =
        providers.readWifiLifecyclePending && providers.readWifiLifecyclePending(providers.wifiLifecycleContext);
    const bool allowWifiProcess = !ctx.skipLateNonCoreThisLoop && (allowTransitionWork || lifecyclePending);

    if (allowWifiProcess && providers.shouldRunWifiProcessingPolicy &&
        providers.shouldRunWifiProcessingPolicy(providers.wifiPolicyContext) && providers.runWifiCadence &&
        providers.runWifiManagerProcess) {
        WifiProcessCadenceContext wifiCadenceCtx;
        if (providers.perfTimestampUs) {
            wifiCadenceCtx.nowProcessUs = providers.perfTimestampUs(providers.perfContext);
        }
        wifiCadenceCtx.minIntervalUs = WIFI_PROCESS_MIN_INTERVAL_US;
        const WifiProcessCadenceDecision wifiCadenceDecision =
            providers.runWifiCadence(providers.wifiCadenceContext, wifiCadenceCtx);
        if (wifiCadenceDecision.shouldRunProcess) {
            if (providers.perfTimestampUs && providers.recordWifiProcessUs) {
                const uint32_t wifiStartUs = providers.perfTimestampUs(providers.perfContext);
                providers.runWifiManagerProcess(providers.wifiManagerProcessContext);
                providers.recordWifiProcessUs(providers.wifiProcessPerfContext,
                                              providers.perfTimestampUs(providers.perfContext) - wifiStartUs);
            } else {
                providers.runWifiManagerProcess(providers.wifiManagerProcessContext);
            }
        }
    }

    if (allowTransitionWork && providers.runWifiVisualSync && providers.readWifiServiceActive &&
        providers.readWifiConnected) {
        const bool wifiVisualActiveNow = providers.readWifiServiceActive(providers.wifiServiceContext) ||
                                         providers.readWifiConnected(providers.wifiConnectedContext);
        uint32_t visualNowMs = ctx.nowMs;
        if (providers.readVisualNowMs) {
            visualNowMs = providers.readVisualNowMs(providers.visualNowContext);
        }
        providers.runWifiVisualSync(providers.wifiVisualSyncContext, visualNowMs, wifiVisualActiveNow,
                                    ctx.displayPreviewRunning, ctx.bootSplashHoldActive);
    }
}
