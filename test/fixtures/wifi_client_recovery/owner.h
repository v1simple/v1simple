#pragma once
#include "Arduino.h"
#include "WiFi.h"
#include <freertos/FreeRTOS.h>
#include "settings_types.h"
#include "modules/wifi/wifi_scan_result_owner.h"
#include "modules/wifi/wifi_maintenance_link_policy.h"
#include "modules/wifi/wifi_maintenance_interface_policy.h"
#include "modules/wifi/wifi_setup_network_policy.h"
#include "modules/wifi/wifi_reconnect_policy.h"
#include "modules/wifi/wifi_sta_slot_policy.h"
#include "modules/wifi/wifi_heap_guard_module.h"
#include "esp_heap_caps.h"

#include "settings_sanitize.h"

#include "modules/wifi/wifi_station_lifecycle.h"
#include "modules/wifi/wifi_client_api_service.h"

class WiFiManager {
  public:
    SettingsManager settings_;
    WifiScanResultOwner wifiScanOwner_;
    WifiStationLifecycle station_{settings_, wifiScanOwner_, {this, [](void* context, unsigned long now) {
        static_cast<WiFiManager*>(context)->lastClientSeenMs_ = now;
    }}};
    WifiStationLifecycle::Environment stationEnvironment() const;
    bool enableWifiClientFromSavedCredentials();
    bool disableWifiClient();
    bool forgetWifiClient();
    int selectSlotForNetworkConnect(const String&) const;
    WifiClientApiService::SlotActionStatus upsertSavedNetwork(
        const WifiClientApiService::SavedNetworkUpsertPayload&, size_t&);
    WifiClientApiService::SlotActionStatus deleteSavedNetwork(const WifiClientApiService::SavedNetworkSlotRequest&);
    bool savedNetworkSlotMatches(const WifiClientApiService::SavedNetworkSlotRequest&) const;
    bool maintenanceBootMode_ = true;
    bool fixtureSetupActive = true;
    bool isSetupModeActive() const { return fixtureSetupActive; }
    bool isWifiServiceActive() const { return true; }
    unsigned long lowDmaCooldownRemainingMs() const { return 0; }
    struct FakeServer {
        void handleClient() {}
        void collectHeaders(const char**, size_t) {}
        void begin() {}
    } server_;
    void checkAutoTimeout() {}
    struct FakeBle { bool isConnected() const { return false; } };
    FakeBle* bleRuntime_ = nullptr;
    bool (*isV1Connected_)(void*) = nullptr;
    void* isV1ConnectedCtx_ = nullptr;
    bool wasAutoStarted_ = false;

    // AP/server resources are available; their radio/client lifecycle is real.
    bool startSetupMode(bool);
    bool canStartSetupMode(uint32_t*, uint32_t*) { return true; }
    bool setupAP() { return true; }
    bool setupWebServer() { return true; }
    const char* maintenanceApiWriteHeader() { return "fixture"; }
    enum SetupModeState { SETUP_MODE_OFF, SETUP_MODE_AP_ON, SETUP_MODE_STOPPING };
    SetupModeState setupModeState_ = SETUP_MODE_OFF;
    enum class WifiStopPhase { IDLE };
    WifiStopPhase wifiStopPhase_ = WifiStopPhase::IDLE;
    unsigned long wifiStopPhaseStartMs_ = 0, wifiStopStartMs_ = 0;
    String wifiStopReason_;
    bool wifiStopManual_ = false, wifiStopHadSta_ = false, wifiStopHadAp_ = false;
    unsigned long lowDmaSinceMs_ = 0, lowDmaCooldownUntilMs_ = 0, lastAnyClientSeenMs_ = 0;
    void resetReconnectFailures() { station_.resetReconnectFailures(); }
    bool isReconnectGaveUp() const { return station_.isReconnectGaveUp(); }
    void runOwnerPass();
    WifiClientApiService::SlotActionStatus testSavedNetwork(const WifiClientApiService::SavedNetworkSlotRequest&);
    bool connectToNetwork(const String&, const String&, bool = true, int = -1, bool = false);
    void disconnectFromNetwork();
    void checkWifiClientStatus();

    bool apInterfaceEnabled_ = false;
    unsigned long setupModeStartTime_ = 0;
    unsigned long lastClientSeenMs_ = 0;
    unsigned long lastApStaCountPollMs_ = 0;
    int cachedApStaCount_ = 0;
    static constexpr unsigned long WIFI_MAINTENANCE_FAST_MS = 10;
    static constexpr unsigned long WIFI_STATUS_CHECK_MS = 50;
    static constexpr unsigned long WIFI_TIMEOUT_CHECK_MS = 250;
    unsigned long lastMaintenanceFastMs_ = 0;
    unsigned long lastStatusCheckMs_ = 0;
    unsigned long lastTimeoutCheckMs_ = 0;
    unsigned long lastUiActivityMs_ = 0;
};
