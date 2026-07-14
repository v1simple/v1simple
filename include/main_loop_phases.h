/**
 * Main loop phase helpers extracted from main.cpp.
 *
 * Keeps setup()/loop() orchestration readable while preserving exact behavior.
 */

#pragma once

#include <Arduino.h>
#include "modules/system/loop_connection_early_module.h"
#include "modules/system/loop_display_module.h"
#include "modules/system/loop_ingest_module.h"
#include "modules/system/loop_post_display_module.h"
#include "modules/system/loop_power_touch_module.h"
#include "modules/system/loop_pre_ingest_module.h"
#include "modules/system/loop_runtime_snapshot_module.h"
#include "modules/system/loop_settings_prep_module.h"

struct LoopConnectionEarlyPhaseValues {
    bool bootSplashHoldActive = false;
    bool initialScanningScreenShown = false;
    bool bleConnectedNow = false;
    bool bleBackpressure = false;
    bool skipNonCoreThisLoop = false;
    bool overloadThisLoop = false;
};

struct LoopIngestPhaseValues {
    LoopSettingsPrepValues loopSettingsPrepValues;
    bool bootReady = false;
    bool bleBackpressure = false;
    bool skipLateNonCoreThisLoop = false;
    bool overloadLateThisLoop = false;
};

struct LoopWifiPhaseValues {
    LoopRuntimeSnapshotValues loopRuntimeSnapshotValues;
    bool wifiAutoStartDone = false;
    bool wifiManualStartIntentLatched = false;
};

struct LoopFinalizePhaseValues {
    unsigned long dispatchNowMs = 0;
    bool bleConnectedNow = false;
    unsigned long lastLoopUs = 0;
};

LoopConnectionEarlyPhaseValues processLoopConnectionEarlyPhase(unsigned long nowMs, unsigned long nowUs,
                                                               unsigned long lastLoopUs,
                                                               bool currentBootSplashHoldActive,
                                                               unsigned long currentBootSplashHoldUntilMs,
                                                               bool currentInitialScanningScreenShown);

LoopIngestPhaseValues processLoopIngestPhase(unsigned long nowMs, bool currentBootReady,
                                             unsigned long bootReadyDeadlineMs, bool skipNonCoreThisLoop,
                                             bool overloadThisLoop);

// Loop ownership contract:
// - Ingest phase mutates BLE runtime and returns the settings snapshot.
// - loop() owns the OBD runtime refresh and speed selection update.
// - Display/Wi-Fi/finalize phases consume snapshots and run only their owned side effects.
void processLoopDisplayPreWifiPhase(unsigned long nowMs, bool bootSplashHoldActive, bool overloadLateThisLoop);

LoopWifiPhaseValues processLoopWifiPhase(unsigned long nowMs, unsigned long v1ConnectedAtMs, bool enableWifi,
                                         bool wifiAutoStartAllowed, bool currentWifiAutoStartDone,
                                         bool wifiManualStartIntentLatched, bool skipLateNonCoreThisLoop,
                                         bool bleBackpressure, bool overloadLateThisLoop, bool bleConnectBurstSettling,
                                         bool bootSplashHoldActive);

LoopFinalizePhaseValues processLoopFinalizePhase(unsigned long nowMs, bool bootSplashHoldActive,
                                                 bool displayPreviewRunning, bool bleBackpressure,
                                                 bool overloadLateThisLoop, unsigned long scanScreenDwellMs,
                                                 unsigned long connectionStateProcessMaxGapMs,
                                                 unsigned long loopStartUs);

unsigned long processLoopSettingsEarlyReturnPhase(unsigned long nowMs, unsigned long loopStartUs);

bool shouldReturnEarlyFromLoopPowerTouchPhase(unsigned long nowMs, unsigned long loopStartUs);
