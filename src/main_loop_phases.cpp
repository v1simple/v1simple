#include "main_loop_phases.h"

#include "battery_manager.h"
#include "ble_client.h"
#include "modules/auto_push/auto_push_module.h"
#include "modules/ble/connection_state_dispatch_module.h"
#include "modules/system/loop_tail_module.h"
#include "modules/system/periodic_maintenance_module.h"
#include "modules/touch/tap_gesture_module.h"
#include "config.h"
#include "main_globals.h"
#include "settings.h"

LoopIngestPhaseValues processLoopIngestPhase(const unsigned long nowMs, const bool currentBootReady,
                                             const unsigned long bootReadyDeadlineMs, const bool skipNonCoreThisLoop,
                                             const bool overloadThisLoop, const bool presentationSuppressed) {
    if (!presentationSuppressed) {
        tapGestureModule.process(nowMs);
    }

    bool bootReady = currentBootReady;
    if (!bootReady && nowMs >= bootReadyDeadlineMs) {
        bootReady = true;
        bleClient.setBootReady(true);
        Serial.printf("[Boot] Ready gate opened at %lu ms (timeout)\n", nowMs);
    }
    LoopIngestContext loopIngestCtx;
    loopIngestCtx.nowMs = nowMs;
    loopIngestCtx.bleProcessEnabled = true;
    loopIngestCtx.skipNonCoreThisLoop = skipNonCoreThisLoop;
    loopIngestCtx.overloadThisLoop = overloadThisLoop;
    const LoopIngestResult loopIngestResult = loopIngestModule.process(loopIngestCtx);

    LoopIngestPhaseValues values;
    values.bootReady = bootReady;
    values.bleBackpressure = loopIngestResult.bleBackpressure;
    values.skipLateNonCoreThisLoop = loopIngestResult.skipLateNonCoreThisLoop;
    values.overloadLateThisLoop = loopIngestResult.overloadLateThisLoop;
    return values;
}

void processLoopDisplayPhase(const unsigned long nowMs, const bool bootSplashHoldActive,
                             const bool overloadLateThisLoop, const bool presentationSuppressed) {

    LoopDisplayContext loopDisplayCtx;
    loopDisplayCtx.nowMs = nowMs;
    loopDisplayCtx.bootSplashHoldActive = bootSplashHoldActive;
    loopDisplayCtx.overloadLateThisLoop = overloadLateThisLoop;
    loopDisplayCtx.presentationSuppressed = presentationSuppressed;
    loopDisplayModule.process(loopDisplayCtx);

    if (!presentationSuppressed) {
        autoPushModule.process();
    }
}

LoopFinalizePhaseValues processLoopFinalizePhase(const bool bootSplashHoldActive, const bool displayPreviewRunning,
                                                 const bool bleBackpressure, const bool overloadLateThisLoop,
                                                 const unsigned long scanScreenDwellMs,
                                                 const unsigned long connectionStateProcessMaxGapMs,
                                                 const unsigned long loopStartUs) {
    const uint32_t dispatchNowMs = millis();
    const bool bleConnectedNow = bleClient.isConnected();
    ConnectionStateDispatchContext dispatchCtx;
    dispatchCtx.nowMs = dispatchNowMs;
    dispatchCtx.displayUpdateIntervalMs = DISPLAY_UPDATE_MS;
    dispatchCtx.scanScreenDwellMs = scanScreenDwellMs;
    dispatchCtx.bleConnectedNow = bleConnectedNow;
    dispatchCtx.bootSplashHoldActive = bootSplashHoldActive;
    dispatchCtx.displayPreviewRunning = displayPreviewRunning;
    dispatchCtx.maxProcessGapMs = connectionStateProcessMaxGapMs;
    connectionStateDispatchModule.process(dispatchCtx);

    PeriodicMaintenanceModule::Context maintenanceCtx;
    maintenanceCtx.bleConnected = bleConnectedNow;
    maintenanceCtx.bleBackpressure = bleBackpressure;
    maintenanceCtx.loopOverloaded = overloadLateThisLoop;
    periodicMaintenanceModule.process(dispatchNowMs, maintenanceCtx);

    LoopFinalizePhaseValues values;
    values.dispatchNowMs = dispatchNowMs;
    values.bleConnectedNow = bleConnectedNow;
    values.lastLoopUs = loopTailModule.process(bleBackpressure, loopStartUs);
    return values;
}

unsigned long processLoopSettingsEarlyReturnPhase(const unsigned long nowMs, const unsigned long loopStartUs,
                                                  const bool bleConnected) {
    PeriodicMaintenanceModule::Context maintenanceCtx;
    maintenanceCtx.bleConnected = bleConnected;
    maintenanceCtx.forceTailBleDrainPending = true;
    periodicMaintenanceModule.process(nowMs, maintenanceCtx);
    return loopTailModule.process(false, loopStartUs, true);
}

bool shouldReturnEarlyFromLoopPowerTouchPhase(const unsigned long nowMs) {
    LoopPowerTouchContext loopPowerTouchCtx;
    loopPowerTouchCtx.nowMs = nowMs;
    loopPowerTouchCtx.bootButtonPressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);
    const LoopPowerTouchResult loopPowerTouchResult = loopPowerTouchModule.process(loopPowerTouchCtx);
    return loopPowerTouchResult.shouldReturnEarly;
}
