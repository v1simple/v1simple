/**
 * WiFi STA connection, scan, and reconnect lifecycle.
 */

#include "wifi_manager_internals.h"
#include "ble_client.h"
#include "settings.h"
#include "settings_sanitize.h"
#include "modules/wifi/wifi_client_api_service.h"
#include <algorithm>
#include <vector>

String WiFiManager::getAPIPAddress() const {
    if (isSetupModeActive()) {
        return WiFi.softAPIP().toString();
    }
    return "";
}

String WiFiManager::getIPAddress() const {
    if (station_.clientState() == WIFI_CLIENT_CONNECTED) {
        return WiFi.localIP().toString();
    }
    return "";
}

bool WiFiManager::startWifiScan() {
    const WifiScanResultOwner::RequestResult result =
        wifiScanOwner_.request(WifiScanConsumer::UI, WifiStationLifecycle::scanDriver());
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
    const WifiScanResultOwner::HarvestResult result = wifiScanOwner_.harvest(WifiStationLifecycle::scanDriver());
    if (result == WifiScanResultOwner::HarvestResult::FAILED) {
        Serial.println("[WiFiClient] Active network scan failed");
    }
    return result == WifiScanResultOwner::HarvestResult::RUNNING;
}

bool WiFiManager::hasCompletedWifiScanResults() {
    const WifiScanResultOwner::HarvestResult result = wifiScanOwner_.harvest(WifiStationLifecycle::scanDriver());
    if (result == WifiScanResultOwner::HarvestResult::FAILED) {
        Serial.println("[WiFiClient] Active network scan failed");
    }
    return wifiScanOwner_.hasSnapshot(WifiScanConsumer::UI);
}

std::vector<ScannedNetwork> WiFiManager::getScannedNetworks() {
    const WifiScanResultOwner::HarvestResult result = wifiScanOwner_.harvest(WifiStationLifecycle::scanDriver());
    if (result == WifiScanResultOwner::HarvestResult::FAILED) {
        Serial.println("[WiFiClient] Active network scan failed");
        return {};
    }

    std::vector<ScannedNetwork> networks = wifiScanOwner_.copySnapshot(WifiScanConsumer::UI, WifiStationLifecycle::scanDriver());
    if (wifiScanOwner_.hasSnapshot(WifiScanConsumer::UI)) {
        Serial.printf("[WiFiClient] UI scan snapshot has %u network(s)\n", static_cast<unsigned>(networks.size()));
    }
    return networks;
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
        if (payload.configured) {
            bool nvsReadFailed = false;
            const String password = settings_.getWifiStaSlotPassword(i, &nvsReadFailed);
            // An unavailable credential must not turn a blank editor field
            // into an explicit request to replace it with an empty password.
            payload.hasPassword = nvsReadFailed || password.length() > 0;
        }
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

WifiClientApiService::SlotActionStatus WiFiManager::upsertSavedNetwork(
    const WifiClientApiService::SavedNetworkUpsertPayload& request, size_t& indexOut) {
    using WifiClientApiService::SlotActionStatus;
    const String ssid = sanitizeWifiClientSsidValue(request.ssid);
    if (ssid.length() == 0 || request.hasIndex != request.hasExpectedSsid) {
        return SlotActionStatus::Failed;
    }

    int selectedIndex = -1;
    const V1Settings& settings = settings_.get();
    if (request.hasIndex) {
        if (request.index >= kWifiStaSlotCount) {
            return SlotActionStatus::Failed;
        }
        selectedIndex = static_cast<int>(request.index);
    } else {
        selectedIndex = selectSlotForNetworkConnect(ssid);
    }

    if (selectedIndex < 0 || static_cast<size_t>(selectedIndex) >= kWifiStaSlotCount) {
        return SlotActionStatus::Failed;
    }

    const size_t index = static_cast<size_t>(selectedIndex);
    const WifiStaSlot& currentSlot = settings.wifiStaSlots[index];
    // The maintenance dispatcher resolves storage before calling this owner.
    // An unindexed request creates a new entry; a known slot needs its identity.
    if (request.hasIndex ? request.expectedSsid != currentSlot.ssid : currentSlot.isConfigured()) {
        return SlotActionStatus::Conflict;
    }
    uint8_t priority = currentSlot.priority;
    if (request.hasPriority) {
        priority = request.priority;
    } else if (!currentSlot.isConfigured()) {
        priority = 0;
        for (const WifiStaSlot& slot : settings.wifiStaSlots) {
            if (slot.isConfigured()) {
                if (slot.priority == UINT8_MAX) return SlotActionStatus::Failed;
                priority = std::max(priority, static_cast<uint8_t>(slot.priority + 1));
            }
        }
    }
    const auto mutation = station_.savedNetworkMutation(index);
    bool nvsReadFailed = false;
    String savedPassword;
    if (!request.hasPassword) savedPassword = settings_.getWifiStaSlotPassword(index, &nvsReadFailed);
    if (nvsReadFailed) return SlotActionStatus::Failed;
    const String& password = request.hasPassword ? request.password : savedPassword;
    const String& label = request.hasLabel ? request.label : currentSlot.label;
    if (!settings_.setWifiStaSlotCredentials(index, ssid, password, label, priority)) {
        return SlotActionStatus::Failed;
    }
    station_.savedNetworkMutationCommitted(stationEnvironment(), index, mutation, "slot_upsert");
    indexOut = index;
    return SlotActionStatus::Success;
}

bool WiFiManager::savedNetworkSlotMatches(const WifiClientApiService::SavedNetworkSlotRequest& request) const {
    return request.index < kWifiStaSlotCount && request.expectedSsid.length() != 0 &&
           settings_.get().wifiStaSlots[request.index].ssid == request.expectedSsid;
}

WifiClientApiService::SlotActionStatus WiFiManager::deleteSavedNetwork(
    const WifiClientApiService::SavedNetworkSlotRequest& request) {
    using WifiClientApiService::SlotActionStatus;
    if (!savedNetworkSlotMatches(request)) return SlotActionStatus::Conflict;
    const size_t index = request.index;
    const auto mutation = station_.savedNetworkMutation(index);
    if (!settings_.clearWifiStaSlot(index)) {
        return SlotActionStatus::Failed;
    }
    station_.savedNetworkMutationCommitted(stationEnvironment(), index, mutation, "slot_delete");
    return SlotActionStatus::Success;
}

WifiClientApiService::PriorityUpdateStatus WiFiManager::updateSavedNetworkPriorities(
    const std::vector<WifiClientApiService::SavedNetworkPriorityUpdate>& updates) {
    using WifiClientApiService::PriorityUpdateStatus;
    if (updates.empty() || updates.size() > kWifiStaSlotCount) return PriorityUpdateStatus::Invalid;

    bool seenIndices[kWifiStaSlotCount] = {};
    bool seenPriorities[kWifiStaSlotCount] = {};
    const V1Settings& current = settings_.get();

    std::vector<WifiStaPriorityUpdate> settingsUpdates;
    settingsUpdates.reserve(updates.size());
    for (const auto& update : updates) {
        if (update.index >= kWifiStaSlotCount || update.priority >= kWifiStaSlotCount || seenIndices[update.index] ||
            seenPriorities[update.priority]) {
            return PriorityUpdateStatus::Invalid;
        }
        const WifiStaSlot& slot = current.wifiStaSlots[update.index];
        if (!slot.isConfigured() || slot.ssid != update.expectedSsid || slot.priority != update.expectedPriority) {
            return PriorityUpdateStatus::Conflict;
        }
        seenIndices[update.index] = true;
        seenPriorities[update.priority] = true;
        if (slot.priority != update.priority) {
            settingsUpdates.push_back(WifiStaPriorityUpdate{update.index, update.priority});
        }
    }

    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        if (current.wifiStaSlots[index].isConfigured() && !seenIndices[index]) {
            return PriorityUpdateStatus::Conflict;
        }
    }

    if (settingsUpdates.empty()) return PriorityUpdateStatus::Success;
    const SettingsPersistResult result = settings_.applyWifiStaPriorityUpdates(settingsUpdates);
    return result.success ? PriorityUpdateStatus::Success : PriorityUpdateStatus::PersistFailed;
}

bool WiFiManager::disableWifiClient() {
    if (!settings_.setWifiClientEnabled(false).success) {
        return false;
    }
    station_.disableCommitted(stationEnvironment());
    return true;
}

bool WiFiManager::forgetWifiClient() {
    if (!settings_.clearWifiClientCredentials()) {
        return false;
    }
    station_.forgetCommitted(stationEnvironment());
    return true;
}

WifiStationLifecycle::Environment WiFiManager::stationEnvironment() const {
    return {maintenanceBootMode_, isSetupModeActive(), lowDmaCooldownRemainingMs(),
            isV1Connected_ ? isV1Connected_(isV1ConnectedCtx_) : (bleRuntime_ && bleRuntime_->isConnected()),
            setupModeStartTime_, wasAutoStarted_, cachedApStaCount_ > 0, lastUiActivityMs_ != 0};
}

bool WiFiManager::connectToNetwork(const String& ssid, const String& password, bool persist, int slot, bool automatic) {
    return station_.connectToNetwork(stationEnvironment(), ssid, password, persist, slot, automatic);
}
void WiFiManager::disconnectFromNetwork() { station_.disconnectFromNetwork(stationEnvironment()); }
void WiFiManager::checkWifiClientStatus() { station_.checkWifiClientStatus(stationEnvironment()); }
WifiClientApiService::SlotActionStatus WiFiManager::testSavedNetwork(
    const WifiClientApiService::SavedNetworkSlotRequest& request) {
    using WifiClientApiService::SlotActionStatus;
    if (!savedNetworkSlotMatches(request)) return SlotActionStatus::Conflict;
    return station_.testSavedNetwork(stationEnvironment(), request.index) ? SlotActionStatus::Success
                                                                         : SlotActionStatus::Failed;
}
bool WiFiManager::enableWifiClientFromSavedCredentials() {
    return station_.enableWifiClientFromSavedCredentials(stationEnvironment());
}
