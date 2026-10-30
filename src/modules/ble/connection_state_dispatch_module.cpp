#include "connection_state_dispatch_module.h"

#include "modules/ble/connection_state_module.h"

void ConnectionStateDispatchModule::begin(ConnectionStateCadenceModule& cadence,
                                          ConnectionStateModule& connectionState) {
    cadence_ = &cadence;
    connectionState_ = &connectionState;
    reset();
}

void ConnectionStateDispatchModule::reset() {
    lastProcessRunMs_ = 0;
    hasRunProcess_ = false;
}

void ConnectionStateDispatchModule::process(const ConnectionStateDispatchContext& ctx) {
    bool shouldRunConnectionStateProcess = false;

    if (cadence_) {
        ConnectionStateCadenceContext cadenceCtx;
        cadenceCtx.nowMs = ctx.nowMs;
        cadenceCtx.displayUpdateIntervalMs = ctx.displayUpdateIntervalMs;
        cadenceCtx.scanScreenDwellMs = ctx.scanScreenDwellMs;
        cadenceCtx.bleConnectedNow = ctx.bleConnectedNow;
        cadenceCtx.bootSplashHoldActive = ctx.bootSplashHoldActive;
        cadenceCtx.displayPreviewRunning = ctx.displayPreviewRunning;
        shouldRunConnectionStateProcess = cadence_->process(cadenceCtx).shouldRunConnectionStateProcess;
    }

    const bool watchdogEligible = !ctx.bootSplashHoldActive && !ctx.displayPreviewRunning;
    if (!shouldRunConnectionStateProcess && watchdogEligible && hasRunProcess_ && ctx.maxProcessGapMs > 0) {
        const uint32_t elapsedSinceProcessMs = static_cast<uint32_t>(ctx.nowMs - lastProcessRunMs_);
        if (elapsedSinceProcessMs >= ctx.maxProcessGapMs) {
            shouldRunConnectionStateProcess = true;
        }
    }

    if (shouldRunConnectionStateProcess && connectionState_) {
        connectionState_->process(ctx.nowMs);
        lastProcessRunMs_ = ctx.nowMs;
        hasRunProcess_ = true;
    }

}
