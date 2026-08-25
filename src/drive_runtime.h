#pragma once

#include <Arduino.h>
#include <esp_system.h>

#include "display.h"
#include "display_mode.h"
#include "main_runtime_state.h"
#include "modules/alp/alp_event_latch.h"
#include "modules/alp/alp_runtime_module.h"
#include "modules/alert_persistence/alert_persistence_module.h"
#include "modules/auto_push/auto_push_module.h"
#include "modules/ble/ble_queue_module.h"
#include "modules/ble/connection_runtime_module.h"
#include "modules/ble/connection_state_cadence_module.h"
#include "modules/ble/connection_state_dispatch_module.h"
#include "modules/ble/connection_state_module.h"
#include "modules/display/display_orchestration_module.h"
#include "modules/display/display_pipeline_module.h"
#include "modules/display/display_preview_module.h"
#include "modules/display/display_restore_module.h"
#include "modules/gps/gps_runtime_module.h"
#include "modules/obd/obd_ble_client.h"
#include "modules/obd/obd_runtime_module.h"
#include "modules/obd/obd_settings_sync_module.h"
#include "modules/power/power_module.h"
#include "modules/quiet/quiet_coordinator_module.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/speed_mute/speed_mute_module.h"
#include "modules/system/connection_cycle_coordinator_module.h"
#include "modules/system/parsed_frame_event_module.h"
#include "modules/system/system_event_bus.h"
#include "modules/touch/tap_gesture_module.h"
#include "modules/touch/touch_ui_module.h"
#include "modules/voice/voice_module.h"
#include "modules/volume_fade/volume_fade_module.h"
#include "packet_parser.h"
#include "runtime_coordinator.h"
#include "touch_handler.h"

class BatteryManager;
class HealthJournal;
class ProductEventLog;
class SettingsManager;
class StorageManager;
class V1DeviceStore;
class V1ProfileManager;

// Exclusive owner of the normal-drive lifecycle and its runtime modules.
// Deliberately has no maintenance-connectivity dependency: normal boot cannot
// start or service the maintenance transport through this composition graph.
class DriveRuntime final : public PowerLifecycle, public ConnectionCycleLifecycle {
  public:
    DriveRuntime(SettingsManager& settings, V1ProfileManager& profiles, V1DeviceStore& devices,
                 StorageManager& storage, BatteryManager& battery, ProductEventLog& events,
                 HealthJournal& health);

    void start(uint32_t setupStartMs, uint32_t stageStartedMs, esp_reset_reason_t resetReason);
    void tick();
    void stop();
    bool active() const { return active_; }

    void resetConnectionCadence();
    void showInitialScanningScreen();

    V1BLEClient& ble() { return ble_; }
    PacketParser& parser() { return parser_; }
    V1Display& display() { return display_; }
    TouchHandler& touch() { return touch_; }
    SpeedSourceSelector& speed() { return speed_; }
    AutoPushModule& autoPush() { return autoPush_; }
    PowerModule& power() { return power_; }
    DisplayPreviewModule& preview() { return preview_; }
    QuietCoordinatorModule& quiet() { return quiet_; }
    ObdRuntimeModule& obd() { return obd_; }
    GpsRuntimeModule& gps() { return gps_; }
    AlpRuntimeModule& alp() { return alp_; }
    MainRuntimeState& state() { return state_; }

  private:
    friend class DriveLoopCoordinator;
    friend class RuntimeServiceLifecycleCoordinator;

    struct DisplayEdges {
        uint32_t nowMs = 0;
        ParsedFrameSignal parsed;
    };

    static DriveRuntime* callbackOwner_;
    static void onV1Data(const uint8_t* data, size_t length, uint16_t charUuid, uint32_t sessionGeneration,
                         uint32_t callbackMillis);
    static void onV1ConnectImmediate();
    static void onV1SessionOpened(uint32_t sessionGeneration);
    static void onV1SessionClosed(uint32_t sessionGeneration);
    static void onV1Connected();
    static void observeAlertTable(const AlertData* alerts, size_t count, uint8_t priorityIndex, uint32_t nowMs,
                                  void* context);
    static bool restoreConnectionDisplayOwner(void* context, uint32_t nowMs);

    void initializeStorageAndProfiles();
    void initializeBle(uint32_t setupStartMs, uint32_t& stageStartedMs);
    void initializeTouchAndUi();
    void initializeRuntimeModules();
    void finalizeBoot(uint32_t setupStartMs, uint32_t& stageStartedMs);
    void requestMaintenanceBootRestart();
    DriveLoopTiming beginDriveLoop();
    ConnectionRuntimeSnapshot processConnectionRuntime(uint32_t nowMs);
    void acceptConnectionSnapshot(const ConnectionRuntimeSnapshot& connection);
    void markInitialScanningScreenHandled();
    bool powerOwnsPresentation() const;
    void presentConnectionState(uint32_t nowMs, const ConnectionRuntimeSnapshot& connection);
    void processPower(uint32_t nowMs);
    bool processTouch(uint32_t nowMs);
    void servicePowerDisplayOwnership(uint32_t nowMs);
    bool preemptSettingsForLiveAlert();
    void processTapGesture(uint32_t nowMs);
    void openBootReadyGate(uint32_t nowMs);
    void processBleRuntime();
    void processBleQueue();
    bool bleQueueBackpressured() const;
    void observeAlpProductState(uint32_t nowMs);
    void processConnectionCycle(uint32_t nowMs, bool bleConnectedNow);
    void processObd(uint32_t nowMs, bool bleConnectedNow);
    void processAlp(uint32_t nowMs);
    void processAlpPresentationAndPower(uint32_t nowMs);
    void processGps(uint32_t nowMs);
    void processSpeed(uint32_t nowMs);
    void processSpeedAlert(uint32_t nowMs);
    DisplayEdges consumeDisplayEdges();
    void presentDisplay(const DisplayEdges& edges, bool overloadThisLoop);
    DriveLoopDispatch processConnectionDispatch(bool powerPresentationOwned);
    void processPeriodicMaintenance(uint32_t nowMs, bool bleConnected, bool bleBackpressure,
                                    bool loopOverloaded, bool forceTailBleDrainPending = false);
    uint32_t finishLoop(bool bleBackpressure, uint32_t loopStartUs, bool forceBleDrain = false);
    void finishDriveLoop(bool bleBackpressure, uint32_t loopStartUs, bool forceBleDrain);
    bool preparePersistenceForShutdownPhase();
    void disconnectDriveBleForShutdown();
    void disconnectDriveObdForShutdown();
    void stopDriveBleScanForShutdown();
    void settleDriveShutdownTransport();
    void writeCleanShutdownMarker();
    void resumePersistenceAfterAbortedShutdownPhase();
    void resumeDriveBleAfterAbortedShutdown();
    void prepareForShutdown() override;
    void resumeAfterAbortedShutdown() override;
    void stopObdScan() override { obd_.stopActiveScan(); }
    void cancelObdConnect() override { obd_.cancelPendingConnect(); }
    void stopProxyAdvertising() override { ble_.stopProxyAdvertising(); }
    void disconnectProxyPhones() override { ble_.disconnectProxyPhones(); }
    bool isObdScanStopped() const override { return obd_.isScanStopped(); }
    bool isObdConnectIdle() const override { return obd_.isConnectIdle(); }
    bool isProxyFullyStopped() const override { return ble_.isProxyFullyStopped(); }
    bool preservedPanicEvidencePresent(esp_reset_reason_t resetReason) const;
    void logBootStage(const char* stage, uint32_t setupStartMs, uint32_t& stageStartedMs) const;

    SettingsManager& settings_;
    V1ProfileManager& profiles_;
    V1DeviceStore& devices_;
    StorageManager& storage_;
    BatteryManager& battery_;
    ProductEventLog& events_;
    HealthJournal& health_;

    V1BLEClient ble_;
    PacketParser parser_;
    V1Display display_;
    TouchHandler touch_;
    SpeedSourceSelector speed_;
    AlertPersistenceModule alertPersistence_;
    DisplayPreviewModule preview_;
    ConnectionStateCadenceModule connectionCadence_;
    DisplayMode displayMode_ = DisplayMode::IDLE;
    VoiceModule voice_;
    VolumeFadeModule volumeFade_;
    QuietCoordinatorModule quiet_;
    AutoPushModule autoPush_;
    TouchUiModule touchUi_;
    TapGestureModule tapGesture_;
    PowerModule power_;
    BleQueueModule bleQueue_;
    ConnectionStateModule connectionState_;
    ConnectionRuntimeModule connectionRuntime_;
    ConnectionStateDispatchModule connectionDispatch_;
    DisplayPipelineModule displayPipeline_;
    AlpEventLatch alpEventLatch_;
    DisplayOrchestrationModule displayOrchestration_;
    DisplayRestoreModule displayRestore_;
    SystemEventBus systemEvents_;
    ObdSettingsSyncModule obdSettingsSync_;
    SpeedMuteModule speedMute_;
    ConnectionCycleCoordinatorModule connectionCycle_;
    ObdRuntimeModule obd_;
    ObdBleClient obdBle_;
    GpsRuntimeModule gps_;
    AlpRuntimeModule alp_;
    MainRuntimeState state_;

    bool connectedPersistenceWindowAnchored_ = false;
    uint32_t connectedPersistenceWindowStartedMs_ = 0;
    bool active_ = false;
};
