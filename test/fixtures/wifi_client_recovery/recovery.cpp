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
    require(manager.settings_.markCalls == 1, "physical reconciliation must advance recency exactly once");
    require(manager.settings_.markedIndex == 0, "physical reconciliation advanced the wrong saved slot");
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

void successWithoutPendingSlotAdvancesResolvedSlotOnce(bool persistUnchangedCredentials) {
    WiFiManager manager{};
    WiFi = WiFiClass{};
    manager.settings_.value.wifiStaSlots[0].ssid = "Saved";
    manager.settings_.value.wifiStaSlots[0].label = "Saved";
    manager.settings_.value.wifiStaSlots[0].priority = 0;
    mockMillis = 1000;
    require(manager.connectToNetwork("Saved", "password", persistUnchangedCredentials), "direct attempt admission");
    manager.processWifiClientConnectPhase();
    mockMillis = 1010;
    manager.processWifiClientConnectPhase();
    WiFi.restored();

    manager.checkWifiClientStatus();

    require(manager.currentConnectedSlotIndex_ == 0, "direct success must resolve the configured slot by SSID");
    require(manager.settings_.markCalls == 1, "direct success must advance recency exactly once");
    require(manager.settings_.markedIndex == 0, "direct success advanced the wrong slot");
    require(manager.settings_.credentialWrites == 0, "recency resolution must not rewrite unchanged credentials");
    require(manager.pendingConnectSSID_.length() == 0 && manager.pendingConnectSlotIndex_ == -1,
            "direct success must clear pending connection state");
}

void startSecondCandidate(WiFiManager& manager, wl_status_t firstFailure) {
    WiFi = WiFiClass{};
    WiFi.lost();
    WiFi.scanSsids = {"Missing", "Working"};
    manager.settings_.value.wifiStaSlots[0].ssid = "Missing";
    manager.settings_.value.wifiStaSlots[0].priority = 0;
    manager.settings_.value.wifiStaSlots[1].ssid = "Working";
    manager.settings_.value.wifiStaSlots[1].priority = 1;
    manager.wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
    mockMillis = 1000;
    require(manager.beginMaintenanceAutoConnectScan(false), "candidate scan must start");
    WiFi.scanState = 2;
    manager.runOwnerPass();
    mockMillis = 1010;
    manager.runOwnerPass();
    require(WiFi.lastBeginSsid == "Missing", "first candidate must start");

    mockMillis = 2000;
    WiFi.failure("Missing", firstFailure);
    manager.runOwnerPass();
    require(manager.pendingConnectSSID_ == "Working", "fresh first failure must queue the next candidate");
    mockMillis = 2010;
    manager.runOwnerPass();
    mockMillis = 2020;
    manager.runOwnerPass();
    require(WiFi.begins == 2 && WiFi.lastBeginSsid == "Working", "second candidate must start");
    require(WiFi.cachedStatus == firstFailure, "begin must retain the framework's cached prior failure");
}

void candidateRetry(wl_status_t firstFailure, bool associationBeforePoll, bool latePriorFailure) {
    WiFiManager manager{};
    startSecondCandidate(manager, firstFailure);
    if (latePriorFailure) WiFi.failure("Missing", firstFailure);
    if (associationBeforePoll) WiFi.cachedStatus = WL_IDLE_STATUS;
    mockMillis = 2050;
    manager.runOwnerPass();
    require(manager.wifiClientState_ == WIFI_CLIENT_CONNECTING,
            "the prior candidate's cached status or late event must not fail the new attempt");
    require(WiFi.radioMode == WIFI_AP_STA && WiFi.apOnlyTransitions == 0 && WiFi.disconnects == 0,
            "candidate retry must preserve the AP and STA radio");
    mockMillis = 2100;
    WiFi.restored();
    manager.runOwnerPass();
    require(manager.wifiClientState_ == WIFI_CLIENT_CONNECTED && manager.currentConnectedSlotIndex_ == 1,
            "second candidate must connect after its own GOT_IP");
    require(manager.settings_.markCalls == 1 && manager.settings_.markedIndex == 1,
            "only the successful candidate may advance recency");
}

void secondCandidateFailure(wl_status_t secondFailure, bool duringBegin) {
    WiFiManager manager{};
    if (duringBegin) {
        // Install once the second candidate is armed, before its first poll.
        startSecondCandidate(manager, secondFailure);
        // Retrying the same SSID must also clear evidence from the old attempt.
        require(manager.connectToNetwork("Working", "", false, 1, true), "retry must queue");
        WiFi.duringBegin = [secondFailure]() { WiFi.failure("Working", secondFailure); };
        mockMillis = 2030;
        manager.runOwnerPass();
        mockMillis = 2040;
        manager.runOwnerPass();
    } else {
        startSecondCandidate(manager, secondFailure);
        WiFi.failure("Working", secondFailure);
    }
    mockMillis = 2050;
    manager.runOwnerPass();
    require(manager.maintenanceAutoConnectPhase_ == WiFiManager::MaintenanceAutoConnectPhase::COMPLETE,
            "fresh second-candidate failure must finish promptly even when cached status did not change");
    require(WiFi.radioMode == WIFI_AP && manager.settings_.markCalls == 0,
            "all failed candidates must release STA without marking any slot connected");
}

void synchronousStartFailure(bool beginFailure) {
    WiFiManager manager{};
    WiFi = WiFiClass{};
    WiFi.lost();
    WiFi.cachedStatus = WL_CONNECT_FAILED;
    WiFi.staBeginSucceeds = !beginFailure;
    WiFi.staConnectSucceeds = beginFailure;
    mockMillis = 1000;
    require(manager.connectToNetwork("Working", "", false), "manual candidate must queue");
    manager.runOwnerPass();
    mockMillis = 1010;
    manager.runOwnerPass();
    mockMillis = 1050;
    manager.runOwnerPass();
    require(manager.wifiClientState_ == WIFI_CLIENT_FAILED,
            "synchronous driver rejection must fail promptly without an asynchronous event");
    require(manager.settings_.markCalls == 0, "start failure must not mark a connection");
}

void unreportedCandidateFailureTimesOut() {
    WiFiManager manager{};
    startSecondCandidate(manager, WL_NO_SSID_AVAIL);
    mockMillis = 17000;
    manager.runOwnerPass();
    require(manager.wifiClientState_ == WIFI_CLIENT_CONNECTING, "old status must not shorten connection timeout");
    mockMillis = 17100;
    manager.runOwnerPass();
    require(manager.maintenanceAutoConnectPhase_ == WiFiManager::MaintenanceAutoConnectPhase::COMPLETE,
            "missing events must still hit the bounded connection timeout");
    require(WiFi.radioMode == WIFI_AP && manager.settings_.markCalls == 0,
            "timed-out candidate must release STA without claiming success");
}

void transientCandidateDisconnect(wl_status_t firstFailure, uint8_t reason, bool preservesStatus) {
    WiFiManager manager{};
    startSecondCandidate(manager, firstFailure);
    if (!preservesStatus) WiFi.cachedStatus = WL_DISCONNECTED;
    WiFi.emitDisconnected("Working", reason);
    mockMillis = 2050;
    manager.runOwnerPass();
    require(manager.wifiClientState_ == WIFI_CLIENT_CONNECTING,
            "a transient disconnect must not inherit the preceding candidate's terminal status");
    require(WiFi.radioMode == WIFI_AP_STA && manager.settings_.markCalls == 0,
            "transient retries must retain STA without marking a successful connection");
    mockMillis = 2100;
    WiFi.restored();
    manager.runOwnerPass();
    require(manager.wifiClientState_ == WIFI_CLIENT_CONNECTED && manager.currentConnectedSlotIndex_ == 1,
            "candidate must remain eligible to connect after the framework retry");
    require(manager.settings_.markCalls == 1 && manager.settings_.markedIndex == 1,
            "the successful retry must advance only its own recency");
}

void recoverSettingsAfterApOnlyBoot(bool enabled, bool hasSlot, bool manualDisconnect,
                                   bool publishRevision = true) {
    WiFiManager manager{};
    WiFi = WiFiClass{};
    WiFi.lost();
    WiFi.radioMode = WIFI_OFF;
    manager.settings_.value.wifiClientEnabled = false;
    manager.settings_.value.wifiClientSSID = "";
    manager.settings_.pendingRecovery = true;
    mockMillis = 100;
    require(manager.startSetupMode(false), "AP-only startup must succeed with provisional defaults");
    require(manager.wifiClientState_ == WIFI_CLIENT_DISABLED, "provisional client must be disabled");
    if (manualDisconnect) manager.disconnectFromNetwork();
    manager.settings_.value.wifiClientEnabled = enabled;
    if (hasSlot) {
        manager.settings_.value.wifiStaSlots[0].ssid = "Saved";
        manager.settings_.value.wifiClientSSID = "Saved";
    }
    manager.settings_.pendingRecovery = false;
    if (publishRevision) ++manager.settings_.recoveredRevision;
    mockMillis = 1000;
    manager.runOwnerPass();
    mockMillis = 1050;
    manager.runOwnerPass();
    const bool shouldJoin = enabled && hasSlot && !manualDisconnect;
    require(WiFi.scans == (shouldJoin ? 1 : 0), "recovered autojoin eligibility mismatch");
    if (!shouldJoin) {
        for (mockMillis = 1100; mockMillis < 60000; mockMillis += 50) manager.runOwnerPass();
        require(WiFi.scans == 0 && WiFi.begins == 0, "ineligible recovery must not start STA later");
        return;
    }
    // A second recovered snapshot must not restart or cancel a live scan.
    ++manager.settings_.recoveredRevision;
    mockMillis = 1100;
    manager.runOwnerPass();
    require(WiFi.scans == 1 && WiFi.scanAborts == 0, "recovery restarted the active scan");
    WiFi.scanState = 1;
    mockMillis = 1150;
    manager.runOwnerPass();
    mockMillis = 1200;
    manager.runOwnerPass();
    require(WiFi.begins == 1, "recovered network must reach the actual staged connect");
    WiFi.restored();
    mockMillis = 1250;
    manager.runOwnerPass();
    require(manager.wifiClientState_ == WIFI_CLIENT_CONNECTED && manager.currentConnectedSlotIndex_ == 0,
            "recovered network must connect without reboot or user action");
    ++manager.settings_.recoveredRevision;
    mockMillis = 1300;
    manager.runOwnerPass();
    require(WiFi.begins == 1 && WiFi.scans == 1 && WiFi.disconnects == 0,
            "later recovered snapshot must preserve the working connection");
}

void loadedSettingsStartOnlyOneScan() {
    WiFiManager manager{};
    WiFi = WiFiClass{};
    WiFi.lost();
    WiFi.radioMode = WIFI_OFF;
    manager.settings_.value.wifiStaSlots[0].ssid = "Saved";
    manager.settings_.recoveredRevision = 1;
    mockMillis = 100;
    require(manager.startSetupMode(false), "loaded startup failed");
    require(WiFi.scans == 1, "loaded startup must start a scan");
    mockMillis = 1000;
    manager.runOwnerPass();
    require(WiFi.scans == 1 && WiFi.scanAborts == 0, "revision must not duplicate startup autoconnect");
}

void switchFromExistingLink(bool automatic, bool sameSsid, int outcome) {
    WiFiManager manager{};
    WiFi = WiFiClass{};
    manager.settings_.value.wifiStaSlots[0].ssid = "Saved";
    manager.settings_.value.wifiStaSlots[1].ssid = "Working";
    manager.wifiClientState_ = WIFI_CLIENT_CONNECTED;
    manager.currentConnectedSlotIndex_ = 0;
    manager.maintenanceAutoConnectPhase_ = WiFiManager::MaintenanceAutoConnectPhase::COMPLETE;
    const int targetSlot = sameSsid ? 0 : 1;
    const String target = sameSsid ? "Saved" : "Working";
    mockMillis = 1000;
    if (automatic) {
        // The old connection returns between a disconnected snapshot and
        // candidate admission. Exercise the actual scan/candidate owners.
        manager.wifiClientState_ = WIFI_CLIENT_DISCONNECTED;
        WiFi.scanSsids = {target};
        require(manager.beginMaintenanceAutoConnectScan(false), "scan admission");
        WiFi.scanState = 1;
        manager.processMaintenanceAutoConnect();
    } else {
        require(manager.testSavedNetwork(targetSlot), "saved-network test admission");
    }
    WiFi.staBeginSucceeds = outcome != 0;
    WiFi.staConnectSucceeds = outcome != 1;
    manager.processWifiClientConnectPhase();
    mockMillis = 1010;
    manager.processWifiClientConnectPhase();
    require(WiFi.begins == (outcome == 0 ? 0 : 1), "actual start owner must run");
    if (outcome == 2) {
        // A terminal attempt event is authoritative even if an old cached
        // connected value is published again before the owner polls.
        WiFi.failure(target, WL_CONNECT_FAILED);
        WiFi.physicalConnected = true;
        WiFi.cachedStatus = WL_CONNECTED;
    } else if (outcome == 3 && sameSsid) {
        // The driver accepted a reconnect but has not associated yet. The old
        // status alone must not pass even when testing the same network.
        WiFi.physicalConnected = false;
    } else if (outcome == 4) {
        WiFi.restored();
    }
    mockMillis = 1050;
    manager.checkWifiClientStatus();
    if (outcome < 3) {
        require(manager.wifiClientState_ != WIFI_CLIENT_CONNECTED, "failed attempt accepted an old connection");
        require(manager.currentConnectedSlotIndex_ == -1 && manager.settings_.markCalls == 0,
                "failed attempt published target slot or recency");
        // Later physical reconciliation may legitimately publish the actual
        // old network, but must never claim a switch to the failed target.
        mockMillis = 1100;
        manager.checkWifiClientStatus();
        if (!sameSsid) require(manager.currentConnectedSlotIndex_ != 1, "later poll misattributed old connection");
    } else if (outcome == 3) {
        require(manager.wifiClientState_ == WIFI_CLIENT_CONNECTING, "old status/SSID completed the pending attempt");
        require(manager.settings_.markCalls == 0, "old status advanced recency");
        mockMillis = 1100;
        WiFi.restored();
        manager.checkWifiClientStatus();
        require(manager.wifiClientState_ == WIFI_CLIENT_CONNECTED && manager.currentConnectedSlotIndex_ == targetSlot,
                "fresh target association must finish the attempt");
        require(manager.settings_.markCalls == 1 && manager.settings_.markedIndex == static_cast<size_t>(targetSlot),
                "only fresh target association advances recency");
    } else {
        require(manager.wifiClientState_ == WIFI_CLIENT_CONNECTED && manager.currentConnectedSlotIndex_ == targetSlot,
                "successful target switch rejected");
        require(manager.settings_.markCalls == 1 && manager.settings_.markedIndex == static_cast<size_t>(targetSlot),
                "successful target switch must advance its recency exactly once");
        manager.checkWifiClientStatus();
        require(manager.settings_.markCalls == 1, "connected poll duplicated recency");
    }
    require(manager.settings_.credentialWrites == 0, "saved-network test must not rewrite credentials");
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
    run("direct_success_without_pending_slot", []() { successWithoutPendingSlotAdvancesResolvedSlotOnce(true); });
    run("reconnect_success_without_pending_slot", []() { successWithoutPendingSlotAdvancesResolvedSlotOnce(false); });
    run("prior_no_ssid_delayed_association", []() { candidateRetry(WL_NO_SSID_AVAIL, false, false); });
    run("prior_connect_failed_delayed_association", []() { candidateRetry(WL_CONNECT_FAILED, false, false); });
    run("prior_no_ssid_fast_association", []() { candidateRetry(WL_NO_SSID_AVAIL, true, false); });
    run("prior_connect_failed_fast_association", []() { candidateRetry(WL_CONNECT_FAILED, true, false); });
    run("prior_no_ssid_late_event", []() { candidateRetry(WL_NO_SSID_AVAIL, false, true); });
    run("prior_connect_failed_late_event", []() { candidateRetry(WL_CONNECT_FAILED, false, true); });
    run("fresh_same_no_ssid_failure", []() { secondCandidateFailure(WL_NO_SSID_AVAIL, false); });
    run("fresh_same_connect_failure", []() { secondCandidateFailure(WL_CONNECT_FAILED, false); });
    run("immediate_async_no_ssid_failure", []() { secondCandidateFailure(WL_NO_SSID_AVAIL, true); });
    run("immediate_async_connect_failure", []() { secondCandidateFailure(WL_CONNECT_FAILED, true); });
    run("synchronous_sta_start_failure", []() { synchronousStartFailure(true); });
    run("synchronous_sta_connect_failure", []() { synchronousStartFailure(false); });
    run("unreported_failure_timeout", unreportedCandidateFailureTimesOut);
    run("auth_expire_after_no_ssid", []() { transientCandidateDisconnect(WL_NO_SSID_AVAIL, WIFI_REASON_AUTH_EXPIRE, true); });
    run("auth_expire_after_connect_failed", []() { transientCandidateDisconnect(WL_CONNECT_FAILED, WIFI_REASON_AUTH_EXPIRE, true); });
    run("initial_auth_retry", []() { transientCandidateDisconnect(WL_NO_SSID_AVAIL, WIFI_REASON_AUTH_FAIL, false); });
    run("association_retry", []() { transientCandidateDisconnect(WL_CONNECT_FAILED, WIFI_REASON_ASSOC_FAIL, false); });
    run("voluntary_disconnect_event", []() { transientCandidateDisconnect(WL_CONNECT_FAILED, WIFI_REASON_ASSOC_LEAVE, false); });
    run("recovered_saved_network_autojoins", []() { recoverSettingsAfterApOnlyBoot(true, true, false); });
    run("recovered_disabled_client", []() { recoverSettingsAfterApOnlyBoot(false, true, false); });
    run("recovered_empty_networks", []() { recoverSettingsAfterApOnlyBoot(true, false, false); });
    run("recovered_manual_disconnect", []() { recoverSettingsAfterApOnlyBoot(true, true, true); });
    run("sd_only_healed_network_autojoins", []() { recoverSettingsAfterApOnlyBoot(true, true, false, false); });
    run("loaded_settings_start_once", loadedSettingsStartOnlyOneScan);
    run("manual_old_link_sta_start_failure", []() { switchFromExistingLink(false, false, 0); });
    run("manual_old_link_connect_failure", []() { switchFromExistingLink(false, false, 1); });
    run("auto_old_link_connect_failure", []() { switchFromExistingLink(true, false, 1); });
    run("manual_same_ssid_connect_failure", []() { switchFromExistingLink(false, true, 1); });
    run("auto_same_ssid_connect_failure", []() { switchFromExistingLink(true, true, 1); });
    run("manual_old_link_terminal_event", []() { switchFromExistingLink(false, false, 2); });
    run("manual_old_link_pending", []() { switchFromExistingLink(false, false, 3); });
    run("auto_old_link_pending", []() { switchFromExistingLink(true, false, 3); });
    run("manual_same_ssid_cached_connected", []() { switchFromExistingLink(false, true, 3); });
    run("manual_successful_switch", []() { switchFromExistingLink(false, false, 4); });
    run("auto_successful_switch", []() { switchFromExistingLink(true, false, 4); });
    run("manual_successful_same_ssid", []() { switchFromExistingLink(false, true, 4); });
    return failures == 0 ? 0 : 1;
}
