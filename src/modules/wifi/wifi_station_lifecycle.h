#pragma once

#include <Arduino.h>
#include "settings_types.h"
#include "wifi_scan_result_owner.h"
#include "wifi_saved_network_mutation_policy.h"

class SettingsManager;

// API-visible values retain their installed numeric and string mapping.
enum WifiClientState {
    WIFI_CLIENT_DISABLED = 0,
    WIFI_CLIENT_DISCONNECTED,
    WIFI_CLIENT_CONNECTING,
    WIFI_CLIENT_CONNECTED,
    WIFI_CLIENT_FAILED,
};

class WifiStationLifecycle {
  public:
    struct Environment {
        bool maintenanceMode = false;
        bool setupActive = false;
        unsigned long lowDmaCooldownMs = 0;
        bool v1Connected = false;
        unsigned long setupStartedAt = 0;
        bool autoStarted = false;
        bool apClientPresent = false;
        bool uiActivityPresent = false;
    };
    struct ConnectedEffect {
        void* context;
        void (*notify)(void*, unsigned long);
    };
    WifiStationLifecycle(SettingsManager& settings, WifiScanResultOwner& scanOwner, ConnectedEffect connectedEffect)
        : settings_(settings), scanOwner_(scanOwner), connectedEffect_(connectedEffect) {}

    WifiClientState clientState() const { return state_.client; }
    int connectedSlot() const { return state_.connectedSlot; }
    int connectedSlotForSsid(const String& ssid) const;
    int pendingSlot() const { return state_.pending.slot; }
    unsigned long retryAt() const { return state_.candidates.retryAt; }
    bool isReconnectGaveUp() const { return state_.reconnectFailures >= WIFI_MAX_RECONNECT_FAILURES; }
    void resetReconnectFailures() { state_.reconnectFailures = 0; state_.lastReconnectAttempt = 0; }

    bool connectToNetwork(const Environment&, const String& ssid, const String& password,
                          bool persistCredentialsOnSuccess = true, int persistSlotIndex = -1,
                          bool maintenanceAutoConnect = false);
    bool enableWifiClientFromSavedCredentials(const Environment&);
    bool testSavedNetwork(const Environment&, size_t index);
    void disconnectFromNetwork(const Environment&);
    void disableCommitted(const Environment&);
    void forgetCommitted(const Environment&);
    WifiSavedNetworkMutationPolicy::Decision savedNetworkMutation(size_t index) const;
    void savedNetworkMutationCommitted(const Environment&, size_t index,
                                       WifiSavedNetworkMutationPolicy::Decision decision, const char* reason);
    void processWifiClientConnectPhase(const Environment&);
    bool beginMaintenanceAutoConnectScan(const Environment&, bool explicitEnableRequest);
    void processMaintenanceAutoConnect(const Environment&);
    void checkWifiClientStatus(const Environment&);
    void cancelMaintenanceAutoConnect(const Environment&, const char* reason);

    void beginSession();
    void selectSessionMode(bool staEnabled);
    void clearPendingWifiConnectState();
    void staDisconnectedForStop() { state_.connectedSlot = -1; }
    void stop();
    void apRetiredForLowDma();
    void linkLostDuringApRetire(const Environment&);
    static WifiScanResultOwner::Driver scanDriver();

  private:
    enum class WifiConnectPhase : uint8_t { IDLE, PREPARE_OFF, WAIT_OFF, ENABLE_AP_STA, WAIT_AP_STA, BEGIN_CONNECT };
    enum class MaintenanceAutoConnectPhase : uint8_t { IDLE, SCANNING, CONNECTING, COMPLETE };
    struct PendingAttempt {
        String ssid;
        String password;
        bool persistCredentials = true;
        bool requiresSavedSlot = false;
        int slot = -1;
        WifiConnectPhase phase = WifiConnectPhase::IDLE;
        unsigned long startedAt = 0;
        unsigned long phaseStartedAt = 0;
    };
    struct Candidates {
        MaintenanceAutoConnectPhase phase = MaintenanceAutoConnectPhase::IDLE;
        unsigned long scanStartedAt = 0;
        size_t slots[kWifiStaSlotCount] = {};
        size_t count = 0;
        size_t cursor = 0;
        WifiScanStaDropGate staDropGate;
        unsigned long retryAt = 0;
        uint8_t collisionMask = 0;
    };
    struct State {
        WifiClientState client = WIFI_CLIENT_DISABLED;
        int connectedSlot = -1;
        PendingAttempt pending;
        Candidates candidates;
        uint32_t recoveryRevision = 0;
        bool recoveryPending = false;
        bool manualDisconnect = false;
        int reconnectFailures = 0;
        unsigned long lastReconnectAttempt = 0;
        bool reconnectDeferredLogged = false;
    } state_;

    SettingsManager& settings_;
    WifiScanResultOwner& scanOwner_;
    const ConnectedEffect connectedEffect_;
    static constexpr unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
    static constexpr unsigned long WIFI_MODE_SWITCH_SETTLE_MS = 100;
    static constexpr unsigned long WIFI_MAINTENANCE_SCAN_TIMEOUT_MS = 15000;
    static constexpr unsigned long WIFI_MAINTENANCE_LINK_LOSS_RETRY_MS = 5000;
    static constexpr unsigned long WIFI_MAINTENANCE_RETRY_INTERVAL_MS = 30000;
    static constexpr int WIFI_MAX_RECONNECT_FAILURES = 5;
    static constexpr unsigned long WIFI_RECONNECT_INTERVAL_MS = 30000;
    static constexpr unsigned long WIFI_RECONNECT_DEFER_NO_V1_MS = 90000;

    int findConfiguredSlotBySsid(const String&) const;
    bool savedSlotMatches(int index, const String& ssid) const;
    bool cancelObsoleteSavedNetworkAttempt(const Environment&);
    bool queueNextMaintenanceAutoConnectSlot(const Environment&);
    void finishMaintenanceAutoConnect(const Environment&, const char* reason, bool dropStaRadio);
    void disconnectTrackedWifiActivity(const Environment&, const char* reason, bool disableClientState);
    void scheduleMaintenanceAutoConnectRetry(const Environment&, unsigned long delayMs);
    bool maintenanceAddressCollision(const Environment&) const;
    void applyDeferredMaintenanceStaRadioDrop(const Environment&);
};
