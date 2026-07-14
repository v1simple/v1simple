#include "connection_state_dispatch_module.h"

void ConnectionStateDispatchModule::begin(const Providers& hooks) {
    providers = hooks;
    reset();
}

void ConnectionStateDispatchModule::reset() {
    lastProcessRunMs_ = 0;
    hasRunProcess_ = false;
}

ConnectionStateDispatchDecision ConnectionStateDispatchModule::process(const ConnectionStateDispatchContext& ctx) {
    ConnectionStateDispatchDecision decision;

    if (providers.runCadence) {
        ConnectionStateCadenceContext cadenceCtx;
        cadenceCtx.nowMs = ctx.nowMs;
        cadenceCtx.displayUpdateIntervalMs = ctx.displayUpdateIntervalMs;
        cadenceCtx.scanScreenDwellMs = ctx.scanScreenDwellMs;
        cadenceCtx.bleConnectedNow = ctx.bleConnectedNow;
        cadenceCtx.bootSplashHoldActive = ctx.bootSplashHoldActive;
        cadenceCtx.displayPreviewRunning = ctx.displayPreviewRunning;
        decision.cadence = providers.runCadence(providers.cadenceContext, cadenceCtx);
    }

    bool shouldRunConnectionStateProcess = decision.cadence.shouldRunConnectionStateProcess;
    const bool watchdogEligible = !ctx.bootSplashHoldActive && !ctx.displayPreviewRunning;
    if (hasRunProcess_) {
        decision.elapsedSinceLastProcessMs = static_cast<uint32_t>(ctx.nowMs - lastProcessRunMs_);
    }
    if (!shouldRunConnectionStateProcess && watchdogEligible && hasRunProcess_ && ctx.maxProcessGapMs > 0) {
        const uint32_t elapsedSinceProcessMs = decision.elapsedSinceLastProcessMs;
        if (elapsedSinceProcessMs >= ctx.maxProcessGapMs) {
            shouldRunConnectionStateProcess = true;
            decision.watchdogForced = true;
        }
    }

    if (shouldRunConnectionStateProcess && providers.runConnectionStateProcess) {
        providers.runConnectionStateProcess(providers.connectionStateContext, ctx.nowMs);
        decision.ranConnectionStateProcess = true;
        lastProcessRunMs_ = ctx.nowMs;
        hasRunProcess_ = true;
    }

    if (providers.recordDecision) {
        providers.recordDecision(providers.decisionContext, decision);
    }

    return decision;
}
