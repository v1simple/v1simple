#pragma once

#include "../../src/modules/wifi/wifi_maintenance_write_policy.h"

namespace {

void seedUnfinishedWifiRollback(SettingsManager& manager) {
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(0, "FirstNetwork", "committed-first", "First", 0));
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(1, "SecondNetwork", "committed-second", "Second", 1));
    Preferences legacy;
    TEST_ASSERT_TRUE(legacy.begin(WIFI_CLIENT_NS, false));
    TEST_ASSERT_GREATER_THAN(0, legacy.putString(kNvsWifiPassword, encodeObfuscatedForStorage("legacy-first")));
    legacy.end();
    // The candidate reaches NVS, legacy cleanup fails, and compensation fails.
    // This returned failure must never become committed by another slot's edit.
    mock_preferences::set_fail_write_for_key_on_call(kNvsWifiPassword, 1);
    mock_preferences::set_fail_write_for_key_on_call(kNvsWifiStaSlotPassword[0], 2);
    TEST_ASSERT_FALSE(manager.setWifiStaSlotCredentials(0, "FirstNetwork", "uncommitted-first", "Changed", 0));
    TEST_ASSERT_TRUE(wifiCredentialJournalPresent());
    TEST_ASSERT_EQUAL_STRING("First", manager.get().wifiStaSlots[0].label.c_str());
}

void assertWifiNamespaceUnchanged(const mock_preferences::NamespaceStore& before) {
    const auto& after = mock_preferences::store().at(WIFI_CLIENT_NS);
    TEST_ASSERT_EQUAL_UINT(before.size(), after.size());
    for (const auto& entry : before) {
        TEST_ASSERT_TRUE(after.count(entry.first) == 1);
        TEST_ASSERT_EQUAL_INT(entry.second.type, after.at(entry.first).type);
        TEST_ASSERT_TRUE(entry.second.value == after.at(entry.first).value);
    }
}

int saveSecondWifiNetworkThroughHttp(SettingsManager& manager) {
    WebServer server;
    server.setArg("plain", "{\"index\":1,\"ssid\":\"SecondNetwork\",\"password\":\"updated-second\",\"label\":\"Second\",\"priority\":1}");
    WifiClientApiService::Runtime runtime;
    runtime.maintenanceBootActive = true;
    // The explicit-index, full-field WiFiManager path forwards these validated
    // fields directly to SettingsManager. Its radio activity tail is not modeled.
    runtime.upsertSavedNetwork = [](const WifiClientApiService::SavedNetworkUpsertPayload& request,
                                    size_t& index, void* context) {
        if (!request.hasIndex || !request.hasPassword || !request.hasLabel || !request.hasPriority) return false;
        index = request.index;
        return static_cast<SettingsManager*>(context)->setWifiStaSlotCredentials(
            index, request.ssid, request.password, request.label, request.priority);
    };
    runtime.upsertSavedNetworkCtx = &manager;
    bool preAdmitted = true;
    WifiMaintenanceWritePolicy::dispatchStorageResolved(server, preAdmitted,
        [&]() { return manager.resolveStorageTransactionsForMutation(); },
        [&]() { WifiClientApiService::handleApiNetworksSave(server, runtime, nullptr, nullptr, nullptr, nullptr); });
    return server.lastStatusCode;
}

void exerciseWifiHttpDiscoveryFault(size_t readFailures) {
    fs::FS filesystem(g_tempRoot);
    storage.setFilesystem(&filesystem, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    seedUnfinishedWifiRollback(manager);
    const auto before = mock_preferences::store().at(WIFI_CLIENT_NS);
    mock_preferences::set_fail_read_only_begin_for_namespace(WIFI_CLIENT_NS, readFailures);
    const int status = saveSecondWifiNetworkThroughHttp(manager);
    if (readFailures) {
        TEST_ASSERT_TRUE(status == 503 || status == 409);
        assertWifiNamespaceUnchanged(before);
    } else {
        TEST_ASSERT_EQUAL_INT(200, status);
    }
    mock_preferences::set_fail_read_only_begin_for_namespace(WIFI_CLIENT_NS, 0);
    SettingsManager reboot(storage, profiles);
    reboot.begin();
    TEST_ASSERT_EQUAL_STRING("committed-first", reboot.getWifiStaSlotPassword(0).c_str());
    TEST_ASSERT_EQUAL_STRING(readFailures ? "committed-second" : "updated-second",
                             reboot.getWifiStaSlotPassword(1).c_str());
    TEST_ASSERT_FALSE(wifiCredentialJournalPresent());
    TEST_ASSERT_EQUAL_INT(200, saveSecondWifiNetworkThroughHttp(reboot));
    SettingsManager finalBoot(storage, profiles);
    finalBoot.begin();
    TEST_ASSERT_EQUAL_STRING("committed-first", finalBoot.getWifiStaSlotPassword(0).c_str());
    TEST_ASSERT_EQUAL_STRING("updated-second", finalBoot.getWifiStaSlotPassword(1).c_str());
}

} // namespace

void test_wifi_http_three_unavailable_discovery_opens_preserve_prior_rollback() {
    exerciseWifiHttpDiscoveryFault(3);
}

void test_wifi_http_two_unavailable_discovery_opens_still_block() {
    exerciseWifiHttpDiscoveryFault(2);
}

void test_wifi_http_healthy_discovery_recovers_before_unrelated_edit() {
    exerciseWifiHttpDiscoveryFault(0);
}

void test_wifi_set_delete_and_forget_preserve_journal_when_discovery_is_unavailable() {
    for (int operation = 0; operation < 4; ++operation) {
        if (operation) { tearDown(); setUp(); }
        fs::FS filesystem(g_tempRoot);
        storage.setFilesystem(&filesystem, true);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        SettingsManager manager(storage, profiles);
        seedUnfinishedWifiRollback(manager);
        const auto before = mock_preferences::store().at(WIFI_CLIENT_NS);
        mock_preferences::set_fail_read_only_begin_for_namespace(WIFI_CLIENT_NS, 2);
        const bool accepted = operation == 0 ? manager.setWifiStaSlotCredentials(1, "SecondNetwork", "updated-second", "Second", 1)
                            : operation == 1 ? manager.clearWifiStaSlot(1)
                            : operation == 2 ? manager.clearWifiClientCredentials()
                                             : manager.setWifiClientCredentials("FirstNetwork", "replacement-first");
        TEST_ASSERT_FALSE(accepted);
        assertWifiNamespaceUnchanged(before);
        mock_preferences::set_fail_read_only_begin_for_namespace(WIFI_CLIENT_NS, 0);
        SettingsManager reboot(storage, profiles);
        reboot.begin();
        TEST_ASSERT_EQUAL_STRING("committed-first", reboot.getWifiStaSlotPassword(0).c_str());
        TEST_ASSERT_EQUAL_STRING("committed-second", reboot.getWifiStaSlotPassword(1).c_str());
        TEST_ASSERT_FALSE(wifiCredentialJournalPresent());
    }
}

void test_wifi_journal_ready_and_mode_read_errors_preserve_authority() {
    for (const char* key : {kNvsWifiTxnReady, kNvsWifiTxnMode}) {
        for (int fault = 0; fault < 3; ++fault) {
            tearDown(); setUp();
            fs::FS filesystem(g_tempRoot);
            storage.setFilesystem(&filesystem, true);
            TEST_ASSERT_TRUE(profiles.begin(storage));
            SettingsManager manager(storage, profiles);
            seedUnfinishedWifiRollback(manager);
            const auto valid = mock_preferences::store().at(WIFI_CLIENT_NS).at(key);
            if (fault == 0) mock_nvs::set_read_result(WIFI_CLIENT_NS, key, -77, mock_nvs::ReadStage::Scalar);
            else {
                Preferences prefs;
                TEST_ASSERT_TRUE(prefs.begin(WIFI_CLIENT_NS, false));
                if (fault == 1) prefs.putString(key, "wrong-type");
                else prefs.putUChar(key, 2);
                prefs.end();
            }
            const auto before = mock_preferences::store().at(WIFI_CLIENT_NS);
            TEST_ASSERT_FALSE(manager.resolveStorageTransactionsForMutation());
            TEST_ASSERT_FALSE(manager.clearWifiClientCredentials());
            assertWifiNamespaceUnchanged(before);
            mock_nvs::reset();
            mock_preferences::store().at(WIFI_CLIENT_NS)[key] = valid;
            SettingsManager reboot(storage, profiles);
            reboot.begin();
            TEST_ASSERT_EQUAL_STRING("committed-first", reboot.getWifiStaSlotPassword(0).c_str());
            TEST_ASSERT_FALSE(wifiCredentialJournalPresent());
        }
    }
}

void test_wifi_unknown_discovery_blocks_sd_recovery_and_backup_import() {
    for (bool import : {false, true}) {
        tearDown(); setUp();
        fs::FS filesystem(g_tempRoot);
        storage.setFilesystem(&filesystem, true);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        SettingsManager manager(storage, profiles);
        seedUnfinishedWifiRollback(manager);
        JsonDocument replacement;
        replacement["_type"] = "v1simple_backup";
        replacement["brightness"] = 66;
        const auto before = mock_preferences::store().at(WIFI_CLIENT_NS);
        const std::string backupBefore = readFileToString(filesystem, SETTINGS_BACKUP_PATH);
        mock_preferences::set_fail_read_only_begin_for_namespace(WIFI_CLIENT_NS, 1);
        if (import) TEST_ASSERT_FALSE(manager.applyBackupDocument(replacement, false).success);
        else {
            TEST_ASSERT_FALSE(manager.checkAndRestoreFromSD());
            TEST_ASSERT_TRUE(manager.recoveryPending());
        }
        assertWifiNamespaceUnchanged(before);
        TEST_ASSERT_EQUAL_STRING(backupBefore.c_str(), readFileToString(filesystem, SETTINGS_BACKUP_PATH).c_str());
        mock_preferences::set_fail_read_only_begin_for_namespace(WIFI_CLIENT_NS, 0);
        manager.checkAndRestoreFromSD();
        TEST_ASSERT_FALSE(manager.recoveryPending());
        TEST_ASSERT_EQUAL_STRING("committed-first", manager.getWifiStaSlotPassword(0).c_str());
        TEST_ASSERT_FALSE(wifiCredentialJournalPresent());
    }
}

void test_wifi_journal_writers_cannot_replace_unresolved_authority() {
    fs::FS filesystem(g_tempRoot);
    storage.setFilesystem(&filesystem, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    seedUnfinishedWifiRollback(manager);
    WifiCredentialJournal single;
    TEST_ASSERT_TRUE(readWifiCredentialJournal(single));
    WifiForgetAllJournal all;
    const auto before = mock_preferences::store().at(WIFI_CLIENT_NS);
    for (size_t failures : {0u, 1u}) {
        mock_preferences::set_fail_read_only_begin_for_namespace(WIFI_CLIENT_NS, failures);
        TEST_ASSERT_FALSE(writeWifiCredentialJournal(single));
        assertWifiNamespaceUnchanged(before);
        mock_preferences::set_fail_read_only_begin_for_namespace(WIFI_CLIENT_NS, failures);
        TEST_ASSERT_FALSE(writeWifiForgetAllJournal(all));
        assertWifiNamespaceUnchanged(before);
    }
    mock_preferences::set_fail_read_only_begin_for_namespace(WIFI_CLIENT_NS, 0);
    TEST_ASSERT_TRUE(manager.resolveStorageTransactionsForMutation());
    TEST_ASSERT_EQUAL_STRING("committed-first", manager.getWifiStaSlotPassword(0).c_str());
}

void test_wifi_legacy_missing_mode_and_proven_absence_keep_recovery_compatible() {
    fs::FS filesystem(g_tempRoot);
    storage.setFilesystem(&filesystem, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    seedUnfinishedWifiRollback(manager);
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(WIFI_CLIENT_NS, false));
    TEST_ASSERT_TRUE(prefs.remove(kNvsWifiTxnMode));
    prefs.end();
    TEST_ASSERT_TRUE(manager.resolveStorageTransactionsForMutation());
    TEST_ASSERT_EQUAL_STRING("committed-first", manager.getWifiStaSlotPassword(0).c_str());
    TEST_ASSERT_FALSE(wifiCredentialJournalPresent());
    mock_nvs::set_next_namespace_open_result(WIFI_CLIENT_NS, ESP_ERR_NVS_NOT_FOUND);
    TEST_ASSERT_TRUE(manager.resolveStorageTransactionsForMutation());
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(1, "SecondNetwork", "updated-second", "Second", 1));
}
