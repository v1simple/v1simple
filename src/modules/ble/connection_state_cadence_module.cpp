#include "connection_state_cadence_module.h"

void ConnectionStateCadenceModule::reset() {
    lastDisplayUpdateMs_ = 0;
    scanScreenEnteredMs_ = 0;
    scanScreenDwellActive_ = false;
    lastBleConnectedForScanDwell_ = false;
}

void ConnectionStateCadenceModule::onScanningScreenShown(unsigned long nowMs) {
    scanScreenEnteredMs_ = nowMs;
    scanScreenDwellActive_ = true;
}

ConnectionStateCadenceDecision ConnectionStateCadenceModule::process(
        const ConnectionStateCadenceContext& ctx) {
    ConnectionStateCadenceDecision decision;

    if (lastBleConnectedForScanDwell_ && !ctx.bleConnectedNow && !ctx.bootSplashHoldActive) {
        scanScreenEnteredMs_ = ctx.nowMs;
        scanScreenDwellActive_ = true;
    }
    lastBleConnectedForScanDwell_ = ctx.bleConnectedNow;

    if ((ctx.nowMs - lastDisplayUpdateMs_) < ctx.displayUpdateIntervalMs) {
        return decision;
    }
    lastDisplayUpdateMs_ = ctx.nowMs;
    decision.displayUpdateDue = true;

    if (ctx.displayPreviewRunning || ctx.bootSplashHoldActive) {
        return decision;
    }

    if (scanScreenDwellActive_ && ctx.bleConnectedNow) {
        const unsigned long scanDwellMs = ctx.nowMs - scanScreenEnteredMs_;
        decision.holdScanDwell = scanDwellMs < ctx.scanScreenDwellMs;
    }

    if (!decision.holdScanDwell) {
        decision.shouldRunConnectionStateProcess = true;
        if (ctx.bleConnectedNow) {
            scanScreenDwellActive_ = false;
        }
    }

    return decision;
}
