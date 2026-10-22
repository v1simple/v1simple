#pragma once

namespace {

enum class PayloadMutation { Edit, ClearOne, ForgetAll };

void seedPendingWifiPayload(SettingsManager& manager, fs::FS& filesystem,
                            bool unknownPassword, PayloadMutation operation) {
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(0, "SameNetwork", "committed-password", "Committed", 0));
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(1, "OtherNetwork", "other-password", "Other", 1));
    if (unknownPassword) {
        Preferences prefs;
        TEST_ASSERT_TRUE(prefs.begin(WIFI_CLIENT_NS, false));
        TEST_ASSERT_TRUE(prefs.remove(kNvsWifiStaSlotPassword[0]));
        prefs.end();
        storage.setFilesystem(nullptr);
    }
    manager.utInterruptWifiCredentialBeforeSettingsCommit(true);
    const bool committed = operation == PayloadMutation::Edit
        ? manager.setWifiStaSlotCredentials(0, "SameNetwork", "uncommitted-password", "Candidate", 0)
        : operation == PayloadMutation::ClearOne ? manager.clearWifiStaSlot(0)
                                                 : manager.clearWifiClientCredentials();
    TEST_ASSERT_FALSE(committed);
    storage.setFilesystem(&filesystem, true);
    manager.load(); // Select the acknowledged settings copy before journal recovery.
    TEST_ASSERT_EQUAL_STRING("Committed", manager.get().wifiStaSlots[0].label.c_str());
    TEST_ASSERT_TRUE(wifiCredentialJournalPresent());
}

void failWifiPayloadRead(bool unknownPassword, size_t count = 1) {
    mock_preferences::set_scalar_read_fault(WIFI_CLIENT_NS,
        unknownPassword ? kNvsWifiTxnUnknownPasswords : kNvsWifiTxnSettingsGeneration,
        unknownPassword ? PT_U8 : PT_U32, ESP_FAIL, count);
}

void assertRecoveredWifiPayload(fs::FS& filesystem) {
    SettingsManager reboot(storage, profiles);
    reboot.begin();
    TEST_ASSERT_FALSE(wifiCredentialJournalPresent());
    TEST_ASSERT_EQUAL_STRING("committed-password", reboot.getWifiStaSlotPassword(0).c_str());
    TEST_ASSERT_EQUAL_STRING("other-password", reboot.getWifiStaSlotPassword(1).c_str());
    TEST_ASSERT_EQUAL_STRING("Committed", reboot.get().wifiStaSlots[0].label.c_str());
    TEST_ASSERT_TRUE(reboot.setWifiStaSlotCredentials(1, "OtherNetwork", "next-password", "Next", 1));
    SettingsManager finalBoot(storage, profiles);
    finalBoot.begin();
    TEST_ASSERT_EQUAL_STRING("committed-password", finalBoot.getWifiStaSlotPassword(0).c_str());
    TEST_ASSERT_EQUAL_STRING("next-password", finalBoot.getWifiStaSlotPassword(1).c_str());
    TEST_ASSERT_EQUAL_STRING("committed-password", decodeObfuscatedFromStorage(
        loadWifiClientSecretFromSD(storage, "SameNetwork", 0)).c_str());
}

void exerciseWifiPayloadFault(bool unknownPassword, PayloadMutation operation) {
    fs::FS filesystem(g_tempRoot);
    storage.setFilesystem(&filesystem, true);
    TEST_ASSERT_TRUE(profiles.begin(storage));
    SettingsManager manager(storage, profiles);
    seedPendingWifiPayload(manager, filesystem, unknownPassword, operation);
    const auto before = mock_preferences::store().at(WIFI_CLIENT_NS);
    const std::string secretsBefore = readFileToString(filesystem, WIFI_CLIENT_SD_SECRET_PATH);
    const std::string backupBefore = readFileToString(filesystem, SETTINGS_BACKUP_PATH);
    failWifiPayloadRead(unknownPassword);
    TEST_ASSERT_FALSE(manager.resolveStorageTransactionsForMutation());
    assertWifiNamespaceUnchanged(before);
    TEST_ASSERT_EQUAL_STRING(secretsBefore.c_str(), readFileToString(filesystem, WIFI_CLIENT_SD_SECRET_PATH).c_str());
    TEST_ASSERT_EQUAL_STRING(backupBefore.c_str(), readFileToString(filesystem, SETTINGS_BACKUP_PATH).c_str());
    assertRecoveredWifiPayload(filesystem);
}

} // namespace

void test_wifi_generation_read_failure_retains_same_ssid_password_rollback() {
    exerciseWifiPayloadFault(false, PayloadMutation::Edit);
}
void test_wifi_generation_read_failure_retains_clear_one_rollback() {
    exerciseWifiPayloadFault(false, PayloadMutation::ClearOne);
}
void test_wifi_unknown_read_failure_retains_edit_sd_password() {
    exerciseWifiPayloadFault(true, PayloadMutation::Edit);
}
void test_wifi_unknown_read_failure_retains_clear_one_sd_password() {
    exerciseWifiPayloadFault(true, PayloadMutation::ClearOne);
}
void test_wifi_unknown_read_failure_retains_forget_all_sd_password() {
    exerciseWifiPayloadFault(true, PayloadMutation::ForgetAll);
}

void test_wifi_payload_faults_block_next_mutations_and_restore_consumers() {
    for (bool unknownPassword : {false, true}) {
        for (int operation = 0; operation < 6; ++operation) {
            tearDown(); setUp();
            fs::FS filesystem(g_tempRoot);
            storage.setFilesystem(&filesystem, true);
            TEST_ASSERT_TRUE(profiles.begin(storage));
            SettingsManager manager(storage, profiles);
            seedPendingWifiPayload(manager, filesystem, unknownPassword, PayloadMutation::Edit);
            const auto before = mock_preferences::store().at(WIFI_CLIENT_NS);
            const std::string secretsBefore = readFileToString(filesystem, WIFI_CLIENT_SD_SECRET_PATH);
            const std::string backupBefore = readFileToString(filesystem, SETTINGS_BACKUP_PATH);
            JsonDocument replacement;
            replacement["_type"] = "v1simple_backup";
            replacement["brightness"] = 66;
            failWifiPayloadRead(unknownPassword);
            const bool accepted = operation == 0 ? manager.setWifiStaSlotCredentials(1, "OtherNetwork", "replacement", "Other", 1)
                : operation == 1 ? manager.clearWifiStaSlot(1)
                : operation == 2 ? manager.clearWifiClientCredentials()
                : operation == 3 ? manager.setWifiClientCredentials("SameNetwork", "replacement")
                : operation == 4 ? manager.applyBackupDocument(replacement, false).success
                                 : manager.checkAndRestoreFromSD();
            TEST_ASSERT_FALSE(accepted);
            assertWifiNamespaceUnchanged(before);
            TEST_ASSERT_EQUAL_STRING(secretsBefore.c_str(), readFileToString(filesystem, WIFI_CLIENT_SD_SECRET_PATH).c_str());
            TEST_ASSERT_EQUAL_STRING(backupBefore.c_str(), readFileToString(filesystem, SETTINGS_BACKUP_PATH).c_str());
            assertRecoveredWifiPayload(filesystem);
        }
    }
}

void test_wifi_payload_authority_survives_save_and_backup_then_failed_recovery() {
    for (bool unknownPassword : {false, true}) {
        tearDown(); setUp();
        fs::FS filesystem(g_tempRoot);
        storage.setFilesystem(&filesystem, true);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        SettingsManager manager(storage, profiles);
        manager.begin();
        TEST_ASSERT_TRUE(manager.migrateAutoPushProfilesToV2());
        seedPendingWifiPayload(manager, filesystem, unknownPassword, PayloadMutation::Edit);
        const auto before = mock_preferences::store().at(WIFI_CLIENT_NS);
        // Ordinary settings snapshots intentionally omit password-store bytes.
        // Saving the loaded committed settings must not change the WiFi token,
        // provenance, journal, or sidecar while its own recovery is unavailable.
        failWifiPayloadRead(unknownPassword);
        TEST_ASSERT_TRUE(manager.save());
        TEST_ASSERT_TRUE(manager.backupToSD());
        TEST_ASSERT_FALSE(manager.resolveStorageTransactionsForMutation());
        assertWifiNamespaceUnchanged(before);
        assertRecoveredWifiPayload(filesystem);
    }
}

void test_wifi_payload_fault_during_begin_keeps_journal_for_next_boot() {
    for (bool unknownPassword : {false, true}) {
        tearDown(); setUp();
        fs::FS filesystem(g_tempRoot);
        storage.setFilesystem(&filesystem, true);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        SettingsManager manager(storage, profiles);
        seedPendingWifiPayload(manager, filesystem, unknownPassword,
            unknownPassword ? PayloadMutation::ForgetAll : PayloadMutation::Edit);
        const auto before = mock_preferences::store().at(WIFI_CLIENT_NS);
        const std::string secretsBefore = readFileToString(filesystem, WIFI_CLIENT_SD_SECRET_PATH);
        failWifiPayloadRead(unknownPassword, 20);
        SettingsManager unavailableBoot(storage, profiles);
        unavailableBoot.begin();
        TEST_ASSERT_TRUE(unavailableBoot.recoveryPending());
        assertWifiNamespaceUnchanged(before);
        TEST_ASSERT_EQUAL_STRING(secretsBefore.c_str(), readFileToString(filesystem, WIFI_CLIENT_SD_SECRET_PATH).c_str());
        mock_preferences::scalarReadFaults().clear();
        assertRecoveredWifiPayload(filesystem);
    }
}

void test_wifi_legacy_absent_payload_fields_recover_single_and_forget_all() {
    for (bool forgetAll : {false, true}) {
        tearDown(); setUp();
        fs::FS filesystem(g_tempRoot);
        storage.setFilesystem(&filesystem, true);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        SettingsManager manager(storage, profiles);
        // Clearing makes old/new SSIDs distinct, as required by the installed
        // tokenless journal's historical SSID-based selection rule.
        seedPendingWifiPayload(manager, filesystem, false,
            forgetAll ? PayloadMutation::ForgetAll : PayloadMutation::ClearOne);
        Preferences prefs;
        TEST_ASSERT_TRUE(prefs.begin(WIFI_CLIENT_NS, false));
        TEST_ASSERT_TRUE(prefs.remove(kNvsWifiTxnUnknownPasswords));
        if (!forgetAll) {
            TEST_ASSERT_TRUE(prefs.remove(kNvsWifiTxnSettingsGeneration));
            TEST_ASSERT_TRUE(prefs.remove(kNvsWifiTxnMode));
        }
        prefs.end();
        TEST_ASSERT_TRUE(manager.resolveStorageTransactionsForMutation());
        assertRecoveredWifiPayload(filesystem);
    }
}

void test_wifi_optional_payload_wrong_types_and_invalid_values_retain_authority() {
    for (bool unknownPassword : {false, true}) {
        for (bool wrongType : {false, true}) {
            tearDown(); setUp();
            fs::FS filesystem(g_tempRoot);
            storage.setFilesystem(&filesystem, true);
            TEST_ASSERT_TRUE(profiles.begin(storage));
            SettingsManager manager(storage, profiles);
            seedPendingWifiPayload(manager, filesystem, unknownPassword, PayloadMutation::Edit);
            const char* key = unknownPassword ? kNvsWifiTxnUnknownPasswords : kNvsWifiTxnSettingsGeneration;
            const auto valid = mock_preferences::store().at(WIFI_CLIENT_NS).at(key);
            Preferences prefs;
            TEST_ASSERT_TRUE(prefs.begin(WIFI_CLIENT_NS, false));
            if (wrongType) prefs.putString(key, "wrong-type");
            else if (unknownPassword) prefs.putUChar(key, UINT8_MAX);
            else prefs.putUInt(key, 0);
            prefs.end();
            const auto before = mock_preferences::store().at(WIFI_CLIENT_NS);
            TEST_ASSERT_FALSE(manager.resolveStorageTransactionsForMutation());
            assertWifiNamespaceUnchanged(before);
            mock_preferences::store().at(WIFI_CLIENT_NS)[key] = valid;
            assertRecoveredWifiPayload(filesystem);
        }
    }
}

void test_typed_nvs_error_is_false_preferences_presence_but_checked_read_failure() {
    for (bool byte : {false, true}) {
        const char* key = byte ? "byte" : "word";
        Preferences prefs;
        TEST_ASSERT_TRUE(prefs.begin("read_boundary", false));
        if (byte) prefs.putUChar(key, 1);
        else prefs.putUInt(key, 1);
        TEST_ASSERT_TRUE(prefs.isKey(key));
        mock_preferences::set_scalar_read_fault("read_boundary", key, byte ? PT_U8 : PT_U32, ESP_FAIL);
        TEST_ASSERT_FALSE(prefs.isKey(key));
        TEST_ASSERT_TRUE(prefs.isKey(key));
        mock_preferences::set_scalar_read_fault("read_boundary", key, byte ? PT_U8 : PT_U32, ESP_FAIL);
        SettingsNvsReader reader("read_boundary");
        if (byte) reader.getUChar(key);
        else reader.getUInt(key);
        TEST_ASSERT_EQUAL_INT(ESP_FAIL, reader.error());
        TEST_ASSERT_TRUE(prefs.isKey(key));
        prefs.end();
    }
}
