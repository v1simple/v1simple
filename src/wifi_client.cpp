/**
 * WiFi STA connection, scan, and reconnect lifecycle.
 */

#include "wifi_manager_internals.h"
#include "ble_client.h"
#include "settings.h"
#include "settings_sanitize.h"
#include "modules/wifi/wifi_client_enable_transaction.h"
#include "modules/wifi/wifi_maintenance_interface_policy.h"
#include "modules/wifi/wifi_reconnect_policy.h"
#include "modules/wifi/wifi_saved_network_mutation_policy.h"
#include "modules/wifi/wifi_setup_network_policy.h"
#include "modules/wifi/wifi_sta_slot_policy.h"
#include <vector>

#include <esp_wifi.h>

namespace {

int16_t startPhysicalWifiScan(void* /*ctx*/) {
    WiFi.scanDelete();
    return WiFi.scanNetworks(true, false, false, 300);
}

int16_t getPhysicalWifiScanStatus(void* /*ctx*/) {
    return WiFi.scanComplete();
}

String getPhysicalWifiScanSsid(int16_t index, void* /*ctx*/) {
    return WiFi.SSID(index);
}

int32_t getPhysicalWifiScanRssi(int16_t index, void* /*ctx*/) {
    return WiFi.RSSI(index);
}

uint8_t getPhysicalWifiScanEncryption(int16_t index, void* /*ctx*/) {
    return static_cast<uint8_t>(WiFi.encryptionType(index));
}

void releasePhysicalWifiScan(void* /*ctx*/) {
    WiFi.scanDelete();
}

void abortPhysicalWifiScan(void* /*ctx*/) {
    esp_wifi_scan_stop();
    WiFi.scanDelete();
}

WifiScanResultOwner::Driver makeWifiScanDriver() {
    WifiScanResultOwner::Driver driver;
    driver.runningStatus = WIFI_SCAN_RUNNING;
    driver.start = startPhysicalWifiScan;
    driver.status = getPhysicalWifiScanStatus;
    driver.ssidAt = getPhysicalWifiScanSsid;
    driver.rssiAt = getPhysicalWifiScanRssi;
    driver.encryptionAt = getPhysicalWifiScanEncryption;
    driver.release = releasePhysicalWifiScan;
    driver.abort = abortPhysicalWifiScan;
    return driver;
}

} // namespace

String WiFiManager::getAPIPAddress() const {
    if (isSetupModeActive()) {
        return WiFi.softAPIP().toString();
    }
    return "";
}

String WiFiManager::getIPAddress() const {
    if (wifiClientState_ == WIFI_CLIENT_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "";
}

String WiFiManager::getConnectedSSID() const {
    if (wifiClientState_ == WIFI_CLIENT_CONNECTED) {
        return WiFi.SSID();
    }
    return "";
}

bool WiFiManager::startWifiScan() {
    const WifiScanResultOwner::RequestResult result =
        wifiScanOwner_.request(WifiScanConsumer::UI, makeWifiScanDriver());
    if (result == WifiScanResultOwner::RequestResult::FAILED) {
        Serial.println("[WiFiClient] UI scan failed to start");
        return false;
    }

    Serial.println(result == WifiScanResultOwner::RequestResult::JOINED
                       ? "[WiFiClient] UI joined the active network scan"
                       : "[WiFiClient] Starting async network scan for UI");
    return true;
}

bool WiFiManager::isWifiScanRunning() const {
    return wifiScanOwner_.isRunning() && wifiScanOwner_.isPending(WifiScanConsumer::UI);
}

bool WiFiManager::isWifiScanInProgress() {
    const WifiScanResultOwner::HarvestResult result = wifiScanOwner_.harvest(makeWifiScanDriver());
    if (result == WifiScanResultOwner::HarvestResult::FAILED) {
        Serial.println("[WiFiClient] Active network scan failed");
    }
    return result == WifiScanResultOwner::HarvestResult::RUNNING;
}

bool WiFiManager::hasCompletedWifiScanResults() {
    const WifiScanResultOwner::HarvestResult result = wifiScanOwner_.harvest(makeWifiScanDriver());
    if (result == WifiScanResultOwner::HarvestResult::FAILED) {
        Serial.println("[WiFiClient] Active network scan failed");
    }
    return wifiScanOwner_.hasSnapshot(WifiScanConsumer::UI);
}

std::vector<ScannedNetwork> WiFiManager::getScannedNetworks() {
    const WifiScanResultOwner::HarvestResult result = wifiScanOwner_.harvest(makeWifiScanDriver());
    if (result == WifiScanResultOwner::HarvestResult::FAILED) {
        Serial.println("[WiFiClient] Active network scan failed");
        return {};
    }

    std::vector<ScannedNetwork> networks = wifiScanOwner_.copySnapshot(WifiScanConsumer::UI, makeWifiScanDriver());
    if (wifiScanOwner_.hasSnapshot(WifiScanConsumer::UI)) {
        Serial.printf("[WiFiClient] UI scan snapshot has %u network(s)\n", static_cast<unsigned>(networks.size()));
    }
    return networks;
}

void WiFiManager::resetWifiScanState() {
    wifiScanOwner_.reset(makeWifiScanDriver());
}

std::vector<WifiClientApiService::SavedNetworkSlotPayload> WiFiManager::getSavedNetworkSlots() const {
    std::vector<WifiClientApiService::SavedNetworkSlotPayload> slots;
    slots.reserve(kWifiStaSlotCount);
    const V1Settings& settings = settings_.get();
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        const WifiStaSlot& slot = settings.wifiStaSlots[i];
        WifiClientApiService::SavedNetworkSlotPayload payload;
        payload.index = i;
        payload.ssid = slot.ssid;
        payload.label = slot.label;
        payload.priority = slot.priority;
        payload.lastConnectedAtSec = slot.lastConnectedAtSec;
        payload.configured = slot.isConfigured();
        payload.hasPassword = payload.configured && settings_.getWifiStaSlotPassword(i).length() > 0;
        slots.push_back(payload);
    }
    return slots;
}

int WiFiManager::selectSlotForNetworkConnect(const String& ssid) const {
    const String sanitizedSsid = sanitizeWifiClientSsidValue(ssid);
    if (sanitizedSsid.length() == 0) {
        return -1;
    }

    const V1Settings& settings = settings_.get();
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        if (settings.wifiStaSlots[i].isConfigured() && settings.wifiStaSlots[i].ssid == sanitizedSsid) {
            return static_cast<int>(i);
        }
    }
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        if (!settings.wifiStaSlots[i].isConfigured()) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int WiFiManager::findConfiguredSlotBySsid(const String& ssid) const {
    const String sanitizedSsid = sanitizeWifiClientSsidValue(ssid);
    if (sanitizedSsid.length() == 0) {
        return -1;
    }

    const V1Settings& settings = settings_.get();
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        if (settings.wifiStaSlots[i].isConfigured() && settings.wifiStaSlots[i].ssid == sanitizedSsid) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool WiFiManager::upsertSavedNetwork(const WifiClientApiService::SavedNetworkUpsertPayload& request, size_t& indexOut) {
    const String ssid = sanitizeWifiClientSsidValue(request.ssid);
    if (ssid.length() == 0) {
        return false;
    }

    int selectedIndex = -1;
    const V1Settings& settings = settings_.get();
    if (request.hasIndex) {
        if (request.index >= kWifiStaSlotCount) {
            return false;
        }
        selectedIndex = static_cast<int>(request.index);
    } else {
        selectedIndex = selectSlotForNetworkConnect(ssid);
    }

    if (selectedIndex < 0 || static_cast<size_t>(selectedIndex) >= kWifiStaSlotCount) {
        return false;
    }

    const size_t index = static_cast<size_t>(selectedIndex);
    const WifiStaSlot& currentSlot = settings.wifiStaSlots[index];
    const bool maintenanceScanActive = maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::SCANNING;
    const bool maintenanceConnectActive = maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::CONNECTING;
    const String password = request.hasPassword ? request.password : settings_.getWifiStaSlotPassword(index);
    const String label = request.hasLabel ? request.label : currentSlot.label;
    const uint8_t priority = request.hasPriority
                                 ? request.priority
                                 : (currentSlot.isConfigured() ? currentSlot.priority : static_cast<uint8_t>(index));

    if (!settings_.setWifiStaSlotCredentials(index, ssid, password, label, priority)) {
        return false;
    }
    const WifiSavedNetworkMutationPolicy::Decision mutation = WifiSavedNetworkMutationPolicy::evaluate(
        {true, false, selectedIndex, currentConnectedSlotIndex_, pendingConnectSlotIndex_, maintenanceScanActive,
         maintenanceConnectActive});
    if (mutation.cancelMaintenanceAutoActivity) {
        cancelMaintenanceAutoConnect("slot_upsert");
    }
    maintenanceAddressCollisionSlotMask_ &= static_cast<uint8_t>(~(1U << index));
    if (mutation.disconnectTrackedActivity) {
        disconnectTrackedWifiActivity("slot_upsert", false);
    }
    if (mutation.scheduleReplacementScan) {
        scheduleMaintenanceAutoConnectRetry(WIFI_MAINTENANCE_LINK_LOSS_RETRY_MS);
    }
    indexOut = index;
    return true;
}

bool WiFiManager::deleteSavedNetwork(size_t index) {
    if (index >= kWifiStaSlotCount) {
        return false;
    }
    const bool maintenanceScanActive = maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::SCANNING;
    const bool maintenanceConnectActive = maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::CONNECTING;
    if (!settings_.clearWifiStaSlot(index)) {
        return false;
    }
    const WifiSavedNetworkMutationPolicy::Decision mutation = WifiSavedNetworkMutationPolicy::evaluate(
        {true, false, static_cast<int>(index), currentConnectedSlotIndex_, pendingConnectSlotIndex_,
         maintenanceScanActive, maintenanceConnectActive});
    if (mutation.cancelMaintenanceAutoActivity) {
        cancelMaintenanceAutoConnect("slot_delete");
    }
    maintenanceAddressCollisionSlotMask_ &= static_cast<uint8_t>(~(1U << index));
    if (mutation.disconnectTrackedActivity) {
        disconnectTrackedWifiActivity("slot_delete", false);
    }
    if (mutation.scheduleReplacementScan) {
        scheduleMaintenanceAutoConnectRetry(WIFI_MAINTENANCE_LINK_LOSS_RETRY_MS);
    }
    return true;
}

WifiClientApiService::PriorityUpdateStatus WiFiManager::updateSavedNetworkPriorities(
    const std::vector<WifiClientApiService::SavedNetworkPriorityUpdate>& updates) {
    if (updates.empty() || updates.size() > kWifiStaSlotCount) {
        return WifiClientApiService::PriorityUpdateStatus::Invalid;
    }

    bool seenIndices[kWifiStaSlotCount] = {};
    bool seenPriorities[kWifiStaSlotCount] = {};
    uint8_t finalPriorities[kWifiStaSlotCount];
    const V1Settings& current = settings_.get();
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        finalPriorities[index] = current.wifiStaSlots[index].priority;
    }

    std::vector<WifiStaPriorityUpdate> settingsUpdates;
    settingsUpdates.reserve(updates.size());
    for (const auto& update : updates) {
        if (update.index >= kWifiStaSlotCount || update.priority >= kWifiStaSlotCount || seenIndices[update.index] ||
            seenPriorities[update.priority]) {
            return WifiClientApiService::PriorityUpdateStatus::Invalid;
        }
        if (!current.wifiStaSlots[update.index].isConfigured()) {
            return WifiClientApiService::PriorityUpdateStatus::Conflict;
        }
        seenIndices[update.index] = true;
        seenPriorities[update.priority] = true;
        finalPriorities[update.index] = update.priority;
        settingsUpdates.push_back(WifiStaPriorityUpdate{update.index, update.priority});
    }

    bool finalSeen[kWifiStaSlotCount] = {};
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        if (!current.wifiStaSlots[index].isConfigured()) {
            continue;
        }
        const uint8_t priority = finalPriorities[index];
        if (priority >= kWifiStaSlotCount || finalSeen[priority]) {
            return WifiClientApiService::PriorityUpdateStatus::Conflict;
        }
        finalSeen[priority] = true;
    }

    const SettingsPersistResult result = settings_.applyWifiStaPriorityUpdates(settingsUpdates);
    return result.success ? WifiClientApiService::PriorityUpdateStatus::Success
                          : WifiClientApiService::PriorityUpdateStatus::PersistFailed;
}

bool WiFiManager::testSavedNetwork(size_t index) {
    if (index >= kWifiStaSlotCount) {
        return false;
    }
    const V1Settings& settings = settings_.get();
    const WifiStaSlot& slot = settings.wifiStaSlots[index];
    if (!slot.isConfigured()) {
        return false;
    }

    if (!settings_.setWifiClientEnabled(true).success) {
        return false;
    }
    maintenanceManualDisconnect_ = false;
    cancelMaintenanceAutoConnect("slot_test");
    maintenanceAddressCollisionSlotMask_ &= static_cast<uint8_t>(~(1U << index));
    resetReconnectFailures();
    return connectToNetwork(slot.ssid, settings_.getWifiStaSlotPassword(index), false, static_cast<int>(index));
}

bool WiFiManager::connectToNetwork(const String& ssid, const String& password, bool persistCredentialsOnSuccess,
                                   int persistSlotIndex, bool maintenanceAutoConnect) {
    if (ssid.length() == 0) {
        Serial.println("[WiFiClient] Cannot connect: empty SSID");
        return false;
    }

    if (!maintenanceAutoConnect) {
        maintenanceManualDisconnect_ = false;
        cancelMaintenanceAutoConnect("manual_connect");
    }

    // Stage a non-blocking connect sequence to avoid stalling loop().
    pendingConnectSSID_ = ssid;
    pendingConnectPassword_ = password;
    pendingConnectPersistCredentials_ = persistCredentialsOnSuccess;
    pendingConnectSlotIndex_ = persistSlotIndex;
    currentConnectedSlotIndex_ = -1;
    wifiConnectStartMs_ = 0;
    wifiClientState_ = WIFI_CLIENT_CONNECTING;
    wifiConnectPhase_ = WifiConnectPhase::PREPARE_OFF;
    wifiConnectPhaseStartMs_ = millis();
    return true;
}

bool WiFiManager::enableWifiClientFromSavedCredentials() {
    struct EnableContext {
        WiFiManager* manager;
        WifiClientState priorState;
        int priorConnectedSlotIndex;
        WifiConnectPhase priorConnectPhase;
        unsigned long priorConnectPhaseStartMs;
        unsigned long priorConnectStartMs;
        String priorPendingSsid;
        String priorPendingPassword;
        bool priorPendingPersist;
        int priorPendingSlotIndex;
        MaintenanceAutoConnectPhase priorAutoConnectPhase;
        unsigned long priorAutoConnectScanStartMs;
        size_t priorAutoConnectSlots[kWifiStaSlotCount];
        size_t priorAutoConnectSlotCount;
        size_t priorAutoConnectSlotCursor;
        bool priorStaDropPending;
        unsigned long priorRetryAtMs;
        uint8_t priorCollisionSlotMask;
        bool priorManualDisconnect;
        wifi_mode_t priorMode;
        bool priorAutoReconnect;
    };

    const bool wasEnabled = settings_.get().wifiClientEnabled;
    EnableContext transaction{
        this,
        wifiClientState_,
        currentConnectedSlotIndex_,
        wifiConnectPhase_,
        wifiConnectPhaseStartMs_,
        wifiConnectStartMs_,
        pendingConnectSSID_,
        pendingConnectPassword_,
        pendingConnectPersistCredentials_,
        pendingConnectSlotIndex_,
        maintenanceAutoConnectPhase_,
        maintenanceAutoConnectScanStartMs_,
        {},
        maintenanceAutoConnectSlotCount_,
        maintenanceAutoConnectSlotCursor_,
        maintenanceAutoConnectStaDropGate_.pending(),
        maintenanceAutoConnectRetryAtMs_,
        maintenanceAddressCollisionSlotMask_,
        maintenanceManualDisconnect_,
        WiFi.getMode(),
        WiFi.getAutoReconnect(),
    };
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        transaction.priorAutoConnectSlots[i] = maintenanceAutoConnectSlots_[i];
    }

    WifiClientEnableTransaction::Runtime runtime;
    runtime.ctx = &transaction;
    runtime.persistedEnabled = wasEnabled;
    runtime.lifecycleAdmitted = wifiClientState_ == WIFI_CLIENT_CONNECTING ||
                                wifiClientState_ == WIFI_CLIENT_CONNECTED ||
                                maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::SCANNING ||
                                maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::CONNECTING;
    runtime.attemptStart = [](void* ctx) {
        auto* transaction = static_cast<EnableContext*>(ctx);
        WiFiManager* self = transaction->manager;
        if (self->maintenanceBootMode_) {
            if (self->beginMaintenanceAutoConnectScan(true)) {
                return true;
            }
            return !self->settings_.get().hasConfiguredWifiStaSlot();
        }

        const String savedSsid = self->settings_.get().wifiClientSSID;
        if (savedSsid.length() == 0) {
            self->wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
            self->currentConnectedSlotIndex_ = -1;
            return true;
        }

        if (self->connectToNetwork(savedSsid, self->settings_.getWifiClientPassword())) {
            return true;
        }

        self->wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
        self->currentConnectedSlotIndex_ = -1;
        return false;
    };
    runtime.rollbackFailedStart = [](void* ctx) {
        auto* transaction = static_cast<EnableContext*>(ctx);
        WiFiManager* self = transaction->manager;
        self->cancelMaintenanceAutoConnect("enable_persist_rollback");
        if (transaction->priorState != WIFI_CLIENT_CONNECTED && WiFi.status() == WL_CONNECTED) {
            WiFi.disconnect(false, false);
        }
        self->wifiClientState_ = transaction->priorState;
        self->currentConnectedSlotIndex_ = transaction->priorConnectedSlotIndex;
        self->wifiConnectPhase_ = transaction->priorConnectPhase;
        self->wifiConnectPhaseStartMs_ = transaction->priorConnectPhaseStartMs;
        self->wifiConnectStartMs_ = transaction->priorConnectStartMs;
        self->pendingConnectSSID_ = transaction->priorPendingSsid;
        self->pendingConnectPassword_ = transaction->priorPendingPassword;
        self->pendingConnectPersistCredentials_ = transaction->priorPendingPersist;
        self->pendingConnectSlotIndex_ = transaction->priorPendingSlotIndex;
        self->maintenanceAutoConnectPhase_ = transaction->priorAutoConnectPhase;
        self->maintenanceAutoConnectScanStartMs_ = transaction->priorAutoConnectScanStartMs;
        self->maintenanceAutoConnectSlotCount_ = transaction->priorAutoConnectSlotCount;
        self->maintenanceAutoConnectSlotCursor_ = transaction->priorAutoConnectSlotCursor;
        for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
            self->maintenanceAutoConnectSlots_[i] = transaction->priorAutoConnectSlots[i];
        }
        self->maintenanceAutoConnectStaDropGate_.clear();
        if (transaction->priorStaDropPending) {
            self->maintenanceAutoConnectStaDropGate_.request();
        }
        self->maintenanceAutoConnectRetryAtMs_ = transaction->priorRetryAtMs;
        self->maintenanceAddressCollisionSlotMask_ = transaction->priorCollisionSlotMask;
        self->maintenanceManualDisconnect_ = transaction->priorManualDisconnect;
        WiFi.setAutoReconnect(transaction->priorAutoReconnect);
        WiFi.mode(transaction->priorMode);
    };
    runtime.commitEnabled = [](void* ctx) {
        auto* transaction = static_cast<EnableContext*>(ctx);
        return transaction->manager->settings_.setWifiClientEnabled(true).success;
    };
    return WifiClientEnableTransaction::execute(runtime);
}

void WiFiManager::disconnectFromNetwork() {
    cancelMaintenanceAutoConnect("disconnect");

    Serial.println("[WiFiClient] Disconnecting from network");
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false); // Don't turn off station mode
    wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
    currentConnectedSlotIndex_ = -1;
    clearPendingWifiConnectState();
    maintenanceAutoConnectRetryAtMs_ = 0;
    maintenanceManualDisconnect_ = true;
}

void WiFiManager::clearPendingWifiConnectState() {
    wifiConnectPhase_ = WifiConnectPhase::IDLE;
    wifiConnectPhaseStartMs_ = 0;
    wifiConnectStartMs_ = 0;
    pendingConnectSSID_ = "";
    pendingConnectPassword_ = "";
    pendingConnectPersistCredentials_ = true;
    pendingConnectSlotIndex_ = -1;
}

void WiFiManager::disconnectTrackedWifiActivity(const char* reason, bool disableClientState) {
    Serial.printf("[WiFiClient] Canceling tracked STA activity: %s\n", reason ? reason : "mutation");
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    clearPendingWifiConnectState();
    currentConnectedSlotIndex_ = -1;
    wifiClientState_ = disableClientState ? WIFI_CLIENT_DISABLED : WIFI_CLIENT_DISCONNECTED;
    maintenanceAutoConnectStaDropGate_.request();
    applyDeferredMaintenanceStaRadioDrop();
    if (!disableClientState) {
        scheduleMaintenanceAutoConnectRetry(WIFI_MAINTENANCE_LINK_LOSS_RETRY_MS);
    }
}

bool WiFiManager::disableWifiClient() {
    if (!settings_.setWifiClientEnabled(false).success) {
        return false;
    }
    disconnectFromNetwork();
    wifiClientState_ = WIFI_CLIENT_DISABLED;
    maintenanceAutoConnectRetryAtMs_ = 0;
    WiFi.mode(WIFI_AP);
    return true;
}

bool WiFiManager::forgetWifiClient() {
    if (!settings_.clearWifiClientCredentials()) {
        return false;
    }
    cancelMaintenanceAutoConnect("forget");
    disconnectFromNetwork();
    wifiClientState_ = WIFI_CLIENT_DISABLED;
    maintenanceAutoConnectRetryAtMs_ = 0;
    maintenanceAddressCollisionSlotMask_ = 0;
    WiFi.mode(WIFI_AP);
    return true;
}

void WiFiManager::processWifiClientConnectPhase() {
    if (wifiConnectPhase_ == WifiConnectPhase::IDLE) {
        return;
    }

    unsigned long now = millis();
    switch (wifiConnectPhase_) {
    case WifiConnectPhase::PREPARE_OFF:
        if (isSetupModeActive()) {
            // Keep AP online and use a direct STA begin path.
            // Repeated STA resets in AP+STA mode have proven brittle on some routers.
            Serial.println("[WiFiClient] Preserving AP, preparing STA connect...");
            if (WiFi.getMode() != WIFI_AP_STA) {
                WiFi.mode(WIFI_AP_STA);
                wifiConnectPhaseStartMs_ = now;
                wifiConnectPhase_ = WifiConnectPhase::WAIT_AP_STA;
            } else {
                wifiConnectPhase_ = WifiConnectPhase::BEGIN_CONNECT;
            }
        } else {
            if (WiFi.getMode() != WIFI_OFF) {
                Serial.println("[WiFiClient] Cleaning up WiFi before reconnect...");
                WiFi.disconnect(false, false); // Graceful release without credential erase
                WiFi.mode(WIFI_OFF);           // Fully shut down WiFi driver
            }
            wifiConnectPhaseStartMs_ = now;
            wifiConnectPhase_ = WifiConnectPhase::WAIT_OFF;
        }
        break;

    case WifiConnectPhase::WAIT_OFF:
        if (now - wifiConnectPhaseStartMs_ >= WIFI_MODE_SWITCH_SETTLE_MS) {
            wifiConnectPhase_ = WifiConnectPhase::ENABLE_AP_STA;
        }
        break;

    case WifiConnectPhase::ENABLE_AP_STA:
        Serial.println("[WiFiClient] Initializing WiFi in AP+STA mode");
        WiFi.mode(WIFI_AP_STA);
        wifiConnectPhaseStartMs_ = now;
        wifiConnectPhase_ = WifiConnectPhase::WAIT_AP_STA;
        break;

    case WifiConnectPhase::WAIT_AP_STA:
        if (now - wifiConnectPhaseStartMs_ >= WIFI_MODE_SWITCH_SETTLE_MS) {
            wifiConnectPhase_ = WifiConnectPhase::BEGIN_CONNECT;
        }
        break;

    case WifiConnectPhase::BEGIN_CONNECT:
        if (pendingConnectSSID_.length() == 0) {
            wifiConnectPhase_ = WifiConnectPhase::IDLE;
            wifiClientState_ = WIFI_CLIENT_FAILED;
            pendingConnectSlotIndex_ = -1;
            currentConnectedSlotIndex_ = -1;
            break;
        }
        // Improve coexistence stability while connecting alongside BLE links.
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);
        Serial.println("[WiFiClient] Connecting to configured network");
        WiFi.begin(pendingConnectSSID_.c_str(), pendingConnectPassword_.c_str());
        wifiConnectStartMs_ = now;
        wifiConnectPhase_ = WifiConnectPhase::IDLE;
        break;

    case WifiConnectPhase::IDLE:
    default:
        break;
    }
}

bool WiFiManager::beginMaintenanceAutoConnectScan(bool explicitEnableRequest) {
    return WifiSetupNetworkPolicy::startMaintenanceAutoConnect(
        [this]() { return settings_.resolveStorageTransactionsForMutation(); },
        [this]() {
            Serial.println("[WiFiClient] Maintenance STA auto-connect deferred: storage recovery pending");
            scheduleMaintenanceAutoConnectRetry(WIFI_MAINTENANCE_RETRY_INTERVAL_MS);
        },
        [this, explicitEnableRequest]() {
            cancelMaintenanceAutoConnect("restart_scan");

            if (!maintenanceBootMode_) {
                return false;
            }

            const V1Settings& settings = settings_.get();
            if (explicitEnableRequest) {
                maintenanceAddressCollisionSlotMask_ = 0;
                maintenanceManualDisconnect_ = false;
            }
            if (!settings.hasConfiguredWifiStaSlot()) {
                Serial.println("[WiFiClient] Maintenance STA auto-connect skipped: no saved slots");
                wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
                return false;
            }
            if (!settings.wifiClientEnabled && !explicitEnableRequest) {
                Serial.println("[WiFiClient] Maintenance STA auto-connect skipped: client disabled");
                wifiClientState_ = WIFI_CLIENT_DISABLED;
                return false;
            }

            const WifiScanResultOwner::RequestResult requestResult =
                wifiScanOwner_.request(WifiScanConsumer::MAINTENANCE, makeWifiScanDriver());
            if (requestResult == WifiScanResultOwner::RequestResult::FAILED) {
                Serial.println("[WiFiClient] Maintenance STA auto-connect scan failed to start");
                finishMaintenanceAutoConnect("scan_start_failed", true);
                return false;
            }

            maintenanceAutoConnectStaDropGate_.clear();
            maintenanceAutoConnectRetryAtMs_ = 0;
            wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
            maintenanceAutoConnectPhase_ = MaintenanceAutoConnectPhase::SCANNING;
            maintenanceAutoConnectScanStartMs_ = millis();
            maintenanceAutoConnectSlotCount_ = 0;
            maintenanceAutoConnectSlotCursor_ = 0;
            Serial.println(requestResult == WifiScanResultOwner::RequestResult::JOINED
                               ? "[WiFiClient] Maintenance STA auto-connect joined the active scan"
                               : "[WiFiClient] Maintenance STA auto-connect scan started");
            return true;
        });
}

void WiFiManager::processMaintenanceAutoConnect() {
    const WifiScanResultOwner::HarvestResult harvestResult = wifiScanOwner_.harvest(makeWifiScanDriver());
    applyDeferredMaintenanceStaRadioDrop();

    if (maintenanceAutoConnectPhase_ != MaintenanceAutoConnectPhase::SCANNING) {
        return;
    }

    if (harvestResult == WifiScanResultOwner::HarvestResult::RUNNING) {
        const unsigned long now = millis();
        if (maintenanceAutoConnectScanStartMs_ != 0 &&
            (now - maintenanceAutoConnectScanStartMs_) >= WIFI_MAINTENANCE_SCAN_TIMEOUT_MS) {
            Serial.println("[WiFiClient] Maintenance STA auto-connect scan timed out");
            wifiScanOwner_.cancel(WifiScanConsumer::MAINTENANCE, makeWifiScanDriver());
            finishMaintenanceAutoConnect("scan_timeout", true);
        }
        return;
    }

    if (harvestResult == WifiScanResultOwner::HarvestResult::FAILED) {
        Serial.println("[WiFiClient] Maintenance STA auto-connect scan failed");
        finishMaintenanceAutoConnect("scan_failed", true);
        return;
    }

    if (!wifiScanOwner_.hasSnapshot(WifiScanConsumer::MAINTENANCE)) {
        Serial.println("[WiFiClient] Maintenance STA auto-connect scan results unavailable");
        finishMaintenanceAutoConnect("scan_results_unavailable", true);
        return;
    }

    const std::vector<ScannedNetwork> scannedNetworks =
        wifiScanOwner_.copySnapshot(WifiScanConsumer::MAINTENANCE, makeWifiScanDriver());
    wifiScanOwner_.clearSnapshot(WifiScanConsumer::MAINTENANCE);

    const V1Settings& settings = settings_.get();
    size_t ordered[kWifiStaSlotCount] = {};
    const size_t orderedCount = WifiStaSlotPolicy::orderConfiguredSlots(settings, ordered, kWifiStaSlotCount);
    maintenanceAutoConnectSlotCount_ = 0;
    maintenanceAutoConnectSlotCursor_ = 0;

    for (size_t orderedPos = 0; orderedPos < orderedCount; ++orderedPos) {
        const size_t slotIndex = ordered[orderedPos];
        if ((maintenanceAddressCollisionSlotMask_ & static_cast<uint8_t>(1U << slotIndex)) != 0) {
            continue;
        }
        const WifiStaSlot& slot = settings.wifiStaSlots[slotIndex];
        for (const ScannedNetwork& scannedNetwork : scannedNetworks) {
            if (scannedNetwork.ssid == slot.ssid) {
                maintenanceAutoConnectSlots_[maintenanceAutoConnectSlotCount_++] = slotIndex;
                break;
            }
        }
    }

    if (maintenanceAutoConnectSlotCount_ == 0) {
        Serial.printf("[WiFiClient] Maintenance STA auto-connect found no saved SSIDs in %u scan results\n",
                      static_cast<unsigned>(scannedNetworks.size()));
        finishMaintenanceAutoConnect("no_saved_ssid_in_range", true);
        return;
    }

    Serial.printf("[WiFiClient] Maintenance STA auto-connect has %u candidate slot(s)\n",
                  static_cast<unsigned>(maintenanceAutoConnectSlotCount_));
    maintenanceAutoConnectPhase_ = MaintenanceAutoConnectPhase::CONNECTING;
    if (!queueNextMaintenanceAutoConnectSlot()) {
        finishMaintenanceAutoConnect("candidate_queue_failed", true);
    }
}

bool WiFiManager::queueNextMaintenanceAutoConnectSlot() {
    if (maintenanceAutoConnectPhase_ != MaintenanceAutoConnectPhase::CONNECTING) {
        return false;
    }

    const V1Settings& settings = settings_.get();
    while (maintenanceAutoConnectSlotCursor_ < maintenanceAutoConnectSlotCount_) {
        const size_t slotIndex = maintenanceAutoConnectSlots_[maintenanceAutoConnectSlotCursor_++];
        if (slotIndex >= kWifiStaSlotCount) {
            continue;
        }
        if ((maintenanceAddressCollisionSlotMask_ & static_cast<uint8_t>(1U << slotIndex)) != 0) {
            continue;
        }

        const WifiStaSlot& slot = settings.wifiStaSlots[slotIndex];
        if (!slot.isConfigured()) {
            continue;
        }

        Serial.printf("[WiFiClient] Maintenance STA auto-connect trying slot %u\n", static_cast<unsigned>(slotIndex));
        if (connectToNetwork(slot.ssid, settings_.getWifiStaSlotPassword(slotIndex), false,
                             static_cast<int>(slotIndex), true)) {
            return true;
        }

        Serial.printf("[WiFiClient] Maintenance STA auto-connect failed to queue slot %u\n",
                      static_cast<unsigned>(slotIndex));
    }

    return false;
}

void WiFiManager::finishMaintenanceAutoConnect(const char* reason, bool dropStaRadio) {
    if (maintenanceAutoConnectPhase_ != MaintenanceAutoConnectPhase::IDLE) {
        Serial.printf("[WiFiClient] Maintenance STA auto-connect complete: %s\n",
                      (reason && reason[0] != '\0') ? reason : "done");
    }

    maintenanceAutoConnectPhase_ = MaintenanceAutoConnectPhase::COMPLETE;
    maintenanceAutoConnectScanStartMs_ = 0;
    maintenanceAutoConnectSlotCount_ = 0;
    maintenanceAutoConnectSlotCursor_ = 0;

    if (dropStaRadio) {
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
        maintenanceAutoConnectStaDropGate_.request();
        scheduleMaintenanceAutoConnectRetry(WIFI_MAINTENANCE_RETRY_INTERVAL_MS);
    } else {
        maintenanceAutoConnectStaDropGate_.clear();
        maintenanceAutoConnectRetryAtMs_ = 0;
    }
    applyDeferredMaintenanceStaRadioDrop();
}

void WiFiManager::applyDeferredMaintenanceStaRadioDrop() {
    if (!maintenanceAutoConnectStaDropGate_.takeIfReady(wifiScanOwner_.isRunning())) {
        return;
    }

    if (isSetupModeActive() && wifiClientState_ != WIFI_CLIENT_CONNECTED &&
        wifiClientState_ != WIFI_CLIENT_CONNECTING) {
        const wifi_mode_t mode = WiFi.getMode();
        if (mode == WIFI_AP_STA || mode == WIFI_STA) {
            Serial.println("[WiFiClient] Maintenance STA unavailable; returning to AP-only mode");
            WiFi.mode(WIFI_AP);
        }
        wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
    }
}

void WiFiManager::cancelMaintenanceAutoConnect(const char* reason) {
    if (maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::IDLE ||
        maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::COMPLETE) {
        return;
    }

    Serial.printf("[WiFiClient] Maintenance STA auto-connect canceled: %s\n",
                  (reason && reason[0] != '\0') ? reason : "unknown");
    const bool wasScanning = maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::SCANNING;
    const bool wasConnecting = maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::CONNECTING;
    if (wasScanning) {
        wifiScanOwner_.cancel(WifiScanConsumer::MAINTENANCE, makeWifiScanDriver());
    }
    if (wasConnecting) {
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
        clearPendingWifiConnectState();
        currentConnectedSlotIndex_ = -1;
        wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
    }
    maintenanceAutoConnectPhase_ = MaintenanceAutoConnectPhase::IDLE;
    maintenanceAutoConnectScanStartMs_ = 0;
    maintenanceAutoConnectSlotCount_ = 0;
    maintenanceAutoConnectSlotCursor_ = 0;
    maintenanceAutoConnectStaDropGate_.request();
    applyDeferredMaintenanceStaRadioDrop();
}

void WiFiManager::scheduleMaintenanceAutoConnectRetry(unsigned long delayMs) {
    if (!maintenanceBootMode_ || !settings_.get().wifiClientEnabled ||
        !settings_.get().hasConfiguredWifiStaSlot()) {
        maintenanceAutoConnectRetryAtMs_ = 0;
        return;
    }
    maintenanceAutoConnectRetryAtMs_ = millis() + delayMs;
    if (maintenanceAutoConnectRetryAtMs_ == 0) {
        maintenanceAutoConnectRetryAtMs_ = 1;
    }
}

bool WiFiManager::maintenanceAddressCollision() const {
    if (!maintenanceBootMode_ || !isSetupModeActive()) {
        return false;
    }
    return WifiMaintenanceInterfacePolicy::hasAddressCollision(static_cast<uint32_t>(WiFi.softAPIP()),
                                                                static_cast<uint32_t>(WiFi.localIP()));
}

void WiFiManager::checkWifiClientStatus() {
    // Skip if WiFi client is disabled
    if (wifiClientState_ == WIFI_CLIENT_DISABLED) {
        if (maintenanceBootMode_ && WiFi.status() == WL_CONNECTED) {
            Serial.println("[WiFiClient] Disconnecting unexpected physical STA while client is disabled");
            WiFi.setAutoReconnect(false);
            WiFi.disconnect(false, false);
            if (!wifiScanOwner_.isRunning() && isSetupModeActive()) {
                WiFi.mode(WIFI_AP);
            }
        }
        return;
    }

    wl_status_t status = WiFi.status();

    if (WifiMaintenanceLinkPolicy::evaluate({
            .physicalConnected = status == WL_CONNECTED,
            .autoJoinSuppressed = maintenanceManualDisconnect_,
        }) == WifiMaintenanceLinkPolicy::Decision::RejectSuppressedPhysicalConnection) {
        Serial.println("[WiFiClient] Rejecting late STA connection after explicit disconnect");
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
        clearPendingWifiConnectState();
        currentConnectedSlotIndex_ = -1;
        wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
        maintenanceAutoConnectRetryAtMs_ = 0;
        maintenanceAutoConnectStaDropGate_.request();
        applyDeferredMaintenanceStaRadioDrop();
        return;
    }

    if (WifiMaintenanceLinkPolicy::shouldDisconnectAddressCollision(status == WL_CONNECTED,
                                                                    maintenanceAddressCollision())) {
        int collisionSlot = pendingConnectSlotIndex_;
        if (collisionSlot < 0 || static_cast<size_t>(collisionSlot) >= kWifiStaSlotCount) {
            collisionSlot = findConfiguredSlotBySsid(WiFi.SSID());
        }
        if (collisionSlot >= 0 && static_cast<size_t>(collisionSlot) < kWifiStaSlotCount) {
            maintenanceAddressCollisionSlotMask_ |= static_cast<uint8_t>(1U << collisionSlot);
        }

        const bool wasMaintenanceCandidate =
            maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::CONNECTING;
        Serial.println("[WiFiClient] STA address collides with maintenance AP; disconnecting STA");
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
        clearPendingWifiConnectState();
        wifiClientState_ = WIFI_CLIENT_FAILED;
        currentConnectedSlotIndex_ = -1;
        if (wasMaintenanceCandidate) {
            if (!queueNextMaintenanceAutoConnectSlot()) {
                finishMaintenanceAutoConnect("address_collision", true);
            }
        } else {
            cancelMaintenanceAutoConnect("address_collision");
            scheduleMaintenanceAutoConnectRetry(WIFI_MAINTENANCE_RETRY_INTERVAL_MS);
        }
        return;
    }

    switch (wifiClientState_) {
    case WIFI_CLIENT_CONNECTING: {
        // Non-blocking mode transition is still in progress.
        if (wifiConnectPhase_ != WifiConnectPhase::IDLE || wifiConnectStartMs_ == 0) {
            break;
        }

        if (status == WL_CONNECTED) {
            wifiClientState_ = WIFI_CLIENT_CONNECTED;
            wifiConnectStartMs_ = 0;
            Serial.println("[WiFiClient] Connected");
            if (isSetupModeActive()) {
                // Arm AP idle timer from STA connect so setup UI clients have
                // a full grace window before AP retirement.
                lastClientSeenMs_ = millis();
                Serial.println("[WiFiClient] STA connected; AP idle-retire timer armed");
            }

            // Reset failure counter on successful connection
            wifiReconnectFailures_ = 0;

            // Save credentials on successful connection
            if (pendingConnectSSID_.length() > 0) {
                const bool hasPendingSlot =
                    pendingConnectSlotIndex_ >= 0 && static_cast<size_t>(pendingConnectSlotIndex_) < kWifiStaSlotCount;
                if (pendingConnectPersistCredentials_) {
                    if (hasPendingSlot) {
                        const size_t slotIndex = static_cast<size_t>(pendingConnectSlotIndex_);
                        const V1Settings& currentSettings = settings_.get();
                        const WifiStaSlot& currentSlot = currentSettings.wifiStaSlots[slotIndex];
                        const String label = currentSlot.label;
                        const uint8_t priority =
                            currentSlot.isConfigured() ? currentSlot.priority : static_cast<uint8_t>(slotIndex);
                        settings_.setWifiStaSlotCredentials(slotIndex, pendingConnectSSID_,
                                                                  pendingConnectPassword_, label, priority);
                    } else {
                        const V1Settings& currentSettings = settings_.get();
                        const bool ssidChanged = (pendingConnectSSID_ != currentSettings.wifiClientSSID);
                        const bool passwordChanged =
                            (pendingConnectPassword_ != settings_.getWifiClientPassword());
                        if (ssidChanged || passwordChanged) {
                            settings_.setWifiClientCredentials(pendingConnectSSID_, pendingConnectPassword_);
                        } else {
                            Serial.println("[WiFiClient] Connected with unchanged credentials; skipping re-save");
                        }
                    }
                } else {
                    Serial.println("[WiFiClient] Connected via auto-reconnect; skipping credential re-save");
                }
                if (hasPendingSlot) {
                    currentConnectedSlotIndex_ = pendingConnectSlotIndex_;
                    settings_.markWifiStaSlotConnected(static_cast<size_t>(pendingConnectSlotIndex_),
                                                             static_cast<uint32_t>(millis() / 1000UL));
                } else {
                    currentConnectedSlotIndex_ = findConfiguredSlotBySsid(pendingConnectSSID_);
                }
                if (maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::CONNECTING) {
                    finishMaintenanceAutoConnect("connected", false);
                }
                pendingConnectSSID_ = "";
                pendingConnectPassword_ = "";
                pendingConnectPersistCredentials_ = true;
                pendingConnectSlotIndex_ = -1;
            }
        } else if (status == WL_CONNECT_FAILED || status == WL_NO_SSID_AVAIL) {
            const bool wasMaintenanceAutoConnect =
                maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::CONNECTING;
            wifiClientState_ = WIFI_CLIENT_FAILED;
            currentConnectedSlotIndex_ = -1;
            Serial.printf("[WiFiClient] Connection failed: %d\n", status);
            wifiConnectStartMs_ = 0;

            pendingConnectSSID_ = "";
            pendingConnectPassword_ = "";
            pendingConnectPersistCredentials_ = true;
            pendingConnectSlotIndex_ = -1;
            if (wasMaintenanceAutoConnect) {
                if (queueNextMaintenanceAutoConnectSlot()) {
                    break;
                }
                finishMaintenanceAutoConnect("all_candidates_failed", true);
            }
        } else if (millis() - wifiConnectStartMs_ > WIFI_CONNECT_TIMEOUT_MS) {
            const bool wasMaintenanceAutoConnect =
                maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::CONNECTING;
            wifiClientState_ = WIFI_CLIENT_FAILED;
            currentConnectedSlotIndex_ = -1;
            Serial.println("[WiFiClient] Connection timeout");
            WiFi.disconnect(false);
            wifiConnectStartMs_ = 0;

            pendingConnectSSID_ = "";
            pendingConnectPassword_ = "";
            pendingConnectPersistCredentials_ = true;
            pendingConnectSlotIndex_ = -1;
            if (wasMaintenanceAutoConnect) {
                if (queueNextMaintenanceAutoConnectSlot()) {
                    break;
                }
                finishMaintenanceAutoConnect("all_candidates_timeout", true);
            }
        }
        break;
    }

    case WIFI_CLIENT_CONNECTED: {
        if (status != WL_CONNECTED) {
            wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
            currentConnectedSlotIndex_ = -1;
            Serial.println("[WiFiClient] Lost connection");
            scheduleMaintenanceAutoConnectRetry(WIFI_MAINTENANCE_LINK_LOSS_RETRY_MS);
        }
        break;
    }

    case WIFI_CLIENT_DISCONNECTED:
    case WIFI_CLIENT_FAILED: {
        if (lowDmaCooldownRemainingMs() > 0) {
            break;
        }

        if (maintenanceBootMode_) {
            const V1Settings& settings = settings_.get();
            const bool autoConnectActive = maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::SCANNING ||
                                           maintenanceAutoConnectPhase_ == MaintenanceAutoConnectPhase::CONNECTING;
            const WifiMaintenanceLinkPolicy::Decision decision = WifiMaintenanceLinkPolicy::evaluate({
                .physicalConnected = status == WL_CONNECTED,
                .appConnected = false,
                .appConnecting = false,
                .autoConnectActive = autoConnectActive,
                .clientEnabled = settings.wifiClientEnabled,
                .hasSavedCandidates = settings.hasConfiguredWifiStaSlot(),
                .autoJoinSuppressed = maintenanceManualDisconnect_,
                .nowMs = static_cast<uint32_t>(millis()),
                .retryAtMs = static_cast<uint32_t>(maintenanceAutoConnectRetryAtMs_),
            });
            if (decision == WifiMaintenanceLinkPolicy::Decision::ReconcilePhysicalConnection) {
                const int slotIndex = findConfiguredSlotBySsid(WiFi.SSID());
                if (slotIndex >= 0) {
                    cancelMaintenanceAutoConnect("physical_reconnect");
                    wifiClientState_ = WIFI_CLIENT_CONNECTED;
                    currentConnectedSlotIndex_ = slotIndex;
                    wifiReconnectFailures_ = 0;
                    maintenanceAutoConnectRetryAtMs_ = 0;
                    Serial.println("[WiFiClient] Reconciled framework STA auto-reconnect");
                } else {
                    Serial.println("[WiFiClient] Rejecting physical connection to unsaved network");
                    WiFi.setAutoReconnect(false);
                    WiFi.disconnect(false, false);
                    scheduleMaintenanceAutoConnectRetry(WIFI_MAINTENANCE_LINK_LOSS_RETRY_MS);
                }
            } else if (decision == WifiMaintenanceLinkPolicy::Decision::StartCandidateScan) {
                if (!beginMaintenanceAutoConnectScan(false)) {
                    scheduleMaintenanceAutoConnectRetry(WIFI_MAINTENANCE_RETRY_INTERVAL_MS);
                }
            }
            break;
        }

        // Defer background STA reconnect attempts during early boot until V1 is
        // connected. This protects BLE acquisition from AP+STA mode churn.
        const bool v1Connected = isV1Connected_ ? isV1Connected_(isV1ConnectedCtx_)
                                                : (bleRuntime_ && bleRuntime_->isConnected());
        const uint32_t bootNowMs = millis();
        const WifiReconnectPolicy::BootDecision bootDecision = WifiReconnectPolicy::evaluateBoot({
            v1Connected,
            setupModeStartTime_ != 0,
            bootNowMs,
            static_cast<uint32_t>(setupModeStartTime_),
            WIFI_RECONNECT_DEFER_NO_V1_MS,
            wifiReconnectDeferredLogged_,
        });

        if (bootDecision.logDeferred) {
            Serial.printf("[WiFiClient] Auto-reconnect deferred (waiting for V1 or %lu ms grace)\n",
                          (unsigned long)WIFI_RECONNECT_DEFER_NO_V1_MS);
        }
        if (bootDecision.logResumed) {
            Serial.println("[WiFiClient] Auto-reconnect resumed");
        }
        wifiReconnectDeferredLogged_ = bootDecision.deferredLogged;
        if (bootDecision.action == WifiReconnectPolicy::BootAction::DEFER_FOR_V1) {
            break;
        }

        // Auto-reconnect if we have saved credentials (with failure limit).
        const V1Settings& settings = settings_.get();
        const uint32_t retryNowMs = millis();
        const WifiReconnectPolicy::AttemptDecision reconnectDecision = WifiReconnectPolicy::evaluateAttempt({
            settings.wifiClientEnabled && settings.wifiClientSSID.length() > 0,
            wasAutoStarted_,
            cachedApStaCount_ > 0,
            lastUiActivityMs_ != 0,
            wifiReconnectFailures_,
            WIFI_MAX_RECONNECT_FAILURES,
            retryNowMs,
            static_cast<uint32_t>(lastReconnectAttemptMs_),
            WIFI_RECONNECT_INTERVAL_MS,
        });

        if (reconnectDecision.action != WifiReconnectPolicy::AttemptAction::ATTEMPT &&
            reconnectDecision.action != WifiReconnectPolicy::AttemptAction::GIVE_UP) {
            break;
        }

        const String savedPassword = settings_.getWifiClientPassword();
        lastReconnectAttemptMs_ = retryNowMs;
        wifiReconnectFailures_ = reconnectDecision.nextFailures;

        if (reconnectDecision.action == WifiReconnectPolicy::AttemptAction::GIVE_UP) {
            Serial.printf("[WiFiClient] Giving up after %d failed attempts. Use BOOT button to retry.\n",
                          wifiReconnectFailures_);
            // Stay in FAILED state, user must toggle WiFi to retry
            break;
        }

        Serial.printf("[WiFiClient] Auto-reconnect attempt %d/%d...\n", wifiReconnectFailures_,
                      WIFI_MAX_RECONNECT_FAILURES);
        connectToNetwork(settings.wifiClientSSID, savedPassword, false);
        break;
    }

    default:
        break;
    }
}
