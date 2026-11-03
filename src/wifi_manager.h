/**
 * WiFi Manager for V1 Gen2 Display
 * AP+STA: setup AP for local UI/API plus optional STA for external network.
 * AP may be dropped dynamically (e.g., after STA connect) while WiFi service
 * state remains active.
 */

#pragma once
#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <FS.h>
#include <WebServer.h>
#include <type_traits>
#include <utility>
#include "modules/wifi/maintenance_write_routes.h"
#include "wifi_rate_limiter.h"
#include "settings_types.h"
#include "v1_settings_operation.h"
#include "modules/wifi/backup_snapshot_cache.h"
#include "modules/wifi/wifi_scan_result_owner.h"
#include "modules/wifi/wifi_station_lifecycle.h"
#include "modules/wifi/wifi_status_api_service.h"
#include "modules/wifi/wifi_maintenance_web_server.h"

struct WifiAudioSettingsRuntime;

namespace WifiAutoPushApiService {
struct Runtime;
}

namespace WifiDisplayColorsApiService {
struct Runtime;
}

namespace WifiSettingsApiService {
struct Runtime;
}

namespace WifiClientApiService {
struct Runtime;
struct SavedNetworkSlotPayload;
struct SavedNetworkUpsertPayload;
struct SavedNetworkPriorityUpdate;
enum class PriorityUpdateStatus : uint8_t;
} // namespace WifiClientApiService

namespace WifiV1ProfileApiService {
struct Runtime;
}

namespace WifiV1DevicesApiService {
struct Runtime;
}

namespace BackupApiService {
struct BackupRuntime;
}

namespace ObdApiService {
struct Runtime;
}

class ObdRuntimeModule;
class SpeedSourceSelector;
class GpsRuntimeModule;
class BatteryManager;
class DisplayPreviewModule;
class HealthJournal;
class ProductEventLog;
class SettingsManager;
class StorageManager;
class V1BLEClient;
class V1DeviceStore;
class V1Display;
class V1ProfileManager;
class V1SettingsOperationStore;

// WiFi service state (AP may be enabled or disabled while service is active)
enum SetupModeState {
    SETUP_MODE_OFF = 0,
    SETUP_MODE_AP_ON,
    SETUP_MODE_STOPPING,
};

inline bool wifiUiActiveSince(unsigned long lastActivityMs, unsigned long nowMs, unsigned long timeoutMs) {
    if (lastActivityMs == 0) {
        return false;
    }
    const uint32_t last = static_cast<uint32_t>(lastActivityMs);
    const uint32_t now = static_cast<uint32_t>(nowMs);
    const uint32_t timeout = static_cast<uint32_t>(timeoutMs);
    return static_cast<uint32_t>(now - last) < timeout;
}

class WiFiManager {
  public:
    WiFiManager(SettingsManager& settings, V1ProfileManager& profiles, V1DeviceStore& devices,
                StorageManager& storage);
    void setV1SettingsOperationStore(V1SettingsOperationStore* store) { v1SettingsOperations_ = store; }
    bool acknowledgeDeliveredSettingsOperationReturn();

    static constexpr unsigned long WIFI_LOW_DMA_PERSIST_MS = 1500;         // Require sustained low heap before shutdown
    static constexpr unsigned long WIFI_LOW_DMA_RETRY_COOLDOWN_MS = 30000; // Avoid rapid start/stop thrash

    // Setup AP control; maintenance may concurrently use STA for saved-network operations.
    bool startSetupMode(bool autoStarted = false);                         // Start or re-enable AP for configuration
    bool stopSetupMode(bool manual = false, const char* reason = nullptr); // Stop AP (manual/timeout/low_dma)

    bool isWifiServiceActive() const { return setupModeState_ == SETUP_MODE_AP_ON; }
    bool isSetupModeActive() const;
    bool isStopping() const;

    // Process web server requests (call in loop)
    void process();

    // Preflight check for setup-mode start admission.
    bool canStartSetupMode(uint32_t* freeInternal = nullptr, uint32_t* largestInternal = nullptr) const;
    unsigned long lowDmaCooldownRemainingMs() const;

    // Reset WiFi reconnect failure counter and debounce timer
    // (call when user manually triggers WiFi)
    void resetReconnectFailures() { station_.resetReconnectFailures(); }
    bool isReconnectGaveUp() const { return station_.isReconnectGaveUp(); }

    // Status
    bool isConnected() const { return !isStopping() && station_.clientState() == WIFI_CLIENT_CONNECTED; }
    bool isWifiServiceReachable() const { return isWifiServiceActive() && (isSetupModeActive() || isConnected()); }
    String getIPAddress() const; // STA IP when connected
    String getAPIPAddress() const;

    // WiFi client (STA) control - connect to external network
    bool startWifiScan();                             // Start or join an async scan for the UI
    bool isWifiScanRunning() const;                   // True while the UI owns the active scan generation
    bool isWifiScanInProgress();                      // Harvests a completed driver buffer before returning
    bool hasCompletedWifiScanResults();               // True when the UI has a stable result snapshot
    std::vector<ScannedNetwork> getScannedNetworks(); // Copy the stable UI scan snapshot
    bool connectToNetwork(const String& ssid, const String& password, bool persistCredentialsOnSuccess = true,
                          int persistSlotIndex = -1, bool maintenanceAutoConnect = false);
    void disconnectFromNetwork();
    void checkWifiClientStatus();    // Called internally by process() to manage STA connection

    // Callbacks for alert data (to display on web page)
    void setAlertCallback(void (*fn)(JsonObject, void*), void* ctx) {
        mergeAlert_ = fn;
        mergeAlertCtx_ = ctx;
    }
    void setStatusCallback(void (*fn)(JsonObject, void*), void* ctx) {
        mergeStatus_ = fn;
        mergeStatusCtx_ = ctx;
    }
    void appendStatusCallback(void (*fn)(JsonObject, void*), void* ctx) {
        mergeStatus2_ = fn;
        mergeStatus2Ctx_ = ctx;
    }

    // V1 connection state used to defer WiFi client operations.
    void setV1ConnectedCallback(bool (*fn)(void*), void* ctx) {
        isV1Connected_ = fn;
        isV1ConnectedCtx_ = ctx;
    }

    // OBD and speed selector dependencies (used by OBD API routes and backup restore sync)
    void setObdDependencies(ObdRuntimeModule* obd, SpeedSourceSelector* speed) {
        obdRuntime_ = obd;
        speedSelector_ = speed;
    }

    // GPS runtime dependency (used by /api/gps/status)
    void setGpsRuntime(GpsRuntimeModule* gps) { gpsRuntime_ = gps; }
    void setMaintenanceDependencies(V1BLEClient& ble, V1Display& display,
                                    DisplayPreviewModule& preview, BatteryManager& battery,
                                    ProductEventLog& events, HealthJournal& health) {
        bleRuntime_ = &ble;
        display_ = &display;
        displayPreview_ = &preview;
        battery_ = &battery;
        productEvents_ = &events;
        health_ = &health;
    }

    // Maintenance boot intentionally skips BLE/V1 scan. Routes that would
    // mutate BLE runtime state must become no-ops while this is true.
    void setMaintenanceBootMode(bool enabled) {
        maintenanceBootMode_ = enabled;
        settingsReturnRetry_.clear();
    }
    bool isMaintenanceBootMode() const { return maintenanceBootMode_; }

    // Web activity tracking for the maintenance-session idle deadline
    void markUiActivity();                                  // Call on every HTTP request
    bool isUiActive(unsigned long timeoutMs = 30000) const; // True if request within timeout

  private:
    SettingsManager& settings_;
    V1ProfileManager& profiles_;
    V1DeviceStore& devices_;
    StorageManager& storage_;
    WifiMaintenanceWebServer server_;
    bool webRoutesInitialized_ = false;
    SetupModeState setupModeState_;
    bool apInterfaceEnabled_ = false; // True only when softAP interface is enabled
    unsigned long setupModeStartTime_;
    unsigned long lastClientSeenMs_ = 0; // Tracks last STA presence for timeout
    unsigned long lastApStaCountPollMs_ = 0;
    int cachedApStaCount_ = 0;
    static constexpr unsigned long AP_STA_COUNT_POLL_MS = 250;
    // Keep request handling hot while amortizing lower-priority maintenance work.
    static constexpr unsigned long WIFI_MAINTENANCE_FAST_MS = 10;
    static constexpr unsigned long WIFI_STATUS_CHECK_MS = 50;
    static constexpr unsigned long WIFI_TIMEOUT_CHECK_MS = 250;
    unsigned long lastMaintenanceFastMs_ = 0;
    unsigned long lastStatusCheckMs_ = 0;
    unsigned long lastTimeoutCheckMs_ = 0;

    WifiScanResultOwner wifiScanOwner_;
    WifiStationLifecycle station_;
    static constexpr unsigned long WIFI_STOP_PHASE_SETTLE_MS = 8;

    enum class WifiStopPhase : uint8_t {
        IDLE = 0,
        STOP_HTTP_SERVER,
        DISCONNECT_STA,
        DISABLE_AP,
        MODE_OFF,
        FINALIZE,
    };
    WifiStopPhase wifiStopPhase_ = WifiStopPhase::IDLE;
    unsigned long wifiStopPhaseStartMs_ = 0;
    unsigned long wifiStopStartMs_ = 0;
    String wifiStopReason_;
    bool wifiStopManual_ = false;
    bool wifiStopHadSta_ = false;
    bool wifiStopHadAp_ = false;

    // Web activity tracking for maintenance-session idle deadlines
    unsigned long lastUiActivityMs_ = 0;

    // Low-DMA protection state (prevents rapid restart loops under heap pressure)
    unsigned long lowDmaCooldownUntilMs_ = 0;
    unsigned long lowDmaSinceMs_ = 0;
    // If neither STA nor AP has any connected client for long enough, shut WiFi
    // down until manual restart to preserve core runtime headroom.
    static constexpr unsigned long WIFI_NO_CLIENT_SHUTDOWN_MS = 60000;
    // Shorter grace for auto-started WiFi: if no client connects within one STA
    // timeout cycle, shut down promptly to reclaim DMA headroom.
    static constexpr unsigned long WIFI_NO_CLIENT_SHUTDOWN_AUTO_MS = 20000;
    // When STA is connected, keep AP alive briefly for setup-page races, then
    // retire AP once no AP clients have been seen for this long.
    static constexpr unsigned long WIFI_AP_IDLE_DROP_AFTER_STA_MS = 60000;
    unsigned long lastAnyClientSeenMs_ = 0;
    bool wasAutoStarted_ = false; // True when WiFi was started by boot auto-start (not manual)

    // Mutation rate limiting. Read-only status polls track UI activity without
    // entering this shared write-admission window.
    SlidingWindowRateLimiter rateLimiter_;
    bool checkRateLimit(); // Returns true if mutation is allowed, false if rate limited
    bool admitMaintenanceWriteBeforeBody();
    bool maintenanceWritePreAdmitted_ = false;
    bool hasMaintenanceWriteRequestShape() const;
    bool requireMaintenanceWriteRequestShape();
    template <MaintenanceWriteRoutes::Id id, typename Handler>
    void registerMaintenanceWriteRoute(Handler handler) {
        constexpr const auto& route = MaintenanceWriteRoutes::get<id>();
        if constexpr (route.reader == MaintenanceWriteRoutes::Reader::Exact) {
            static_assert(std::is_invocable_r_v<void, Handler, const uint8_t*, size_t, const char*, size_t>,
                          "Exact maintenance route requires a byte-preserving body handler");
            registerMaintenanceExactBodyWriteRoute(route, std::move(handler));
        } else {
            static_assert(std::is_invocable_r_v<void, Handler>,
                          "Framework maintenance route requires a framework argument handler");
            registerMaintenanceFrameworkWriteRoute(route, std::move(handler));
        }
    }
    void registerMaintenanceFrameworkWriteRoute(const MaintenanceWriteRoutes::Route& route,
                                                 WebServer::THandlerFunction handler);
    void registerMaintenanceExactBodyWriteRoute(const MaintenanceWriteRoutes::Route& route,
                                                std::function<void(const uint8_t*, size_t,
                                                                   const char*, size_t)> handler);
    static const char* maintenanceApiWriteHeader();
    static const char* maintenanceApiWriteHeaderValue();

    // Status JSON caching (Option 2 optimization)
    static constexpr unsigned long STATUS_CACHE_TTL_MS = 500; // 500ms cache
    WifiStatusApiService::StatusJsonCache cachedStatusJson_;
    unsigned long lastStatusJsonTime_ = 0;
    BackupApiService::BackupSnapshotCache cachedBackupSnapshot_;

    void (*mergeAlert_)(JsonObject, void* ctx) = nullptr;
    void* mergeAlertCtx_ = nullptr;
    void (*mergeStatus_)(JsonObject, void* ctx) = nullptr;
    void* mergeStatusCtx_ = nullptr;
    void (*mergeStatus2_)(JsonObject, void* ctx) = nullptr; // appended by appendStatusCallback
    void* mergeStatus2Ctx_ = nullptr;
    bool (*isV1Connected_)(void* ctx) = nullptr; // Returns true when V1 is connected (defer WiFi ops until then)
    void* isV1ConnectedCtx_ = nullptr;
    ObdRuntimeModule* obdRuntime_ = nullptr;
    SpeedSourceSelector* speedSelector_ = nullptr;
    GpsRuntimeModule* gpsRuntime_ = nullptr;
    V1BLEClient* bleRuntime_ = nullptr;
    V1Display* display_ = nullptr;
    DisplayPreviewModule* displayPreview_ = nullptr;
    BatteryManager* battery_ = nullptr;
    ProductEventLog* productEvents_ = nullptr;
    HealthJournal* health_ = nullptr;
    V1SettingsOperationStore* v1SettingsOperations_ = nullptr;
    bool operationRestartPending_ = false;
    uint32_t operationRestartAtMs_ = 0;
    bool maintenanceBootMode_ = false;
    V1SettingsOperationPolicy::ReturnToMaintenanceRetry settingsReturnRetry_;

    // Setup functions
    bool setupAP();
    bool setupWebServer();
    void checkAutoTimeout();
    std::vector<WifiClientApiService::SavedNetworkSlotPayload> getSavedNetworkSlots() const;
    bool upsertSavedNetwork(const WifiClientApiService::SavedNetworkUpsertPayload& request, size_t& indexOut);
    bool deleteSavedNetwork(size_t index);
    WifiClientApiService::PriorityUpdateStatus updateSavedNetworkPriorities(
        const std::vector<WifiClientApiService::SavedNetworkPriorityUpdate>& updates);
    bool testSavedNetwork(size_t index);
    int selectSlotForNetworkConnect(const String& ssid) const;
    WifiStationLifecycle::Environment stationEnvironment() const;
    bool enableWifiClientFromSavedCredentials();
    bool disableWifiClient();
    bool forgetWifiClient();
    void processStopSetupModePhase();
    void finalizeStopSetupMode();
    bool stopSetupModeImmediate(bool emergencyLowDma);
    WifiAutoPushApiService::Runtime makeAutoPushRuntime();
    WifiDisplayColorsApiService::Runtime makeDisplayColorsRuntime();
    WifiAudioSettingsRuntime makeAudioRuntime();
    WifiStatusApiService::StatusRuntime makeStatusRuntime();
    WifiSettingsApiService::Runtime makeSettingsRuntime();
    WifiClientApiService::Runtime makeWifiClientRuntime();
    ObdApiService::Runtime makeObdRuntime();
    WifiV1ProfileApiService::Runtime makeV1ProfileRuntime();
    WifiV1DevicesApiService::Runtime makeV1DevicesRuntime();
    BackupApiService::BackupRuntime makeBackupRuntime();

    // API endpoints
    void handleNotFound();

    // LittleFS file serving (new UI)
    bool serveLittleFSFile(const char* path, const char* contentType);
};

#endif // WIFI_MANAGER_H
