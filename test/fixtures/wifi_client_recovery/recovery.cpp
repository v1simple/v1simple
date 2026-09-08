#include "owner.h"
#include <iostream>
#include <stdexcept>

SerialClass Serial;
WiFiClass WiFi;
unsigned long mockMillis = 0, mockMicros = 0;

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void loseSavedConnection(WiFiManager& manager) {
    WiFi = WiFiClass{};
    manager.settings_.value.wifiStaSlots[0].ssid = "Saved";
    manager.wifiClientState_ = WIFI_CLIENT_CONNECTED;
    manager.currentConnectedSlotIndex_ = 0;
    manager.maintenanceAutoConnectPhase_ = WiFiManager::MaintenanceAutoConnectPhase::COMPLETE;
    mockMillis = 1000;
    WiFi.lost();
    manager.runOwnerPass();
    require(manager.maintenanceAutoConnectRetryAtMs_ == 6000, "link loss must schedule the real retry");
}

void requireRecovered(const WiFiManager& manager) {
    require(manager.wifiClientState_ == WIFI_CLIENT_CONNECTED, "app must be connected");
    require(manager.currentConnectedSlotIndex_ == 0, "saved slot must be reconciled");
    require(WiFi.physicalConnected && WiFi.radioMode == WIFI_AP_STA, "reconciliation removed the recovered STA");
    require(WiFi.apOnlyTransitions == 0 && WiFi.disconnects == 0, "reconciliation must not tear down STA");
    require(manager.maintenanceAutoConnectRetryAtMs_ == 0, "recovered link must not retain a retry");
}

void reconnectAtScanBoundary(bool sharedUi, bool delayedStatus) {
    WiFiManager manager{};
    loseSavedConnection(manager);
    WiFi.publishStopImmediately = !delayedStatus;
    // GOT_IP arrives after the owner's DISCONNECTED snapshot but before scan
    // start. This is a supported background scan of an already connected STA.
    WiFi.beforeScan = []() {
        require(WiFi.status() == WL_DISCONNECTED, "event must follow the disconnected snapshot");
        WiFi.restored();
    };
    mockMillis = 6000;
    manager.runOwnerPass();
    require(WiFi.startedScanConnected && manager.wifiScanOwner_.isRunning(), "background scan must be active");
    if (sharedUi) {
        WifiScanResultOwner::Driver driver;
        driver.runningStatus = WIFI_SCAN_RUNNING;
        require(manager.wifiScanOwner_.request(WifiScanConsumer::UI, driver) ==
                    WifiScanResultOwner::RequestResult::JOINED, "UI must share the existing scan");
    }

    mockMillis = 6050;
    manager.runOwnerPass();
    require(WiFi.scanStatusReads == 2, "both real maintenance harvest passes must precede reconciliation");
    requireRecovered(manager);
    require(!manager.wifiScanOwner_.isPending(WifiScanConsumer::MAINTENANCE), "obsolete maintenance consumer retained");
    require(WiFi.scanAborts == (sharedUi ? 0 : 1), "only the last consumer may abort the scan");
    require(manager.wifiScanOwner_.isRunning() == sharedUi, "shared UI scan lifetime changed");

    if (sharedUi) WiFi.scanState = 1;
    mockMillis = 6100;
    manager.runOwnerPass();
    requireRecovered(manager);
    if (sharedUi) {
        require(manager.wifiScanOwner_.hasSnapshot(WifiScanConsumer::UI), "UI lost completed results");
        const auto snapshot = manager.wifiScanOwner_.copySnapshot(WifiScanConsumer::UI);
        require(snapshot.size() == 1 && snapshot[0].ssid == "Saved", "UI snapshot changed");
    }
}

void noActiveScan() {
    WiFiManager manager{};
    loseSavedConnection(manager);
    WiFi.restored();
    mockMillis = 5900;
    manager.runOwnerPass();
    mockMillis = 6050;
    manager.runOwnerPass();
    requireRecovered(manager);
    require(WiFi.scans == 0 && WiFi.scanAborts == 0, "early recovery must not start a scan");
}

void scanCompletedBeforeHarvest() {
    WiFiManager manager{};
    loseSavedConnection(manager);
    WiFi.beforeScan = []() { WiFi.restored(); };
    mockMillis = 6000;
    manager.runOwnerPass();
    WiFi.scanState = 1;
    mockMillis = 6050;
    manager.runOwnerPass();
    // This control reaches candidate queuing instead of reconciliation. It does
    // not claim anything about the subsequent explicit WiFi.begin() sequence.
    require(manager.wifiClientState_ == WIFI_CLIENT_CONNECTING, "completed scan must queue its candidate");
    require(WiFi.physicalConnected && WiFi.apOnlyTransitions == 0, "completed scan control removed STA");
    require(WiFi.scanAborts == 0, "completed scan must not be aborted");
}

void cancelConnectingCandidate() {
    WiFiManager manager{};
    loseSavedConnection(manager);
    mockMillis = 6000;
    manager.runOwnerPass();
    WiFi.scanState = 1;
    mockMillis = 6050;
    manager.runOwnerPass();
    require(manager.wifiClientState_ == WIFI_CLIENT_CONNECTING, "candidate must be connecting");
    mockMillis = 6060;
    manager.runOwnerPass();
    require(WiFi.begins == 1, "candidate must reach the real staged begin");

    manager.cancelMaintenanceAutoConnect("slot_test");
    require(WiFi.disconnects == 1 && !WiFi.physicalConnected, "explicit cancellation must disconnect its candidate");
    require(manager.wifiClientState_ == WIFI_CLIENT_DISCONNECTED, "cancelled candidate must not claim connected");
    require(manager.wifiConnectPhase_ == WiFiManager::WifiConnectPhase::IDLE, "cancelled connect phase remained active");
    require(manager.pendingConnectSSID_.length() == 0, "cancelled candidate retained credentials");
    require(WiFi.radioMode == WIFI_AP, "unneeded STA must still be retired after explicit cancellation");
}

void explicitDisconnectStillSuppressesReconnect() {
    WiFiManager manager{};
    loseSavedConnection(manager);
    WiFi.restored();
    mockMillis = 5900;
    manager.runOwnerPass();
    manager.disconnectFromNetwork();
    WiFi.restored();
    mockMillis = 5950;
    manager.runOwnerPass();
    require(!WiFi.physicalConnected && WiFi.disconnects == 2, "explicit disconnect must reject late framework recovery");
    require(manager.wifiClientState_ == WIFI_CLIENT_DISCONNECTED, "suppressed reconnect must remain disconnected");
    require(manager.maintenanceAutoConnectRetryAtMs_ == 0, "explicit disconnect must not schedule automatic retry");
}

int main() {
    int failures = 0;
    const auto run = [&failures](const char* name, void (*test)()) {
        try {
            test();
            std::cout << name << ": PASS\n";
        } catch (const std::exception& error) {
            ++failures;
            std::cout << name << ": FAIL: " << error.what() << '\n';
        }
    };
    run("sole_scan_consumer", []() { reconnectAtScanBoundary(false, false); });
    run("sole_scan_delayed_framework_status", []() { reconnectAtScanBoundary(false, true); });
    run("shared_ui_scan", []() { reconnectAtScanBoundary(true, false); });
    run("shared_ui_delayed_framework_status", []() { reconnectAtScanBoundary(true, true); });
    run("no_active_scan", noActiveScan);
    run("completed_scan_before_harvest", scanCompletedBeforeHarvest);
    run("explicit_connecting_candidate_cancel", cancelConnectingCandidate);
    run("explicit_disconnect", explicitDisconnectStillSuppressesReconnect);
    return failures == 0 ? 0 : 1;
}
