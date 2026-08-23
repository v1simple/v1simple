/**
 * Main-loop phase interfaces.
 *
 * Keeps setup()/loop() orchestration readable while preserving exact behavior.
 */

#pragma once

#include <Arduino.h>
#include "modules/system/loop_connection_early_module.h"
#include "modules/system/loop_display_module.h"
#include "modules/system/loop_ingest_module.h"
#include "modules/system/loop_power_touch_module.h"
#include "modules/system/loop_runtime_snapshot_module.h"

struct LoopIngestPhaseValues {
    bool enableWifi = true;
    bool bootReady = false;
    bool bleBackpressure = false;
    bool skipLateNonCoreThisLoop = false;
    bool overloadLateThisLoop = false;
};

struct LoopWifiPhaseValues {
    LoopRuntimeSnapshotValues loopRuntimeSnapshotValues;
};

struct LoopFinalizePhaseValues {
    unsigned long dispatchNowMs = 0;
    bool bleConnectedNow = false;
    unsigned long lastLoopUs = 0;
};

LoopIngestPhaseValues processLoopIngestPhase(unsigned long nowMs, bool currentBootReady,
                                             unsigned long bootReadyDeadlineMs, bool skipNonCoreThisLoop,
                                             bool overloadThisLoop, bool presentationSuppressed);

// Loop ownership contract:
// - Ingest phase mutates BLE runtime and returns the settings snapshot.
// - loop() owns the OBD runtime refresh and speed selection update.
// - Display/Wi-Fi/finalize phases consume snapshots and run only their owned side effects.
void processLoopDisplayPreWifiPhase(unsigned long nowMs, bool bootSplashHoldActive, bool overloadLateThisLoop,
                                    bool presentationSuppressed);

LoopWifiPhaseValues processLoopWifiPhase(unsigned long nowMs, bool skipLateNonCoreThisLoop, bool bleBackpressure,
                                         bool overloadLateThisLoop, bool bleConnectBurstSettling,
                                         bool bootSplashHoldActive);

LoopFinalizePhaseValues processLoopFinalizePhase(bool bootSplashHoldActive, bool displayPreviewRunning,
                                                 bool bleBackpressure, bool overloadLateThisLoop,
                                                 unsigned long scanScreenDwellMs,
                                                 unsigned long connectionStateProcessMaxGapMs,
                                                 unsigned long loopStartUs);

unsigned long processLoopSettingsEarlyReturnPhase(unsigned long nowMs, unsigned long loopStartUs, bool bleConnected);

bool shouldReturnEarlyFromLoopPowerTouchPhase(unsigned long nowMs);
