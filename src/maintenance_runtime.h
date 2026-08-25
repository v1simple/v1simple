#pragma once

#include <Arduino.h>
#include <esp_system.h>

#include "main_runtime_state.h"
#include "modules/power/power_module.h"
#include "modules/wifi/wifi_maintenance_recovery_module.h"
#include "modules/wifi/wifi_orchestrator_module.h"

class AutoPushModule;
class BatteryManager;
class DisplayPreviewModule;
class GpsRuntimeModule;
class HealthJournal;
class ObdRuntimeModule;
class PacketParser;
class ProductEventLog;
class QuietCoordinatorModule;
class SettingsManager;
class SpeedSourceSelector;
class StorageManager;
class TouchHandler;
class V1BLEClient;
class V1DeviceStore;
class V1Display;
class V1ProfileManager;
class WiFiManager;

// Exclusive owner of the maintenance boot lifecycle. DriveRuntime is not
// given a WiFiManager reference, so maintenance WiFi cannot be admitted from
// the normal boot composition graph.
class MaintenanceRuntime : public PowerLifecycle {
  public:
    MaintenanceRuntime(WiFiManager& wifi, SettingsManager& settings, V1ProfileManager& profiles,
                       V1DeviceStore& devices, StorageManager& storage, V1Display& display,
                       DisplayPreviewModule& preview, PowerModule& power, BatteryManager& battery, TouchHandler& touch,
                       V1BLEClient& ble, PacketParser& parser, AutoPushModule& autoPush,
                       ObdRuntimeModule& obd, SpeedSourceSelector& speed, GpsRuntimeModule& gps,
                       QuietCoordinatorModule& quiet, ProductEventLog& events, HealthJournal& health,
                       MainRuntimeState& state);

    void start(uint32_t setupStartMs, esp_reset_reason_t resetReason);
    void tick(uint32_t nowMs);
    void stop();

    bool active() const { return active_; }

  private:
    static void appendStatus(JsonObject status, void* context);

    void configureWebApi();
    void initializeStorageAndProfiles();
    void initializeTouchAndDisplayControls();
    void servicePowerDisplayOwnership(uint32_t nowMs);
    void logHeapSnapshot(const char* stage) const;
    void restartNormal(const char* reason);
    void prepareForShutdown() override;
    void resumeAfterAbortedShutdown() override;
    bool preservedPanicEvidencePresent(esp_reset_reason_t resetReason) const;

    WiFiManager& wifi_;
    SettingsManager& settings_;
    V1ProfileManager& profiles_;
    V1DeviceStore& devices_;
    StorageManager& storage_;
    V1Display& display_;
    DisplayPreviewModule& preview_;
    PowerModule& power_;
    BatteryManager& battery_;
    TouchHandler& touch_;
    V1BLEClient& ble_;
    PacketParser& parser_;
    AutoPushModule& autoPush_;
    ObdRuntimeModule& obd_;
    SpeedSourceSelector& speed_;
    GpsRuntimeModule& gps_;
    QuietCoordinatorModule& quiet_;
    ProductEventLog& events_;
    HealthJournal& health_;
    MainRuntimeState& state_;
    WifiOrchestrator wifiOrchestrator_;
    WifiMaintenanceRecoveryModule wifiRecovery_;

    String shownIp_;
    uint32_t bootButtonPressStartMs_ = 0;
    bool shownStation_ = false;
    bool exitRequestFired_ = false;
    bool idleHeapLogged_ = false;
    bool statusCallbackConfigured_ = false;
    bool active_ = false;
};
