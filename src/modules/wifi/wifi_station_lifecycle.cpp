#include "wifi_station_lifecycle.h"
#include "settings.h"
#include "settings_sanitize.h"
#include "wifi_client_enable_transaction.h"
#include "wifi_maintenance_interface_policy.h"
#include "wifi_maintenance_link_policy.h"
#include "wifi_reconnect_policy.h"
#include "wifi_setup_network_policy.h"
#include "modules/wifi/wifi_sta_slot_policy.h"
#include <WiFi.h>
#include <algorithm>
#include <cstring>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>

namespace {

// Arduino's cached status survives begin(), including the preceding candidate's
// failure. Only a failure event for the armed SSID belongs to this attempt.
// The listener has process lifetime and never captures a WiFiManager instance.
portMUX_TYPE wifiAttemptMux = portMUX_INITIALIZER_UNLOCKED;
char wifiAttemptSsid[33] = {};
size_t wifiAttemptSsidLength = 0;
wl_status_t wifiAttemptFailure = WL_IDLE_STATUS;

void observeWifiAttemptEvent(arduino_event_id_t, arduino_event_info_t info) {
    const auto& disconnected = info.wifi_sta_disconnected;
    // Only these reasons can publish a terminal status in Arduino's system
    // listener. AUTH_EXPIRE deliberately leaves its old status untouched while
    // retrying, so even a matching SSID does not make that cached failure fresh.
    if (disconnected.reason != WIFI_REASON_NO_AP_FOUND && disconnected.reason != WIFI_REASON_AUTH_FAIL &&
        disconnected.reason != WIFI_REASON_ASSOC_FAIL) {
        return;
    }
    // The system listener runs first; retain its initial-auth retry policy.
    const wl_status_t status = WiFi.status();
    if (status != WL_CONNECT_FAILED && status != WL_NO_SSID_AVAIL) {
        return;
    }
    portENTER_CRITICAL(&wifiAttemptMux);
    if (wifiAttemptSsidLength != 0 && disconnected.ssid_len == wifiAttemptSsidLength &&
        memcmp(disconnected.ssid, wifiAttemptSsid, wifiAttemptSsidLength) == 0) {
        wifiAttemptFailure = status;
    }
    portEXIT_CRITICAL(&wifiAttemptMux);
}

void startWifiAttempt(const String& ssid, const String& password) {
    static const auto listener = WiFi.onEvent(observeWifiAttemptEvent, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    (void)listener;
    portENTER_CRITICAL(&wifiAttemptMux);
    wifiAttemptSsidLength = std::min(static_cast<size_t>(ssid.length()), sizeof(wifiAttemptSsid) - 1);
    memcpy(wifiAttemptSsid, ssid.c_str(), wifiAttemptSsidLength);
    wifiAttemptSsid[wifiAttemptSsidLength] = '\0';
    wifiAttemptFailure = WL_IDLE_STATUS;
    portEXIT_CRITICAL(&wifiAttemptMux);

    // These are the same operations as WiFi.begin(), but their bool results
    // distinguish a synchronous start failure from an old cached status.
    if (!WiFi.STA.begin() || !WiFi.STA.connect(ssid.c_str(), password.c_str())) {
        portENTER_CRITICAL(&wifiAttemptMux);
        wifiAttemptFailure = WL_CONNECT_FAILED;
        portEXIT_CRITICAL(&wifiAttemptMux);
    }
}

wl_status_t currentWifiAttemptFailure() {
    portENTER_CRITICAL(&wifiAttemptMux);
    const wl_status_t failure = wifiAttemptFailure;
    portEXIT_CRITICAL(&wifiAttemptMux);
    return failure;
}

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

} // namespace

WifiScanResultOwner::Driver WifiStationLifecycle::scanDriver() {
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


int WifiStationLifecycle::findConfiguredSlotBySsid(const String& ssid) const {
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

bool WifiStationLifecycle::savedSlotMatches(int index, const String& ssid) const {
    return index >= 0 && static_cast<size_t>(index) < kWifiStaSlotCount && ssid.length() > 0 &&
           settings_.get().wifiStaSlots[index].ssid == ssid;
}

int WifiStationLifecycle::connectedSlotForSsid(const String& ssid) const {
    return settings_.get().wifiClientEnabled && savedSlotMatches(state_.connectedSlot, ssid)
               ? state_.connectedSlot
               : -1;
}

bool WifiStationLifecycle::testSavedNetwork(const Environment& environment, size_t index) {
    if (index >= kWifiStaSlotCount) {
        return false;
    }
    const V1Settings& settings = settings_.get();
    const WifiStaSlot& slot = settings.wifiStaSlots[index];
    if (!slot.isConfigured()) {
        return false;
    }

    bool credentialUnavailable = false;
    const String password = settings_.getWifiStaSlotPassword(index, &credentialUnavailable);
    if (credentialUnavailable) return false;

    if (!settings_.setWifiClientEnabled(true).success) {
        return false;
    }
    state_.manualDisconnect = false;
    cancelMaintenanceAutoConnect(environment, "slot_test");
    state_.candidates.collisionMask &= static_cast<uint8_t>(~(1U << index));
    resetReconnectFailures();
    if (!connectToNetwork(environment, slot.ssid, password, false, static_cast<int>(index))) return false;
    state_.pending.requiresSavedSlot = true;
    return true;
}

bool WifiStationLifecycle::connectToNetwork(const Environment& environment, const String& ssid, const String& password, bool persistCredentialsOnSuccess,
                                   int persistSlotIndex, bool maintenanceAutoConnect) {
    if (ssid.length() == 0) {
        Serial.println("[WiFiClient] Cannot connect: empty SSID");
        return false;
    }

    if (!maintenanceAutoConnect) {
        state_.manualDisconnect = false;
        cancelMaintenanceAutoConnect(environment, "manual_connect");
    }

    // Stage a non-blocking connect sequence to avoid stalling loop().
    state_.pending.ssid = ssid;
    state_.pending.password = password;
    state_.pending.persistCredentials = persistCredentialsOnSuccess;
    state_.pending.requiresSavedSlot = maintenanceAutoConnect;
    state_.pending.slot = persistSlotIndex;
    state_.connectedSlot = -1;
    state_.pending.startedAt = 0;
    state_.client = WIFI_CLIENT_CONNECTING;
    state_.pending.phase = WifiConnectPhase::PREPARE_OFF;
    state_.pending.phaseStartedAt = millis();
    return true;
}

void WifiStationLifecycle::disconnectFromNetwork(const Environment& environment) {
    cancelMaintenanceAutoConnect(environment, "disconnect");

    Serial.println("[WiFiClient] Disconnecting from network");
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false); // Don't turn off station mode
    state_.client = WIFI_CLIENT_DISCONNECTED;
    state_.connectedSlot = -1;
    clearPendingWifiConnectState();
    state_.candidates.retryAt = 0;
    state_.manualDisconnect = true;
}

void WifiStationLifecycle::clearPendingWifiConnectState() {
    state_.pending = PendingAttempt{};
}

void WifiStationLifecycle::disconnectTrackedWifiActivity(const Environment& environment, const char* reason, bool disableClientState) {
    Serial.printf("[WiFiClient] Canceling tracked STA activity: %s\n", reason ? reason : "mutation");
    WiFi.setAutoReconnect(false);
    WiFi.disconnect(false, false);
    clearPendingWifiConnectState();
    state_.connectedSlot = -1;
    state_.client = disableClientState ? WIFI_CLIENT_DISABLED : WIFI_CLIENT_DISCONNECTED;
    state_.candidates.staDropGate.request();
    applyDeferredMaintenanceStaRadioDrop(environment);
    if (disableClientState) {
        state_.candidates.retryAt = 0;
    } else {
        scheduleMaintenanceAutoConnectRetry(environment, WIFI_MAINTENANCE_LINK_LOSS_RETRY_MS);
    }
}

bool WifiStationLifecycle::cancelObsoleteSavedNetworkAttempt(const Environment& environment) {
    const PendingAttempt& attempt = state_.pending;
    if (state_.client != WIFI_CLIENT_CONNECTING || !attempt.requiresSavedSlot) {
        return false;
    }
    const bool enabled = settings_.get().wifiClientEnabled;
    if (enabled && savedSlotMatches(attempt.slot, attempt.ssid)) {
        return false;
    }

    // A restore can replace a slot without going through the saved-network
    // mutation callback. Retire its attempt before using the stale credentials
    // or attributing a late association to the new occupant.
    if (attempt.slot >= 0 && static_cast<size_t>(attempt.slot) < kWifiStaSlotCount) {
        state_.candidates.collisionMask &= static_cast<uint8_t>(~(1U << attempt.slot));
    }
    const int liveSlot = enabled && attempt.phase != WifiConnectPhase::IDLE && WiFi.status() == WL_CONNECTED
                             ? findConfiguredSlotBySsid(WiFi.SSID())
                             : -1;
    if (state_.candidates.phase == MaintenanceAutoConnectPhase::CONNECTING) {
        finishMaintenanceAutoConnect(environment, "saved_network_changed", false);
    }
    if (liveSlot >= 0) {
        // The radio has not started the retired attempt. Keep the physical
        // link without recording a new connection or changing its retry policy.
        clearPendingWifiConnectState();
        state_.connectedSlot = liveSlot;
        state_.client = WIFI_CLIENT_CONNECTED;
        state_.candidates.staDropGate.clear();
        state_.candidates.retryAt = 0;
    } else {
        disconnectTrackedWifiActivity(environment, "saved_network_changed", !enabled);
    }
    return true;
}

void WifiStationLifecycle::processWifiClientConnectPhase(const Environment& environment) {
    if (state_.pending.phase == WifiConnectPhase::IDLE || cancelObsoleteSavedNetworkAttempt(environment)) {
        return;
    }

    unsigned long now = millis();
    switch (state_.pending.phase) {
    case WifiConnectPhase::PREPARE_OFF:
        if (environment.setupActive) {
            // Keep AP online and use a direct STA begin path.
            // Repeated STA resets in AP+STA mode have proven brittle on some routers.
            Serial.println("[WiFiClient] Preserving AP, preparing STA connect...");
            if (WiFi.getMode() != WIFI_AP_STA) {
                WiFi.mode(WIFI_AP_STA);
                state_.pending.phaseStartedAt = now;
                state_.pending.phase = WifiConnectPhase::WAIT_AP_STA;
            } else {
                state_.pending.phase = WifiConnectPhase::BEGIN_CONNECT;
            }
        } else {
            if (WiFi.getMode() != WIFI_OFF) {
                Serial.println("[WiFiClient] Cleaning up WiFi before reconnect...");
                WiFi.disconnect(false, false); // Graceful release without credential erase
                WiFi.mode(WIFI_OFF);           // Fully shut down WiFi driver
            }
            state_.pending.phaseStartedAt = now;
            state_.pending.phase = WifiConnectPhase::WAIT_OFF;
        }
        break;

    case WifiConnectPhase::WAIT_OFF:
        if (now - state_.pending.phaseStartedAt >= WIFI_MODE_SWITCH_SETTLE_MS) {
            state_.pending.phase = WifiConnectPhase::ENABLE_AP_STA;
        }
        break;

    case WifiConnectPhase::ENABLE_AP_STA:
        Serial.println("[WiFiClient] Initializing WiFi in AP+STA mode");
        WiFi.mode(WIFI_AP_STA);
        state_.pending.phaseStartedAt = now;
        state_.pending.phase = WifiConnectPhase::WAIT_AP_STA;
        break;

    case WifiConnectPhase::WAIT_AP_STA:
        if (now - state_.pending.phaseStartedAt >= WIFI_MODE_SWITCH_SETTLE_MS) {
            state_.pending.phase = WifiConnectPhase::BEGIN_CONNECT;
        }
        break;

    case WifiConnectPhase::BEGIN_CONNECT:
        if (state_.pending.ssid.length() == 0) {
            state_.pending.phase = WifiConnectPhase::IDLE;
            state_.client = WIFI_CLIENT_FAILED;
            state_.pending.slot = -1;
            state_.connectedSlot = -1;
            break;
        }
        // Improve coexistence stability while connecting alongside BLE links.
        WiFi.setSleep(false);
        WiFi.setAutoReconnect(true);
        Serial.println("[WiFiClient] Connecting to configured network");
        startWifiAttempt(state_.pending.ssid, state_.pending.password);
        state_.pending.startedAt = now;
        state_.pending.phase = WifiConnectPhase::IDLE;
        break;

    case WifiConnectPhase::IDLE:
    default:
        break;
    }
}

bool WifiStationLifecycle::beginMaintenanceAutoConnectScan(const Environment& environment, bool explicitEnableRequest) {
    return WifiSetupNetworkPolicy::startMaintenanceAutoConnect(
        [this]() { return settings_.resolveStorageTransactionsForMutation(); },
        [this, &environment]() {
            Serial.println("[WiFiClient] Maintenance STA auto-connect deferred: storage recovery pending");
            scheduleMaintenanceAutoConnectRetry(environment, WIFI_MAINTENANCE_RETRY_INTERVAL_MS);
        },
        [this, &environment, explicitEnableRequest]() {
            cancelMaintenanceAutoConnect(environment, "restart_scan");

            if (!environment.maintenanceMode) {
                return false;
            }

            const V1Settings& settings = settings_.get();
            if (explicitEnableRequest) {
                state_.candidates.collisionMask = 0;
                state_.manualDisconnect = false;
            }
            if (!settings.hasConfiguredWifiStaSlot()) {
                Serial.println("[WiFiClient] Maintenance STA auto-connect skipped: no saved slots");
                state_.client = WIFI_CLIENT_DISCONNECTED;
                return false;
            }
            if (!settings.wifiClientEnabled && !explicitEnableRequest) {
                Serial.println("[WiFiClient] Maintenance STA auto-connect skipped: client disabled");
                state_.client = WIFI_CLIENT_DISABLED;
                return false;
            }

            const WifiScanResultOwner::RequestResult requestResult =
                scanOwner_.request(WifiScanConsumer::MAINTENANCE, scanDriver());
            if (requestResult == WifiScanResultOwner::RequestResult::FAILED) {
                Serial.println("[WiFiClient] Maintenance STA auto-connect scan failed to start");
                finishMaintenanceAutoConnect(environment, "scan_start_failed", true);
                return false;
            }

            state_.candidates.staDropGate.clear();
            state_.candidates.retryAt = 0;
            state_.client = WIFI_CLIENT_DISCONNECTED;
            state_.candidates.phase = MaintenanceAutoConnectPhase::SCANNING;
            state_.candidates.scanStartedAt = millis();
            state_.candidates.count = 0;
            state_.candidates.cursor = 0;
            Serial.println(requestResult == WifiScanResultOwner::RequestResult::JOINED
                               ? "[WiFiClient] Maintenance STA auto-connect joined the active scan"
                               : "[WiFiClient] Maintenance STA auto-connect scan started");
            return true;
        });
}

void WifiStationLifecycle::processMaintenanceAutoConnect(const Environment& environment) {
    const WifiScanResultOwner::HarvestResult harvestResult = scanOwner_.harvest(scanDriver());
    applyDeferredMaintenanceStaRadioDrop(environment);

    if (state_.candidates.phase != MaintenanceAutoConnectPhase::SCANNING) {
        return;
    }

    if (harvestResult == WifiScanResultOwner::HarvestResult::RUNNING) {
        const unsigned long now = millis();
        if (state_.candidates.scanStartedAt != 0 &&
            (now - state_.candidates.scanStartedAt) >= WIFI_MAINTENANCE_SCAN_TIMEOUT_MS) {
            Serial.println("[WiFiClient] Maintenance STA auto-connect scan timed out");
            scanOwner_.cancel(WifiScanConsumer::MAINTENANCE, scanDriver());
            finishMaintenanceAutoConnect(environment, "scan_timeout", true);
        }
        return;
    }

    if (harvestResult == WifiScanResultOwner::HarvestResult::FAILED) {
        Serial.println("[WiFiClient] Maintenance STA auto-connect scan failed");
        finishMaintenanceAutoConnect(environment, "scan_failed", true);
        return;
    }

    if (!scanOwner_.hasSnapshot(WifiScanConsumer::MAINTENANCE)) {
        Serial.println("[WiFiClient] Maintenance STA auto-connect scan results unavailable");
        finishMaintenanceAutoConnect(environment, "scan_results_unavailable", true);
        return;
    }

    const std::vector<ScannedNetwork> scannedNetworks =
        scanOwner_.copySnapshot(WifiScanConsumer::MAINTENANCE, scanDriver());
    scanOwner_.clearSnapshot(WifiScanConsumer::MAINTENANCE);

    const V1Settings& settings = settings_.get();
    size_t ordered[kWifiStaSlotCount] = {};
    const size_t orderedCount = WifiStaSlotPolicy::orderConfiguredSlots(settings, ordered, kWifiStaSlotCount);
    state_.candidates.count = 0;
    state_.candidates.cursor = 0;

    for (size_t orderedPos = 0; orderedPos < orderedCount; ++orderedPos) {
        const size_t slotIndex = ordered[orderedPos];
        if ((state_.candidates.collisionMask & static_cast<uint8_t>(1U << slotIndex)) != 0) {
            continue;
        }
        const WifiStaSlot& slot = settings.wifiStaSlots[slotIndex];
        for (const ScannedNetwork& scannedNetwork : scannedNetworks) {
            if (scannedNetwork.ssid == slot.ssid) {
                state_.candidates.slots[state_.candidates.count++] = slotIndex;
                break;
            }
        }
    }

    if (state_.candidates.count == 0) {
        Serial.printf("[WiFiClient] Maintenance STA auto-connect found no saved SSIDs in %u scan results\n",
                      static_cast<unsigned>(scannedNetworks.size()));
        finishMaintenanceAutoConnect(environment, "no_saved_ssid_in_range", true);
        return;
    }

    Serial.printf("[WiFiClient] Maintenance STA auto-connect has %u candidate slot(s)\n",
                  static_cast<unsigned>(state_.candidates.count));
    state_.candidates.phase = MaintenanceAutoConnectPhase::CONNECTING;
    if (!queueNextMaintenanceAutoConnectSlot(environment)) {
        finishMaintenanceAutoConnect(environment, "candidate_queue_failed", true);
    }
}

bool WifiStationLifecycle::queueNextMaintenanceAutoConnectSlot(const Environment& environment) {
    if (state_.candidates.phase != MaintenanceAutoConnectPhase::CONNECTING) {
        return false;
    }

    const V1Settings& settings = settings_.get();
    while (state_.candidates.cursor < state_.candidates.count) {
        const size_t slotIndex = state_.candidates.slots[state_.candidates.cursor++];
        if (slotIndex >= kWifiStaSlotCount) {
            continue;
        }
        if ((state_.candidates.collisionMask & static_cast<uint8_t>(1U << slotIndex)) != 0) {
            continue;
        }

        const WifiStaSlot& slot = settings.wifiStaSlots[slotIndex];
        if (!slot.isConfigured()) {
            continue;
        }

        bool credentialUnavailable = false;
        const String password = settings_.getWifiStaSlotPassword(slotIndex, &credentialUnavailable);
        if (credentialUnavailable) continue;
        Serial.printf("[WiFiClient] Maintenance STA auto-connect trying slot %u\n", static_cast<unsigned>(slotIndex));
        if (connectToNetwork(environment, slot.ssid, password, false,
                             static_cast<int>(slotIndex), true)) {
            return true;
        }

        Serial.printf("[WiFiClient] Maintenance STA auto-connect failed to queue slot %u\n",
                      static_cast<unsigned>(slotIndex));
    }

    return false;
}

void WifiStationLifecycle::finishMaintenanceAutoConnect(const Environment& environment, const char* reason, bool dropStaRadio) {
    if (state_.candidates.phase != MaintenanceAutoConnectPhase::IDLE) {
        Serial.printf("[WiFiClient] Maintenance STA auto-connect complete: %s\n",
                      (reason && reason[0] != '\0') ? reason : "done");
    }

    state_.candidates.phase = MaintenanceAutoConnectPhase::COMPLETE;
    state_.candidates.scanStartedAt = 0;
    state_.candidates.count = 0;
    state_.candidates.cursor = 0;

    if (dropStaRadio) {
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
        state_.candidates.staDropGate.request();
        scheduleMaintenanceAutoConnectRetry(environment, WIFI_MAINTENANCE_RETRY_INTERVAL_MS);
    } else {
        state_.candidates.staDropGate.clear();
        state_.candidates.retryAt = 0;
    }
    applyDeferredMaintenanceStaRadioDrop(environment);
}

void WifiStationLifecycle::applyDeferredMaintenanceStaRadioDrop(const Environment& environment) {
    if (!state_.candidates.staDropGate.takeIfReady(scanOwner_.isRunning())) {
        return;
    }

    if (environment.setupActive && state_.client != WIFI_CLIENT_CONNECTED &&
        state_.client != WIFI_CLIENT_CONNECTING) {
        const wifi_mode_t mode = WiFi.getMode();
        if (mode == WIFI_AP_STA || mode == WIFI_STA) {
            Serial.println("[WiFiClient] Maintenance STA unavailable; returning to AP-only mode");
            WiFi.mode(WIFI_AP);
        }
        if (state_.client != WIFI_CLIENT_DISABLED) state_.client = WIFI_CLIENT_DISCONNECTED;
    }
}

void WifiStationLifecycle::cancelMaintenanceAutoConnect(const Environment& environment, const char* reason) {
    if (state_.candidates.phase == MaintenanceAutoConnectPhase::IDLE ||
        state_.candidates.phase == MaintenanceAutoConnectPhase::COMPLETE) {
        return;
    }

    Serial.printf("[WiFiClient] Maintenance STA auto-connect canceled: %s\n",
                  (reason && reason[0] != '\0') ? reason : "unknown");
    const bool wasScanning = state_.candidates.phase == MaintenanceAutoConnectPhase::SCANNING;
    const bool wasConnecting = state_.candidates.phase == MaintenanceAutoConnectPhase::CONNECTING;
    if (wasScanning) {
        scanOwner_.cancel(WifiScanConsumer::MAINTENANCE, scanDriver());
    }
    if (wasConnecting) {
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
        clearPendingWifiConnectState();
        state_.connectedSlot = -1;
        state_.client = WIFI_CLIENT_DISCONNECTED;
    }
    state_.candidates.phase = MaintenanceAutoConnectPhase::IDLE;
    state_.candidates.scanStartedAt = 0;
    state_.candidates.count = 0;
    state_.candidates.cursor = 0;
    state_.candidates.staDropGate.request();
    applyDeferredMaintenanceStaRadioDrop(environment);
}

void WifiStationLifecycle::scheduleMaintenanceAutoConnectRetry(const Environment& environment, unsigned long delayMs) {
    if (!environment.maintenanceMode || !settings_.get().wifiClientEnabled ||
        !settings_.get().hasConfiguredWifiStaSlot()) {
        state_.candidates.retryAt = 0;
        return;
    }
    state_.candidates.retryAt = millis() + delayMs;
    if (state_.candidates.retryAt == 0) {
        state_.candidates.retryAt = 1;
    }
}

bool WifiStationLifecycle::maintenanceAddressCollision(const Environment& environment) const {
    if (!environment.maintenanceMode || !environment.setupActive) {
        return false;
    }
    return WifiMaintenanceInterfacePolicy::hasAddressCollision(static_cast<uint32_t>(WiFi.softAPIP()),
                                                                static_cast<uint32_t>(WiFi.localIP()));
}

void WifiStationLifecycle::checkWifiClientStatus(const Environment& environment) {
    const uint32_t recoveredRevision = settings_.recoveryRevision();
    const bool startupRecoveryCompleted =
        state_.recoveryPending && !settings_.recoveryPending();
    if (environment.maintenanceMode &&
        (recoveredRevision != state_.recoveryRevision || startupRecoveryCompleted)) {
        state_.recoveryRevision = recoveredRevision;
        if (startupRecoveryCompleted) state_.recoveryPending = false;
        const V1Settings& recovered = settings_.get();
        // AP-only boot may have used provisional defaults while NVS was
        // unavailable, or WiFi keys healed during a deferred SD-only recovery.
        // Admit the recovered saved networks through the normal
        // retry path, preserving explicit disconnects and any active attempt.
        if (state_.client == WIFI_CLIENT_DISABLED && !state_.manualDisconnect &&
            recovered.wifiClientEnabled && recovered.hasConfiguredWifiStaSlot()) {
            state_.client = WIFI_CLIENT_DISCONNECTED;
            scheduleMaintenanceAutoConnectRetry(environment, 1u);
        }
    }
    // Skip if WiFi client is disabled
    if (state_.client == WIFI_CLIENT_DISABLED) {
        if (environment.maintenanceMode && WiFi.status() == WL_CONNECTED) {
            Serial.println("[WiFiClient] Disconnecting unexpected physical STA while client is disabled");
            WiFi.setAutoReconnect(false);
            WiFi.disconnect(false, false);
            if (!scanOwner_.isRunning() && environment.setupActive) {
                WiFi.mode(WIFI_AP);
            }
        }
        return;
    }

    if (cancelObsoleteSavedNetworkAttempt(environment)) {
        return;
    }
    if (environment.maintenanceMode && state_.client == WIFI_CLIENT_CONNECTED && state_.connectedSlot >= 0 &&
        !settings_.get().wifiClientEnabled) {
        disconnectTrackedWifiActivity(environment, "saved_network_disabled", true);
        return;
    }

    wl_status_t status = WiFi.status();

    if (WifiMaintenanceLinkPolicy::evaluate({
            .physicalConnected = status == WL_CONNECTED,
            .autoJoinSuppressed = state_.manualDisconnect,
        }) == WifiMaintenanceLinkPolicy::Decision::RejectSuppressedPhysicalConnection) {
        Serial.println("[WiFiClient] Rejecting late STA connection after explicit disconnect");
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
        clearPendingWifiConnectState();
        state_.connectedSlot = -1;
        state_.client = WIFI_CLIENT_DISCONNECTED;
        state_.candidates.retryAt = 0;
        state_.candidates.staDropGate.request();
        applyDeferredMaintenanceStaRadioDrop(environment);
        return;
    }

    if (WifiMaintenanceLinkPolicy::shouldDisconnectAddressCollision(status == WL_CONNECTED,
                                                                    maintenanceAddressCollision(environment))) {
        int collisionSlot = state_.pending.slot;
        if (collisionSlot < 0 || static_cast<size_t>(collisionSlot) >= kWifiStaSlotCount) {
            collisionSlot = findConfiguredSlotBySsid(WiFi.SSID());
        }
        if (collisionSlot >= 0 && static_cast<size_t>(collisionSlot) < kWifiStaSlotCount) {
            state_.candidates.collisionMask |= static_cast<uint8_t>(1U << collisionSlot);
        }

        const bool wasMaintenanceCandidate =
            state_.candidates.phase == MaintenanceAutoConnectPhase::CONNECTING;
        Serial.println("[WiFiClient] STA address collides with maintenance AP; disconnecting STA");
        WiFi.setAutoReconnect(false);
        WiFi.disconnect(false, false);
        clearPendingWifiConnectState();
        state_.client = WIFI_CLIENT_FAILED;
        state_.connectedSlot = -1;
        if (wasMaintenanceCandidate) {
            if (!queueNextMaintenanceAutoConnectSlot(environment)) {
                finishMaintenanceAutoConnect(environment, "address_collision", true);
            }
        } else {
            cancelMaintenanceAutoConnect(environment, "address_collision");
            scheduleMaintenanceAutoConnectRetry(environment, WIFI_MAINTENANCE_RETRY_INTERVAL_MS);
        }
        return;
    }

    switch (state_.client) {
    case WIFI_CLIENT_CONNECTING: {
        // Non-blocking mode transition is still in progress.
        if (state_.pending.phase != WifiConnectPhase::IDLE || state_.pending.startedAt == 0) {
            break;
        }

        const wl_status_t failure = currentWifiAttemptFailure();
        // The framework can retain the old link/status when switching fails.
        // Admit only this target after its start succeeded, never another
        // network's cached connection as completion of the pending attempt.
        if (failure != WL_CONNECT_FAILED && failure != WL_NO_SSID_AVAIL && status == WL_CONNECTED &&
            state_.pending.ssid.length() > 0 && WiFi.SSID() == state_.pending.ssid) {
            state_.client = WIFI_CLIENT_CONNECTED;
            state_.pending.startedAt = 0;
            Serial.println("[WiFiClient] Connected");
            if (environment.setupActive) {
                // Arm AP idle timer from STA connect so setup UI clients have
                // a full grace window before AP retirement.
                connectedEffect_.notify(connectedEffect_.context, millis());
                Serial.println("[WiFiClient] STA connected; AP idle-retire timer armed");
            }

            // Reset failure counter on successful connection
            state_.reconnectFailures = 0;

            // Save credentials on successful connection
            if (state_.pending.ssid.length() > 0) {
                const bool hasPendingSlot =
                    state_.pending.slot >= 0 && static_cast<size_t>(state_.pending.slot) < kWifiStaSlotCount;
                if (state_.pending.persistCredentials) {
                    if (hasPendingSlot) {
                        const size_t slotIndex = static_cast<size_t>(state_.pending.slot);
                        const V1Settings& currentSettings = settings_.get();
                        const WifiStaSlot& currentSlot = currentSettings.wifiStaSlots[slotIndex];
                        const String label = currentSlot.label;
                        const uint8_t priority =
                            currentSlot.isConfigured() ? currentSlot.priority : static_cast<uint8_t>(slotIndex);
                        settings_.setWifiStaSlotCredentials(slotIndex, state_.pending.ssid,
                                                                  state_.pending.password, label, priority);
                    } else {
                        const V1Settings& currentSettings = settings_.get();
                        const bool ssidChanged = (state_.pending.ssid != currentSettings.wifiClientSSID);
                        const bool passwordChanged =
                            (state_.pending.password != settings_.getWifiClientPassword());
                        if (ssidChanged || passwordChanged) {
                            settings_.setWifiClientCredentials(state_.pending.ssid, state_.pending.password);
                        } else {
                            Serial.println("[WiFiClient] Connected with unchanged credentials; skipping re-save");
                        }
                    }
                } else {
                    Serial.println("[WiFiClient] Connected via auto-reconnect; skipping credential re-save");
                }
                state_.connectedSlot = hasPendingSlot
                                                 ? state_.pending.slot
                                                 : findConfiguredSlotBySsid(state_.pending.ssid);
                if (state_.connectedSlot >= 0 &&
                    static_cast<size_t>(state_.connectedSlot) < kWifiStaSlotCount) {
                    if (!settings_.markWifiStaSlotConnected(static_cast<size_t>(state_.connectedSlot))) {
                        Serial.println("[WiFiClient] WARN: Connected-network recency persist failed");
                    }
                }
                if (state_.candidates.phase == MaintenanceAutoConnectPhase::CONNECTING) {
                    finishMaintenanceAutoConnect(environment, "connected", false);
                }
                clearPendingWifiConnectState();
            }
        } else if (failure == WL_CONNECT_FAILED || failure == WL_NO_SSID_AVAIL) {
            const bool wasMaintenanceAutoConnect =
                state_.candidates.phase == MaintenanceAutoConnectPhase::CONNECTING;
            state_.client = WIFI_CLIENT_FAILED;
            state_.connectedSlot = -1;
            Serial.printf("[WiFiClient] Connection failed: %d\n", failure);
            state_.pending.startedAt = 0;

            clearPendingWifiConnectState();
            if (wasMaintenanceAutoConnect) {
                if (queueNextMaintenanceAutoConnectSlot(environment)) {
                    break;
                }
                finishMaintenanceAutoConnect(environment, "all_candidates_failed", true);
            }
        } else if (millis() - state_.pending.startedAt > WIFI_CONNECT_TIMEOUT_MS) {
            const bool wasMaintenanceAutoConnect =
                state_.candidates.phase == MaintenanceAutoConnectPhase::CONNECTING;
            state_.client = WIFI_CLIENT_FAILED;
            state_.connectedSlot = -1;
            Serial.println("[WiFiClient] Connection timeout");
            WiFi.disconnect(false);
            state_.pending.startedAt = 0;

            clearPendingWifiConnectState();
            if (wasMaintenanceAutoConnect) {
                if (queueNextMaintenanceAutoConnectSlot(environment)) {
                    break;
                }
                finishMaintenanceAutoConnect(environment, "all_candidates_timeout", true);
            }
        }
        break;
    }

    case WIFI_CLIENT_CONNECTED: {
        if (status != WL_CONNECTED) {
            state_.client = WIFI_CLIENT_DISCONNECTED;
            state_.connectedSlot = -1;
            Serial.println("[WiFiClient] Lost connection");
            scheduleMaintenanceAutoConnectRetry(environment, WIFI_MAINTENANCE_LINK_LOSS_RETRY_MS);
        } else if (environment.maintenanceMode && state_.connectedSlot >= 0) {
            const String ssid = WiFi.SSID();
            if (ssid.length() == 0 || savedSlotMatches(state_.connectedSlot, ssid)) break;
            const int currentSlot = findConfiguredSlotBySsid(ssid);
            if (currentSlot >= 0) {
                state_.connectedSlot = currentSlot;
            } else {
                disconnectTrackedWifiActivity(environment, "saved_network_changed", false);
            }
        }
        break;
    }

    case WIFI_CLIENT_DISCONNECTED:
    case WIFI_CLIENT_FAILED: {
        if (environment.lowDmaCooldownMs > 0) {
            break;
        }

        if (environment.maintenanceMode) {
            const V1Settings& settings = settings_.get();
            const bool autoConnectActive = state_.candidates.phase == MaintenanceAutoConnectPhase::SCANNING ||
                                           state_.candidates.phase == MaintenanceAutoConnectPhase::CONNECTING;
            const WifiMaintenanceLinkPolicy::Decision decision = WifiMaintenanceLinkPolicy::evaluate({
                .physicalConnected = status == WL_CONNECTED,
                .appConnected = false,
                .appConnecting = false,
                .autoConnectActive = autoConnectActive,
                .clientEnabled = settings.wifiClientEnabled,
                .hasSavedCandidates = settings.hasConfiguredWifiStaSlot(),
                .autoJoinSuppressed = state_.manualDisconnect,
                .nowMs = static_cast<uint32_t>(millis()),
                .retryAtMs = static_cast<uint32_t>(state_.candidates.retryAt),
            });
            if (decision == WifiMaintenanceLinkPolicy::Decision::ReconcilePhysicalConnection) {
                const int slotIndex = findConfiguredSlotBySsid(WiFi.SSID());
                if (slotIndex >= 0) {
                    // Publish the recovered link before scan cancellation can
                    // retire STA based on the stale DISCONNECTED app state.
                    state_.client = WIFI_CLIENT_CONNECTED;
                    state_.connectedSlot = slotIndex;
                    if (!settings_.markWifiStaSlotConnected(static_cast<size_t>(state_.connectedSlot))) {
                        Serial.println("[WiFiClient] WARN: Connected-network recency persist failed");
                    }
                    cancelMaintenanceAutoConnect(environment, "physical_reconnect");
                    state_.candidates.staDropGate.clear();
                    state_.reconnectFailures = 0;
                    state_.candidates.retryAt = 0;
                    Serial.println("[WiFiClient] Reconciled framework STA auto-reconnect");
                } else {
                    Serial.println("[WiFiClient] Rejecting physical connection to unsaved network");
                    WiFi.setAutoReconnect(false);
                    WiFi.disconnect(false, false);
                    scheduleMaintenanceAutoConnectRetry(environment, WIFI_MAINTENANCE_LINK_LOSS_RETRY_MS);
                }
            } else if (decision == WifiMaintenanceLinkPolicy::Decision::StartCandidateScan) {
                if (!beginMaintenanceAutoConnectScan(environment, false)) {
                    scheduleMaintenanceAutoConnectRetry(environment, WIFI_MAINTENANCE_RETRY_INTERVAL_MS);
                }
            }
            break;
        }

        // Defer background STA reconnect attempts during early boot until V1 is
        // connected. This protects BLE acquisition from AP+STA mode churn.
        const bool v1Connected = environment.v1Connected;
        const uint32_t bootNowMs = millis();
        const WifiReconnectPolicy::BootDecision bootDecision = WifiReconnectPolicy::evaluateBoot({
            v1Connected,
            environment.setupStartedAt != 0,
            bootNowMs,
            static_cast<uint32_t>(environment.setupStartedAt),
            WIFI_RECONNECT_DEFER_NO_V1_MS,
            state_.reconnectDeferredLogged,
        });

        if (bootDecision.logDeferred) {
            Serial.printf("[WiFiClient] Auto-reconnect deferred (waiting for V1 or %lu ms grace)\n",
                          (unsigned long)WIFI_RECONNECT_DEFER_NO_V1_MS);
        }
        if (bootDecision.logResumed) {
            Serial.println("[WiFiClient] Auto-reconnect resumed");
        }
        state_.reconnectDeferredLogged = bootDecision.deferredLogged;
        if (bootDecision.action == WifiReconnectPolicy::BootAction::DEFER_FOR_V1) {
            break;
        }

        // Auto-reconnect if we have saved credentials (with failure limit).
        const V1Settings& settings = settings_.get();
        const uint32_t retryNowMs = millis();
        const WifiReconnectPolicy::AttemptDecision reconnectDecision = WifiReconnectPolicy::evaluateAttempt({
            settings.wifiClientEnabled && settings.wifiClientSSID.length() > 0,
            environment.autoStarted,
            environment.apClientPresent,
            environment.uiActivityPresent,
            state_.reconnectFailures,
            WIFI_MAX_RECONNECT_FAILURES,
            retryNowMs,
            static_cast<uint32_t>(state_.lastReconnectAttempt),
            WIFI_RECONNECT_INTERVAL_MS,
        });

        if (reconnectDecision.action != WifiReconnectPolicy::AttemptAction::ATTEMPT &&
            reconnectDecision.action != WifiReconnectPolicy::AttemptAction::GIVE_UP) {
            break;
        }

        const String savedPassword = settings_.getWifiClientPassword();
        state_.lastReconnectAttempt = retryNowMs;
        state_.reconnectFailures = reconnectDecision.nextFailures;

        if (reconnectDecision.action == WifiReconnectPolicy::AttemptAction::GIVE_UP) {
            Serial.printf("[WiFiClient] Giving up after %d failed attempts. Use BOOT button to retry.\n",
                          state_.reconnectFailures);
            // Stay in FAILED state, user must toggle WiFi to retry
            break;
        }

        Serial.printf("[WiFiClient] Auto-reconnect attempt %d/%d...\n", state_.reconnectFailures,
                      WIFI_MAX_RECONNECT_FAILURES);
        connectToNetwork(environment, settings.wifiClientSSID, savedPassword, false);
        break;
    }

    default:
        break;
    }
}

bool WifiStationLifecycle::enableWifiClientFromSavedCredentials(const Environment& environment) {
    struct EnableContext {
        WifiStationLifecycle* lifecycle;
        const Environment& environment;
        State priorState;
        wifi_mode_t priorMode;
        bool priorAutoReconnect;
    };
    EnableContext transaction{this, environment, state_, WiFi.getMode(), WiFi.getAutoReconnect()};
    WifiClientEnableTransaction::Runtime runtime;
    runtime.ctx = &transaction;
    runtime.persistedEnabled = settings_.get().wifiClientEnabled;
    runtime.lifecycleAdmitted = state_.client == WIFI_CLIENT_CONNECTING || state_.client == WIFI_CLIENT_CONNECTED ||
                               state_.candidates.phase == MaintenanceAutoConnectPhase::SCANNING ||
                               state_.candidates.phase == MaintenanceAutoConnectPhase::CONNECTING;
    runtime.attemptStart = [](void* context) {
        auto& transaction = *static_cast<EnableContext*>(context);
        auto& self = *transaction.lifecycle;
        if (transaction.environment.maintenanceMode) {
            if (self.beginMaintenanceAutoConnectScan(transaction.environment, true)) return true;
            return !self.settings_.get().hasConfiguredWifiStaSlot();
        }
        const String savedSsid = self.settings_.get().wifiClientSSID;
        if (savedSsid.length() == 0) {
            self.selectSessionMode(true);
            return true;
        }
        if (self.connectToNetwork(transaction.environment, savedSsid, self.settings_.getWifiClientPassword())) return true;
        self.selectSessionMode(true);
        return false;
    };
    runtime.rollbackFailedStart = [](void* context) {
        auto& transaction = *static_cast<EnableContext*>(context);
        auto& self = *transaction.lifecycle;
        self.cancelMaintenanceAutoConnect(transaction.environment, "enable_persist_rollback");
        if (transaction.priorState.client != WIFI_CLIENT_CONNECTED && WiFi.status() == WL_CONNECTED) {
            WiFi.disconnect(false, false);
        }
        self.state_ = transaction.priorState;
        WiFi.setAutoReconnect(transaction.priorAutoReconnect);
        WiFi.mode(transaction.priorMode);
    };
    runtime.commitEnabled = [](void* context) {
        return static_cast<EnableContext*>(context)->lifecycle->settings_.setWifiClientEnabled(true).success;
    };
    return WifiClientEnableTransaction::execute(runtime);
}

WifiSavedNetworkMutationPolicy::Decision WifiStationLifecycle::savedNetworkMutation(size_t index) const {
    return WifiSavedNetworkMutationPolicy::evaluate({true, false, static_cast<int>(index), state_.connectedSlot,
        state_.pending.slot, state_.candidates.phase == MaintenanceAutoConnectPhase::SCANNING,
        state_.candidates.phase == MaintenanceAutoConnectPhase::CONNECTING});
}

void WifiStationLifecycle::savedNetworkMutationCommitted(const Environment& environment, size_t index,
                                                         WifiSavedNetworkMutationPolicy::Decision mutation,
                                                         const char* reason) {
    if (mutation.cancelMaintenanceAutoActivity) cancelMaintenanceAutoConnect(environment, reason);
    state_.candidates.collisionMask &= static_cast<uint8_t>(~(1U << index));
    if (mutation.disconnectTrackedActivity) disconnectTrackedWifiActivity(environment, reason, false);
    if (mutation.scheduleReplacementScan) scheduleMaintenanceAutoConnectRetry(environment, WIFI_MAINTENANCE_LINK_LOSS_RETRY_MS);
}

void WifiStationLifecycle::disableCommitted(const Environment& environment) {
    disconnectFromNetwork(environment);
    state_.client = WIFI_CLIENT_DISABLED;
    state_.candidates.retryAt = 0;
    WiFi.mode(WIFI_AP);
}

void WifiStationLifecycle::forgetCommitted(const Environment& environment) {
    cancelMaintenanceAutoConnect(environment, "forget");
    disconnectFromNetwork(environment);
    state_.client = WIFI_CLIENT_DISABLED;
    state_.candidates.retryAt = 0;
    state_.candidates.collisionMask = 0;
    WiFi.mode(WIFI_AP);
}

void WifiStationLifecycle::beginSession() {
    state_.candidates.retryAt = 0;
    state_.recoveryPending = settings_.recoveryPending();
    state_.candidates.collisionMask = 0;
    state_.manualDisconnect = false;
    resetReconnectFailures();
}

void WifiStationLifecycle::selectSessionMode(bool staEnabled) {
    state_.client = staEnabled ? WIFI_CLIENT_DISCONNECTED : WIFI_CLIENT_DISABLED;
    state_.connectedSlot = -1;
}

void WifiStationLifecycle::stop() {
    selectSessionMode(false);
    scanOwner_.reset(scanDriver());
    clearPendingWifiConnectState();
    state_.candidates = Candidates{};
    state_.manualDisconnect = false;
    state_.lastReconnectAttempt = 0;
    state_.reconnectDeferredLogged = false;
}

void WifiStationLifecycle::apRetiredForLowDma() {
    const wl_status_t status = WiFi.status();
    if (status == WL_CONNECTED) {
        state_.client = WIFI_CLIENT_CONNECTED;
        state_.reconnectFailures = 0;
    } else if (state_.client != WIFI_CLIENT_CONNECTING) {
        selectSessionMode(true);
    }
    // Preserve the in-flight credentials and clock for subsequent status service.
    state_.pending.phase = WifiConnectPhase::IDLE;
    state_.pending.phaseStartedAt = 0;
}

void WifiStationLifecycle::linkLostDuringApRetire(const Environment& environment) {
    selectSessionMode(true);
    const V1Settings& settings = settings_.get();
    if (settings.wifiClientEnabled && settings.wifiClientSSID.length() > 0) {
        String password = settings_.getWifiClientPassword();
        connectToNetwork(environment, settings.wifiClientSSID, password, false);
    }
}
