#include "loop_connection_early_module.h"

void LoopConnectionEarlyModule::begin(const Providers& hooks) {
    providers = hooks;
}

LoopConnectionEarlyResult LoopConnectionEarlyModule::process(const LoopConnectionEarlyContext& ctx) {
    ConnectionRuntimeSnapshot snapshot;
    if (providers.runConnectionRuntime) {
        snapshot = providers.runConnectionRuntime(
            providers.connectionRuntimeContext,
            ctx.nowMs,
            ctx.nowUs,
            ctx.lastLoopUs,
            ctx.bootSplashHoldActive,
            ctx.bootSplashHoldUntilMs,
            ctx.initialScanningScreenShown);
    }

    LoopConnectionEarlyResult result;
    result.bootSplashHoldActive = snapshot.bootSplashHoldActive;
    result.initialScanningScreenShown = snapshot.initialScanningScreenShown;
    result.bleConnectedNow = snapshot.connected;
    result.bleBackpressure = snapshot.backpressured;
    result.skipNonCoreThisLoop = snapshot.skipNonCore;
    result.overloadThisLoop = snapshot.overloaded;
    result.bleReceiving = snapshot.receiving;

    if (snapshot.requestShowInitialScanning) {
        if (providers.showInitialScanning) {
            providers.showInitialScanning(providers.scanningContext);
        }
        // Preserve single-shot behavior when the request is emitted.
        result.initialScanningScreenShown = true;
    }

    DisplayOrchestrationEarlyContext displayEarlyCtx;
    displayEarlyCtx.nowMs = ctx.nowMs;
    displayEarlyCtx.bootSplashHoldActive = result.bootSplashHoldActive;
    displayEarlyCtx.overloadThisLoop = result.overloadThisLoop;
    displayEarlyCtx.bleContext = {
        result.bleConnectedNow,
        providers.readProxyConnected ? providers.readProxyConnected(providers.proxyConnectedContext) : false,
        providers.readConnectionRssi ? providers.readConnectionRssi(providers.connectionRssiContext) : 0,
        providers.readProxyRssi ? providers.readProxyRssi(providers.proxyRssiContext) : 0,
    };
    displayEarlyCtx.bleReceiving = result.bleReceiving;

    if (providers.runDisplayEarly) {
        providers.runDisplayEarly(providers.displayEarlyContext, displayEarlyCtx);
    }

    return result;
}
