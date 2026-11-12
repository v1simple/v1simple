#pragma once

void requireStationEffects(std::initializer_list<const char*> expected) {
    std::vector<std::string> values;
    for (const char* value : expected) values.emplace_back(value);
    if (values != stationEffects) {
        std::string actual;
        for (const auto& value : stationEffects) actual += value + ",";
        throw std::runtime_error("ordered station effects changed: " + actual);
    }
}

void enableWithNoSavedSlot() {
    WiFiManager manager;
    WiFi = WiFiClass{};
    WiFi.lost();
    manager.settings_.value.wifiClientEnabled = false;
    manager.settings_.value.wifiClientSSID = "";
    stationEffects.clear();
    require(manager.enableWifiClientFromSavedCredentials(), "enabling an empty saved list succeeds");
    require(manager.settings_.value.wifiClientEnabled, "enabled flag was not committed");
    require(manager.station_.clientState() == WIFI_CLIENT_DISCONNECTED, "empty saved list must remain disconnected");
    requireStationEffects({"storage:resolve", "enabled:1"});
}

void enabledAdmittedConnectionIsNoop() {
    WiFiManager manager;
    WiFi = WiFiClass{};
    WiFi.lost();
    require(manager.connectToNetwork(" Pending ", " password ", false, 2), "prior attempt admission");
    stationEffects.clear();
    require(manager.enableWifiClientFromSavedCredentials(), "already enabled admitted lifecycle is a success");
    require(manager.station_.pendingSlot() == 2, "enable replaced an admitted connection");
    requireStationEffects({});
}

void enableRollbackPreservesPending(bool sharedUi) {
    WiFiManager manager;
    WiFi = WiFiClass{};
    WiFi.lost();
    manager.maintenanceBootMode_ = sharedUi;
    manager.settings_.value.wifiClientEnabled = false;
    manager.settings_.value.wifiStaSlots[0].ssid = "Saved";
    manager.settings_.enabledWriteSucceeds = false;
    mockMillis = 1000;
    require(manager.connectToNetwork(" Prior ", " secret ", false, 2), "prior pending admission");
    uint32_t generation = 0;
    if (sharedUi) {
        require(manager.wifiScanOwner_.request(WifiScanConsumer::UI, WifiStationLifecycle::scanDriver()) ==
                    WifiScanResultOwner::RequestResult::STARTED, "UI scan setup");
        generation = manager.wifiScanOwner_.generation();
    }
    stationEffects.clear();
    require(!manager.enableWifiClientFromSavedCredentials(), "enabled persistence failure must reject enable");
    require(!manager.settings_.value.wifiClientEnabled, "failed enabled write changed settings");
    require(manager.station_.clientState() == WIFI_CLIENT_CONNECTING && manager.station_.pendingSlot() == 2,
            "failed enable did not restore the pending attempt");
    require(WiFi.radioMode == WIFI_AP_STA && WiFi.autoReconnect, "failed enable did not restore physical mode/options");
    if (sharedUi) {
        requireStationEffects({"storage:resolve", "enabled:1", "auto:1", "mode:3"});
        require(manager.wifiScanOwner_.isRunning() && manager.wifiScanOwner_.isPending(WifiScanConsumer::UI),
                "enable rollback cancelled the UI scan");
        require(!manager.wifiScanOwner_.isPending(WifiScanConsumer::MAINTENANCE) &&
                    manager.wifiScanOwner_.generation() == generation, "enable rollback retained/replaced its scan consumer");
        require(WiFi.scanAborts == 0, "shared physical scan must not be aborted");
    } else {
        requireStationEffects({"password:legacy", "enabled:1", "auto:1", "mode:3"});
    }
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    mockMillis = 1010;
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    require(WiFi.lastBeginSsid == " Prior " && WiFi.lastBeginPassword == " secret ",
            "rollback changed original credential bytes or phase progression");
}

void enableRollbackCancelsOnlyItsScan() {
    WiFiManager manager;
    WiFi = WiFiClass{};
    WiFi.lost();
    WiFi.radioMode = WIFI_AP;
    manager.settings_.value.wifiClientEnabled = false;
    manager.settings_.value.wifiStaSlots[0].ssid = "Saved";
    manager.settings_.enabledWriteSucceeds = false;
    mockMillis = 1000;
    stationEffects.clear();
    require(!manager.enableWifiClientFromSavedCredentials(), "failed enable commit must reject");
    requireStationEffects({"storage:resolve", "scan:delete", "scan:start", "enabled:1", "scan:abort", "scan:delete",
                           "mode:2", "auto:1", "mode:2"});
    require(!manager.wifiScanOwner_.isRunning() && WiFi.scanAborts == 1, "rollback leaked its sole scan");
    require(manager.station_.clientState() == WIFI_CLIENT_DISABLED && manager.station_.pendingSlot() == -1,
            "rollback changed prior disabled state");
}

void mutationCommitsBeforeRadio(bool erase, bool fail) {
    WiFiManager manager;
    WiFi = WiFiClass{};
    manager.settings_.value.wifiStaSlots[0].ssid = "Saved";
    manager.settings_.value.wifiStaSlots[1].ssid = "Other";
    manager.station_.selectSessionMode(true);
    mockMillis = 1000;
    manager.checkWifiClientStatus();
    require(manager.station_.connectedSlot() == 0, "prior physical saved link admission");
    manager.settings_.credentialWriteSucceeds = !fail;
    manager.settings_.clearSucceeds = !fail;
    stationEffects.clear();
    using WifiClientApiService::SlotActionStatus;
    SlotActionStatus result;
    if (erase) {
        result = manager.deleteSavedNetwork({0, "Saved"});
    } else {
        WifiClientApiService::SavedNetworkUpsertPayload request;
        request.hasIndex = true;
        request.index = 0;
        request.hasExpectedSsid = true;
        request.expectedSsid = "Saved";
        request.ssid = " Saved ";
        request.hasPassword = true;
        request.password = "";
        size_t selected = kWifiStaSlotCount;
        result = manager.upsertSavedNetwork(request, selected);
        require(selected == (fail ? kWifiStaSlotCount : 0), "failed edit changed selected output");
    }
    require(result == (fail ? SlotActionStatus::Failed : SlotActionStatus::Success),
            "credential mutation result changed");
    if (fail) {
        if (erase) requireStationEffects({"clear:0"}); else requireStationEffects({"slot:0"});
        require(manager.station_.clientState() == WIFI_CLIENT_CONNECTED && WiFi.physicalConnected,
                "failed credential commit disconnected a working link");
    } else {
        if (erase) requireStationEffects({"clear:0", "auto:0", "disconnect", "mode:2"});
        else requireStationEffects({"slot:0", "auto:0", "disconnect", "mode:2"});
        require(manager.station_.connectedSlot() == -1 && manager.station_.pendingSlot() == -1,
                "successful mutation retained tracked slot activity");
        require(manager.station_.retryAt() == 6000, "mutation replacement retry changed");
    }
}

void disableAndForgetCommitOrdering(bool forget, bool fail) {
    WiFiManager manager;
    WiFi = WiFiClass{};
    manager.settings_.value.wifiStaSlots[0].ssid = "Saved";
    manager.station_.selectSessionMode(true);
    mockMillis = 1000;
    manager.checkWifiClientStatus();
    manager.settings_.enabledWriteSucceeds = !fail;
    manager.settings_.clearSucceeds = !fail;
    stationEffects.clear();
    require((forget ? manager.forgetWifiClient() : manager.disableWifiClient()) == !fail, "disable/forget result changed");
    if (fail) {
        if (forget) requireStationEffects({"forget"}); else requireStationEffects({"enabled:0"});
        require(WiFi.physicalConnected && manager.station_.clientState() == WIFI_CLIENT_CONNECTED,
                "rejected settings write changed the radio");
    } else {
        if (forget) requireStationEffects({"forget", "auto:0", "disconnect", "mode:2"});
        else requireStationEffects({"enabled:0", "auto:0", "disconnect", "mode:2"});
        require(manager.station_.clientState() == WIFI_CLIENT_DISABLED && manager.station_.retryAt() == 0,
                "committed disable/forget retained connection or retry");
    }
}

void connectedEffectPrecedesRecency() {
    WiFiManager manager;
    WiFi = WiFiClass{};
    WiFi.lost();
    manager.settings_.value.wifiStaSlots[0].ssid = "Saved";
    mockMillis = 1000;
    require(manager.connectToNetwork("Saved", "password", false, 0), "connect admission");
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    mockMillis = 1010;
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    mockMillis = 2000;
    manager.settings_.beforeMark = [&]() {
        require(manager.lastClientSeenMs_ == 2000, "connected effect must arm AP timer before recency write");
    };
    WiFi.restored();
    stationEffects.clear();
    manager.checkWifiClientStatus();
    requireStationEffects({"recency:0"});
    require(manager.station_.connectedSlot() == 0 && manager.station_.pendingSlot() == -1, "success retirement changed");
}

void connectTimeoutBoundary() {
    WiFiManager manager;
    WiFi = WiFiClass{};
    WiFi.lost();
    mockMillis = 1000;
    require(manager.connectToNetwork("Saved", "", false), "timeout attempt admission");
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    mockMillis = 16000;
    manager.checkWifiClientStatus();
    require(manager.station_.clientState() == WIFI_CLIENT_CONNECTING, "15s exact boundary timed out early");
    mockMillis = 16001;
    stationEffects.clear();
    manager.checkWifiClientStatus();
    require(manager.station_.clientState() == WIFI_CLIENT_FAILED && manager.station_.pendingSlot() == -1,
            "timeout failed to retire attempt");
    requireStationEffects({"disconnect"});
}

void normalBootDeferralBoundary() {
    WiFiManager manager;
    WiFi = WiFiClass{};
    WiFi.lost();
    manager.maintenanceBootMode_ = false;
    manager.setupModeStartTime_ = 1;
    manager.station_.selectSessionMode(true);
    mockMillis = 90000;
    stationEffects.clear();
    manager.checkWifiClientStatus();
    require(manager.station_.clientState() == WIFI_CLIENT_DISCONNECTED, "normal reconnect bypassed V1 grace");
    requireStationEffects({});
    mockMillis = 90001;
    manager.checkWifiClientStatus();
    require(manager.station_.clientState() == WIFI_CLIENT_CONNECTING, "normal reconnect missed exact grace boundary");
    requireStationEffects({"password:legacy"});
}

void nonblockingSettleBoundaries() {
    WiFiManager manager;
    WiFi = WiFiClass{};
    WiFi.lost();
    manager.fixtureSetupActive = false;
    mockMillis = 1000;
    require(manager.connectToNetwork("Saved", "", false), "non-AP connect admission");
    stationEffects.clear();
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    requireStationEffects({"disconnect", "mode:0"});
    mockMillis = 1099;
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    requireStationEffects({"disconnect", "mode:0"});
    mockMillis = 1100;
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    requireStationEffects({"disconnect", "mode:0"});
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    requireStationEffects({"disconnect", "mode:0", "mode:3"});
    mockMillis = 1199;
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    require(WiFi.begins == 0, "AP+STA settle started the connection early");
    mockMillis = 1200;
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    require(WiFi.begins == 0, "phase selection must not run the next phase in the same pass");
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    requireStationEffects({"disconnect", "mode:0", "mode:3", "auto:1", "connect:Saved"});
}

void scanTimeoutKeepsUiConsumer(bool sharedUi) {
    WiFiManager manager;
    WiFi = WiFiClass{};
    WiFi.lost();
    manager.settings_.value.wifiStaSlots[0].ssid = "Saved";
    mockMillis = 1000;
    require(manager.station_.beginMaintenanceAutoConnectScan(manager.stationEnvironment(), false), "scan admission");
    if (sharedUi) {
        require(manager.wifiScanOwner_.request(WifiScanConsumer::UI, WifiStationLifecycle::scanDriver()) ==
                    WifiScanResultOwner::RequestResult::JOINED, "UI scan join");
    }
    mockMillis = 15999;
    manager.station_.processMaintenanceAutoConnect(manager.stationEnvironment());
    require(manager.wifiScanOwner_.isPending(WifiScanConsumer::MAINTENANCE), "scan timed out before15s");
    mockMillis = 16000;
    stationEffects.clear();
    manager.station_.processMaintenanceAutoConnect(manager.stationEnvironment());
    require(!manager.wifiScanOwner_.isPending(WifiScanConsumer::MAINTENANCE), "timed out maintenance scan retained consumer");
    require(manager.station_.retryAt() == 46000, "scan timeout retry is not30s");
    if (sharedUi) {
        requireStationEffects({"auto:0", "disconnect"});
        require(manager.wifiScanOwner_.isRunning() && WiFi.scanAborts == 0, "timeout aborted a shared UI scan");
        WiFi.scanState = 1;
        manager.station_.processMaintenanceAutoConnect(manager.stationEnvironment());
        require(manager.wifiScanOwner_.hasSnapshot(WifiScanConsumer::UI), "timeout destroyed eventual UI results");
        require(WiFi.radioMode == WIFI_AP, "STA drop did not wait until shared scan completion");
    } else {
        requireStationEffects({"scan:abort", "scan:delete", "auto:0", "disconnect", "mode:2"});
        require(!manager.wifiScanOwner_.isRunning() && WiFi.scanAborts == 1, "sole expired scan not retired");
    }
}

void collisionAdvancesCandidateAndRetainsMask() {
    WiFiManager manager;
    WiFi = WiFiClass{};
    WiFi.lost();
    manager.settings_.value.wifiStaSlots[0].ssid = "Saved";
    manager.settings_.value.wifiStaSlots[0].priority = 0;
    manager.settings_.value.wifiStaSlots[1].ssid = "Working";
    manager.settings_.value.wifiStaSlots[1].priority = 1;
    WiFi.scanSsids = {"Saved", "Working"};
    mockMillis = 1000;
    require(manager.station_.beginMaintenanceAutoConnectScan(manager.stationEnvironment(), false), "collision scan admission");
    WiFi.scanState = 2;
    manager.station_.processMaintenanceAutoConnect(manager.stationEnvironment());
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    WiFi.restored();
    WiFi.stationIp = 0x0104a8c0;
    manager.checkWifiClientStatus();
    require(manager.station_.clientState() == WIFI_CLIENT_CONNECTING && manager.station_.pendingSlot() == 1,
            "collision did not advance to the next saved candidate");
    require(manager.settings_.markCalls == 0, "colliding candidate was published as connected");
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    WiFi.stationIp = 0x6401a8c0U;
    WiFi.restored();
    manager.checkWifiClientStatus();
    require(manager.station_.connectedSlot() == 1, "second candidate not admitted");
    mockMillis = 2000;
    WiFi.lost();
    manager.checkWifiClientStatus();
    mockMillis = 7000;
    manager.checkWifiClientStatus();
    WiFi.scanState = 2;
    manager.station_.processMaintenanceAutoConnect(manager.stationEnvironment());
    require(manager.station_.pendingSlot() == 1, "later retry forgot the colliding slot mask");
}

void normalRetryIntervalAndGiveUp(uint32_t start) {
    WiFiManager manager;
    WiFi = WiFiClass{};
    WiFi.lost();
    WiFi.staBeginSucceeds = false;
    manager.maintenanceBootMode_ = false;
    manager.station_.selectSessionMode(true);
    uint32_t lastAttempt = start;
    for (int attempt = 1; attempt <= 4; ++attempt) {
        if (attempt > 1) {
            mockMillis = static_cast<uint32_t>(lastAttempt + 30000u);
            manager.checkWifiClientStatus();
            require(manager.station_.clientState() == WIFI_CLIENT_FAILED, "retry interval admitted exact30s boundary");
            lastAttempt = static_cast<uint32_t>(lastAttempt + 30001u);
        }
        mockMillis = lastAttempt;
        manager.checkWifiClientStatus();
        require(manager.station_.clientState() == WIFI_CLIENT_CONNECTING, "due normal reconnect was not queued");
        manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
        manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
        manager.checkWifiClientStatus();
        require(manager.station_.clientState() == WIFI_CLIENT_FAILED, "synchronous failure was not retired");
    }
    mockMillis = static_cast<uint32_t>(lastAttempt + 30001u);
    stationEffects.clear();
    manager.checkWifiClientStatus();
    require(manager.isReconnectGaveUp(), "fifth failure decision must stop reconnect attempts");
    require(manager.station_.clientState() == WIFI_CLIENT_FAILED, "give-up unexpectedly queued another attempt");
    requireStationEffects({"password:legacy"});
    stationEffects.clear();
    mockMillis = static_cast<uint32_t>(mockMillis + 30001u);
    manager.checkWifiClientStatus();
    requireStationEffects({});
    manager.resetReconnectFailures();
    manager.checkWifiClientStatus();
    require(!manager.isReconnectGaveUp() && manager.station_.clientState() == WIFI_CLIENT_CONNECTING,
            "explicit reconnect reset did not restore admission");
}

void restoreDuringSavedAttempt(bool radioStarted, bool retainOtherLink, bool automatic) {
    WiFiManager manager{};
    WiFi = WiFiClass{};
    manager.settings_.value.wifiStaSlots[0].ssid = "Saved";
    manager.settings_.value.wifiStaSlots[1].ssid = "Test target";
    manager.station_.selectSessionMode(true);
    if (!retainOtherLink) WiFi.lost();
    mockMillis = 500;
    manager.checkWifiClientStatus();
    manager.settings_.markCalls = 0;

    mockMillis = 1000;
    if (automatic) {
        WiFi.scanSsids = {"Test target"};
        require(manager.station_.beginMaintenanceAutoConnectScan(manager.stationEnvironment(), false), "scan admission");
        WiFi.scanState = 1;
        manager.station_.processMaintenanceAutoConnect(manager.stationEnvironment());
    } else {
        require(manager.testSavedNetwork({1, "Test target"}) == WifiClientApiService::SlotActionStatus::Success,
                "saved-network test admission");
    }
    if (radioStarted) {
        manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
        mockMillis = 1010;
        manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
        require(WiFi.begins == 1 && WiFi.lastBeginSsid == "Test target", "radio attempt did not start");
    }

    // The maintenance restore replaces the effective slot without using the
    // individual saved-network mutation callback.
    manager.settings_.value.wifiStaSlots[1].ssid = "Replacement";
    if (radioStarted) {
        WiFi.restored();
        WiFi.stationIp = static_cast<uint32_t>(WiFi.softAPIP());
    } else {
        manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
        mockMillis = 1010;
        manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
        require(WiFi.begins == 0, "obsolete credentials reached radio start");
    }
    mockMillis = 1050;
    manager.checkWifiClientStatus();
    require(manager.station_.pendingSlot() == -1 && manager.settings_.markCalls == 0,
            "obsolete attempt retained ownership or wrote replacement recency");
    if (retainOtherLink) {
        require(manager.station_.connectedSlot() == 0 && WiFi.physicalConnected && WiFi.SSID() == "Saved" &&
                    WiFi.disconnects == 0,
                "retiring the attempt disturbed a different authorized link");
    } else {
        require(manager.station_.connectedSlot() == -1 && !WiFi.physicalConnected,
                "obsolete association was retained");
    }
    if (radioStarted) {
        // A late address collision from the old SSID must not blacklist the
        // new occupant. The ordinary retry must still reach its credentials.
        mockMillis = manager.station_.retryAt();
        WiFi.scanSsids = {"Replacement"};
        manager.checkWifiClientStatus();
        WiFi.scanState = 1;
        manager.station_.processMaintenanceAutoConnect(manager.stationEnvironment());
        require(manager.station_.pendingSlot() == 1, "late old association blocked the replacement retry");
    }
}

void restoreChangesConnectedNetwork(bool moveToAnotherSlot) {
    WiFiManager manager{};
    WiFi = WiFiClass{};
    manager.settings_.value.wifiStaSlots[0].ssid = "Saved";
    manager.station_.selectSessionMode(true);
    mockMillis = 500;
    manager.checkWifiClientStatus();
    manager.settings_.markCalls = 0;
    require(manager.station_.connectedSlotForSsid(WiFi.SSID()) == 0, "original link is not attributed");

    manager.settings_.value.wifiStaSlots[0].ssid = "Replacement";
    if (moveToAnotherSlot) manager.settings_.value.wifiStaSlots[1].ssid = "Saved";
    require(manager.station_.connectedSlotForSsid(WiFi.SSID()) == -1, "status exposes a stale slot before service");
    mockMillis = 550;
    manager.checkWifiClientStatus();
    require(manager.settings_.markCalls == 0, "catalog restore fabricated a fresh association");
    if (moveToAnotherSlot) {
        require(WiFi.physicalConnected && WiFi.disconnects == 0 &&
                    manager.station_.connectedSlotForSsid(WiFi.SSID()) == 1,
                "valid restored SSID was disconnected or attributed to the replacement");
    } else {
        require(!WiFi.physicalConnected && manager.station_.connectedSlot() == -1,
                "removed saved network remained active");
    }
}

void restoreDisablesSavedConnection(bool stagedTest, bool colliding = false) {
    WiFiManager manager{};
    WiFi = WiFiClass{};
    manager.settings_.value.wifiStaSlots[0].ssid = "Saved";
    manager.station_.selectSessionMode(true);
    mockMillis = 500;
    manager.checkWifiClientStatus();
    if (stagedTest) {
        require(manager.testSavedNetwork({0, "Saved"}) == WifiClientApiService::SlotActionStatus::Success,
                "saved-network test admission");
        require(manager.wifiScanOwner_.request(WifiScanConsumer::UI, WifiStationLifecycle::scanDriver()) ==
                    WifiScanResultOwner::RequestResult::STARTED,
                "shared radio scan must start before restore");
    }
    manager.settings_.value.wifiClientEnabled = false;
    if (colliding) WiFi.stationIp = static_cast<uint32_t>(WiFi.softAPIP());
    mockMillis = 550;
    manager.station_.processWifiClientConnectPhase(manager.stationEnvironment());
    manager.checkWifiClientStatus();
    require(manager.station_.clientState() == WIFI_CLIENT_DISABLED && manager.station_.pendingSlot() == -1 &&
                manager.station_.retryAt() == 0 && !WiFi.physicalConnected && WiFi.begins == 0,
            "a disabled restored client still owns station activity");
    manager.settings_.markCalls = 0;
    WiFi.physicalConnected = true;
    WiFi.cachedStatus = WL_CONNECTED;
    mockMillis = 600;
    manager.checkWifiClientStatus();
    require(manager.station_.clientState() == WIFI_CLIENT_DISABLED && !WiFi.physicalConnected &&
                manager.settings_.markCalls == 0,
            "a late physical association bypassed the restored disabled state");
    if (stagedTest) {
        require(manager.wifiScanOwner_.isRunning() && WiFi.scanAborts == 0,
                "retiring station disturbed the UI's active scan");
        WiFi.scanState = 1;
        manager.station_.processMaintenanceAutoConnect(manager.stationEnvironment());
        require(manager.station_.clientState() == WIFI_CLIENT_DISABLED && WiFi.radioMode == WIFI_AP,
                "deferred station radio drop reopened the disabled state");
    }
}
