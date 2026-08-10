#include "main_loop_phases.h"

#include "battery_manager.h"
#include "ble_client.h"
#include "modules/auto_push/auto_push_module.h"
#include "modules/ble/connection_state_dispatch_module.h"
#include "modules/perf/debug_macros.h"
#include "modules/system/loop_tail_module.h"
#include "modules/system/periodic_maintenance_module.h"
#include "modules/touch/tap_gesture_module.h"
#include "modules/wifi/wifi_priority_policy_module.h"
#include "modules/wifi/wifi_runtime_module.h"
#include "config.h"
#include "main_globals.h"
#include "main_loop_settings_prep.h"
#include "settings.h"
#include "wifi_manager.h"

LoopConnectionEarlyPhaseValues processLoopConnectionEarlyPhase(const unsigned long nowMs, const unsigned long nowUs,
                                                               const unsigned long lastLoopUs,
                                                               const bool currentBootSplashHoldActive,
                                                               const unsigned long currentBootSplashHoldUntilMs,
                                                               const bool currentInitialScanningScreenShown,
                                                               const bool presentationSuppressed) {
    LoopConnectionEarlyContext loopConnectionEarlyCtx;
    loopConnectionEarlyCtx.nowMs = nowMs;
    loopConnectionEarlyCtx.nowUs = nowUs;
    loopConnectionEarlyCtx.lastLoopUs = lastLoopUs;
    loopConnectionEarlyCtx.bootSplashHoldActive = currentBootSplashHoldActive;
    loopConnectionEarlyCtx.bootSplashHoldUntilMs = currentBootSplashHoldUntilMs;
    loopConnectionEarlyCtx.initialScanningScreenShown = currentInitialScanningScreenShown;
    loopConnectionEarlyCtx.presentationSuppressed = presentationSuppressed;
    const LoopConnectionEarlyResult loopConnectionEarlyResult =
        loopConnectionEarlyModule.process(loopConnectionEarlyCtx);

    LoopConnectionEarlyPhaseValues values;
    values.bootSplashHoldActive = loopConnectionEarlyResult.bootSplashHoldActive;
    values.initialScanningScreenShown = loopConnectionEarlyResult.initialScanningScreenShown;
    values.bleConnectedNow = loopConnectionEarlyResult.bleConnectedNow;
    values.bleBackpressure = loopConnectionEarlyResult.bleBackpressure;
    values.skipNonCoreThisLoop = loopConnectionEarlyResult.skipNonCoreThisLoop;
    values.overloadThisLoop = loopConnectionEarlyResult.overloadThisLoop;
    return values;
}

LoopIngestPhaseValues processLoopIngestPhase(const unsigned long nowMs, const bool currentBootReady,
                                             const unsigned long bootReadyDeadlineMs, const bool skipNonCoreThisLoop,
                                             const bool overloadThisLoop, const bool presentationSuppressed) {
    const bool enableWifi = prepareLoopSettingsForIngest(
        nowMs, presentationSuppressed, [](const unsigned long gestureNowMs) { tapGestureModule.process(gestureNowMs); },
        []() { return settingsManager.get().enableWifi; });

    bool bootReady = currentBootReady;
    if (!bootReady && nowMs >= bootReadyDeadlineMs) {
        bootReady = true;
        bleClient.setBootReady(true);
        SerialLog.printf("[Boot] Ready gate opened at %lu ms (timeout)\n", nowMs);
    }
    wifiPriorityPolicyModule.apply(nowMs, bleClient, wifiManager);

    LoopIngestContext loopIngestCtx;
    loopIngestCtx.nowMs = nowMs;
    loopIngestCtx.bleProcessEnabled = true;
    loopIngestCtx.skipNonCoreThisLoop = skipNonCoreThisLoop;
    loopIngestCtx.overloadThisLoop = overloadThisLoop;
    const LoopIngestResult loopIngestResult = loopIngestModule.process(loopIngestCtx);

    LoopIngestPhaseValues values;
    values.enableWifi = enableWifi;
    values.bootReady = bootReady;
    values.bleBackpressure = loopIngestResult.bleBackpressure;
    values.skipLateNonCoreThisLoop = loopIngestResult.skipLateNonCoreThisLoop;
    values.overloadLateThisLoop = loopIngestResult.overloadLateThisLoop;
    return values;
}

void processLoopDisplayPreWifiPhase(const unsigned long nowMs, const bool bootSplashHoldActive,
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

LoopWifiPhaseValues processLoopWifiPhase(const unsigned long nowMs, const unsigned long v1ConnectedAtMs,
                                         const bool enableWifi, const bool wifiAutoStartAllowed,
                                         const bool currentWifiAutoStartDone, const bool wifiManualStartIntentLatched,
                                         const bool skipLateNonCoreThisLoop, const bool bleBackpressure,
                                         const bool overloadLateThisLoop, const bool bleConnectBurstSettling,
                                         const bool bootSplashHoldActive) {
    LoopRuntimeSnapshotContext loopRuntimeSnapshotCtx;
    loopRuntimeSnapshotCtx.canStartDmaProbeAllowed =
        enableWifi && !currentWifiAutoStartDone && wifiManualStartIntentLatched && wifiAutoStartAllowed;
    const LoopRuntimeSnapshotValues loopRuntimeSnapshotValues =
        loopRuntimeSnapshotModule.process(loopRuntimeSnapshotCtx);

    WifiRuntimeContext wifiRuntimeCtx;
    wifiRuntimeCtx.nowMs = nowMs;
    wifiRuntimeCtx.v1ConnectedAtMs = v1ConnectedAtMs;
    wifiRuntimeCtx.enableWifi = enableWifi;
    wifiRuntimeCtx.bleConnected = loopRuntimeSnapshotValues.bleConnected;
    wifiRuntimeCtx.canStartDma = loopRuntimeSnapshotValues.canStartDma;
    wifiRuntimeCtx.wifiAutoStartAllowed = wifiAutoStartAllowed;
    wifiRuntimeCtx.wifiAutoStartDone = currentWifiAutoStartDone;
    wifiRuntimeCtx.wifiManualStartIntentLatched = wifiManualStartIntentLatched;
    wifiRuntimeCtx.skipLateNonCoreThisLoop = skipLateNonCoreThisLoop;
    wifiRuntimeCtx.bleBackpressure = bleBackpressure;
    wifiRuntimeCtx.overloadLateThisLoop = overloadLateThisLoop;
    wifiRuntimeCtx.bleConnectBurstSettling = bleConnectBurstSettling;
    wifiRuntimeCtx.displayPreviewRunning = loopRuntimeSnapshotValues.displayPreviewRunning;
    wifiRuntimeCtx.bootSplashHoldActive = bootSplashHoldActive;
    const WifiRuntimeResult wifiRuntimeResult = wifiRuntimeModule.process(wifiRuntimeCtx);

    LoopWifiPhaseValues values;
    values.loopRuntimeSnapshotValues = loopRuntimeSnapshotValues;
    values.wifiAutoStartDone = wifiRuntimeResult.wifiAutoStartDone;
    values.wifiManualStartIntentLatched = wifiRuntimeResult.wifiManualStartIntentLatched;
    return values;
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

bool shouldReturnEarlyFromLoopPowerTouchPhase(const unsigned long nowMs, const unsigned long loopStartUs) {
    LoopPowerTouchContext loopPowerTouchCtx;
    loopPowerTouchCtx.nowMs = nowMs;
    loopPowerTouchCtx.loopStartUs = loopStartUs;
    loopPowerTouchCtx.bootButtonPressed = (digitalRead(BOOT_BUTTON_GPIO) == LOW);
    const LoopPowerTouchResult loopPowerTouchResult = loopPowerTouchModule.process(loopPowerTouchCtx);
    return loopPowerTouchResult.shouldReturnEarly;
}
