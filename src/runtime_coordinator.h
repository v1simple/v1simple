#pragma once

#include <cstdint>

struct DriveLoopTiming {
    uint32_t loopStartUs = 0;
    uint32_t nowMs = 0;
};

struct DriveLoopDispatch {
    uint32_t nowMs = 0;
    bool bleConnected = false;
};

// Owns the normal-drive loop's phase order and presentation-suppression rules.
// The template keeps production calls statically bound while allowing native
// tests to execute the same coordinator against small deterministic fakes.
class DriveLoopCoordinator {
  public:
    template <typename Runtime>
    static void tick(Runtime& runtime) {
        if (!runtime.active()) {
            return;
        }

        const DriveLoopTiming timing = runtime.beginDriveLoop();
        const auto connection = runtime.processConnectionRuntime(timing.nowMs);
        runtime.acceptConnectionSnapshot(connection);

        if (connection.requestShowInitialScanning) {
            if (!runtime.powerOwnsPresentation()) {
                runtime.showInitialScanningScreen();
            }
            runtime.markInitialScanningScreenHandled();
        }
        if (!runtime.powerOwnsPresentation()) {
            runtime.presentConnectionState(timing.nowMs, connection);
        }

        bool bleConnected = connection.connected;
        bool bleBackpressure = connection.backpressured;
        runtime.processPower(timing.nowMs);
        const bool inSettings = !runtime.powerOwnsPresentation() && runtime.processTouch(timing.nowMs);
        runtime.servicePowerDisplayOwnership(timing.nowMs);
        const bool powerPresentationOwned = runtime.powerOwnsPresentation();

        bool alpProcessed = false;
        if (inSettings) {
            runtime.processAlp(timing.nowMs);
            alpProcessed = true;
            if (!runtime.preemptSettingsForLiveAlert()) {
                runtime.observeAlpProductState(timing.nowMs);
                runtime.processPeriodicMaintenance(timing.nowMs, bleConnected, false, false, true);
                runtime.finishDriveLoop(false, timing.loopStartUs, true);
                return;
            }
        }

        if (!powerPresentationOwned) {
            runtime.processTapGesture(timing.nowMs);
        }
        runtime.openBootReadyGate(timing.nowMs);
        runtime.processBleRuntime();
        runtime.processBleQueue();
        bleBackpressure = runtime.bleQueueBackpressured();
        const bool overloadLate = connection.overloaded || bleBackpressure;

        runtime.processConnectionCycle(timing.nowMs, bleConnected);
        runtime.processObd(timing.nowMs, bleConnected);
        if (!alpProcessed) {
            runtime.processAlp(timing.nowMs);
        }
        runtime.observeAlpProductState(timing.nowMs);
        runtime.processAlpPresentationAndPower(timing.nowMs);
        runtime.processGps(timing.nowMs);
        runtime.processSpeed(timing.nowMs);
        runtime.processSpeedAlert(timing.nowMs);

        const auto displayEdges = runtime.consumeDisplayEdges();
        if (!powerPresentationOwned) {
            runtime.presentDisplay(displayEdges, overloadLate);
        }

        const DriveLoopDispatch dispatch = runtime.processConnectionDispatch(powerPresentationOwned);
        bleConnected = dispatch.bleConnected;
        runtime.processPeriodicMaintenance(dispatch.nowMs, bleConnected, bleBackpressure, overloadLate, false);
        runtime.finishDriveLoop(bleBackpressure, timing.loopStartUs, false);
    }
};

// Owns the mutually exclusive boot and loop dispatch. The normal path has no
// route to the maintenance runtime, which is the sole owner of Wi-Fi service.
class MainRuntimeCoordinator {
  public:
    template <typename DriveRuntime, typename MaintenanceRuntime, typename ResetReason>
    static void start(bool maintenanceBoot, uint32_t setupStartMs, uint32_t stageStartedMs,
                      ResetReason resetReason, DriveRuntime& drive, MaintenanceRuntime& maintenance) {
        if (maintenanceBoot) {
            maintenance.start(setupStartMs, resetReason);
        } else {
            drive.start(setupStartMs, stageStartedMs, resetReason);
        }
    }

    template <typename DriveRuntime, typename MaintenanceRuntime, typename Clock>
    static void tick(DriveRuntime& drive, MaintenanceRuntime& maintenance, Clock&& nowMs) {
        if (maintenance.active()) {
            maintenance.tick(nowMs());
            return;
        }
        drive.tick();
    }
};

// Owns maintenance Wi-Fi start, service/recovery, and stop admission.
class MaintenanceWifiCoordinator {
  public:
    template <typename Runtime>
    static bool start(Runtime& runtime) {
        return runtime.startMaintenanceWifi();
    }

    template <typename Runtime>
    static void service(Runtime& runtime, uint32_t nowMs, bool presentationSuppressed) {
        if (presentationSuppressed) {
            return;
        }
        runtime.processMaintenanceWifi();
        const auto recovery = runtime.evaluateMaintenanceWifiRecovery(nowMs);
        if (recovery.attemptRestart) {
            runtime.restartMaintenanceWifi(recovery.attemptNumber);
        }
    }

    template <typename Runtime>
    static void stop(Runtime& runtime, const char* reason) {
        if (runtime.maintenanceWifiActive()) {
            runtime.stopMaintenanceWifi(reason);
        }
    }
};

// Owns shutdown transport boundaries around the shared persistence lifecycle.
// Resume always restores persistence admission before runtime transport service.
class RuntimeServiceLifecycleCoordinator {
  public:
    template <typename Runtime>
    static void prepareDrive(Runtime& runtime) {
        const bool persistenceSafe = runtime.preparePersistenceForShutdownPhase();
        runtime.disconnectDriveBleForShutdown();
        runtime.disconnectDriveObdForShutdown();
        runtime.stopDriveBleScanForShutdown();
        runtime.settleDriveShutdownTransport();
        if (persistenceSafe) {
            runtime.writeCleanShutdownMarker();
        }
    }

    template <typename Runtime>
    static void resumeDrive(Runtime& runtime) {
        runtime.resumePersistenceAfterAbortedShutdownPhase();
        runtime.resumeDriveBleAfterAbortedShutdown();
    }

    template <typename Runtime>
    static void prepareMaintenance(Runtime& runtime) {
        const bool persistenceSafe = runtime.preparePersistenceForShutdownPhase();
        if (runtime.maintenanceWifiActive()) {
            runtime.stopMaintenanceWifiForShutdown();
        }
        if (persistenceSafe) {
            runtime.writeCleanShutdownMarker();
        }
    }

    template <typename Runtime>
    static void resumeMaintenance(Runtime& runtime) {
        runtime.resumePersistenceAfterAbortedShutdownPhase();
        if (!runtime.maintenanceWifiActive()) {
            runtime.resumeMaintenanceWifiAfterAbortedShutdown();
        }
    }
};
