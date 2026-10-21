#pragma once

// Exercise the same admission, GET/POST handlers and reboot used by a full
// backup restore. The setup never calls the password getter: that would heal
// NVS and hide a missing fallback source from the restore under test.
namespace {

enum class RestorePasswordSource { Nvs, LegacyNvs, Sidecar, PreviousSidecar, BackupSlots, BackupLegacy };

void prepareRestorePasswordSource(fs::FS& filesystem, SettingsManager& manager, size_t slot,
                                  RestorePasswordSource source, size_t backupPath = 0) {
    makeCurrentV21Source(manager.mutableSettings());
    TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(slot, "RestoreNet", "retained-secret", "Saved", 0));
    if (source == RestorePasswordSource::Nvs) return;
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(WIFI_CLIENT_NS, false));
    const String encoded = prefs.getString(kNvsWifiStaSlotPassword[slot], "");
    TEST_ASSERT_TRUE(prefs.remove(kNvsWifiStaSlotPassword[slot]));
    if (source == RestorePasswordSource::LegacyNvs) {
        TEST_ASSERT_EQUAL_UINT(encoded.length(), prefs.putString(kNvsWifiPassword, encoded));
    }
    prefs.end();
    if (source == RestorePasswordSource::PreviousSidecar) {
        const String previous = StorageManager::rollbackPathFor(WIFI_CLIENT_SD_SECRET_PATH);
        filesystem.remove(previous);
        TEST_ASSERT_TRUE(filesystem.rename(WIFI_CLIENT_SD_SECRET_PATH, previous));
    }
    if (source == RestorePasswordSource::LegacyNvs || source == RestorePasswordSource::BackupSlots ||
        source == RestorePasswordSource::BackupLegacy) {
        filesystem.remove(WIFI_CLIENT_SD_SECRET_PATH);
        filesystem.remove(StorageManager::rollbackPathFor(WIFI_CLIENT_SD_SECRET_PATH));
    }
    if (source == RestorePasswordSource::BackupSlots || source == RestorePasswordSource::BackupLegacy) {
        for (size_t index = 0; index < SETTINGS_BACKUP_CANDIDATES_COUNT; ++index) {
            filesystem.remove(SETTINGS_BACKUP_CANDIDATES[index]);
        }
        JsonDocument legacy;
        legacy["_version"] = 20;
        legacy["brightness"] = manager.get().brightness;
        legacy["wifiClientSSID"] = "RestoreNet";
        if (source == RestorePasswordSource::BackupSlots) {
            JsonObject entry = legacy["wifiStaSlots"].to<JsonArray>().add<JsonObject>();
            entry["index"] = slot;
            entry["ssid"] = "RestoreNet";
            entry["passwordObf"] = encoded;
        } else {
            legacy["wifiClientPasswordObf"] = encoded;
        }
        writeJsonFile(filesystem, SETTINGS_BACKUP_CANDIDATES[backupPath], legacy);
    }
}

void assertRestoredPassword(SettingsManager& manager, size_t slot, const char* expected) {
    TEST_ASSERT_EQUAL_STRING(expected, manager.getWifiStaSlotPassword(slot).c_str());
    SettingsManager reboot(storage, profiles);
    reboot.begin();
    TEST_ASSERT_EQUAL_STRING(expected, reboot.getWifiStaSlotPassword(slot).c_str());
}

} // namespace

void test_http_restore_preserves_every_recoverable_password_source_and_slot_move() {
    for (size_t slot = 0; slot < kWifiStaSlotCount; ++slot) {
        for (RestorePasswordSource source : {RestorePasswordSource::Nvs, RestorePasswordSource::LegacyNvs,
                                            RestorePasswordSource::Sidecar, RestorePasswordSource::PreviousSidecar,
                                            RestorePasswordSource::BackupSlots, RestorePasswordSource::BackupLegacy}) {
            if (source == RestorePasswordSource::LegacyNvs && slot != 0) continue;
            const size_t paths = source == RestorePasswordSource::BackupSlots || source == RestorePasswordSource::BackupLegacy
                                     ? SETTINGS_BACKUP_CANDIDATES_COUNT : 1;
            for (size_t path = 0; path < paths; ++path) for (bool move : {false, true}) {
                tearDown(); setUp();
                fs::FS filesystem(g_tempRoot);
                storage.setFilesystem(&filesystem, true);
                TEST_ASSERT_TRUE(profiles.begin(storage));
                SettingsManager manager(storage, profiles);
                prepareRestorePasswordSource(filesystem, manager, slot, source, path);
                JsonDocument incoming;
                downloadCurrentHttpBackup(manager, incoming);
                const size_t destination = move ? (slot + 1u) % kWifiStaSlotCount : slot;
                if (move) {
                    for (JsonObject entry : incoming["wifiStaSlots"].as<JsonArray>()) {
                        if (entry["index"].as<size_t>() == slot) entry["index"] = destination;
                    }
                }
                TEST_ASSERT_TRUE(manager.resolveStorageTransactionsForMutation());
                TEST_ASSERT_EQUAL_INT(200, restoreHttpBackup(manager, incoming));
                // Inspect durable NVS before the getter can fall back/heal it.
                TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[destination]));
                assertRestoredPassword(manager, destination, "retained-secret");
            }
        }
    }
}

void test_http_legacy_same_ssid_restore_uses_the_same_password_resolution() {
    for (RestorePasswordSource source : {RestorePasswordSource::Nvs, RestorePasswordSource::LegacyNvs,
                                        RestorePasswordSource::Sidecar, RestorePasswordSource::PreviousSidecar,
                                        RestorePasswordSource::BackupSlots, RestorePasswordSource::BackupLegacy}) {
        tearDown(); setUp();
        fs::FS filesystem(g_tempRoot);
        storage.setFilesystem(&filesystem, true);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        SettingsManager manager(storage, profiles);
        prepareRestorePasswordSource(filesystem, manager, 0, source);
        JsonDocument incoming;
        incoming["_type"] = "v1simple_http_backup";
        incoming["wifiClientSSID"] = "RestoreNet";
        TEST_ASSERT_TRUE(manager.resolveStorageTransactionsForMutation());
        TEST_ASSERT_EQUAL_INT(200, restoreHttpBackup(manager, incoming));
        assertRestoredPassword(manager, 0, "retained-secret");
    }
}

void test_http_restore_distinguishes_explicit_open_password_from_absent_offline_password() {
    for (bool sd : {false, true}) for (bool explicitEmpty : {false, true}) {
        for (size_t slot = 0; slot < kWifiStaSlotCount; ++slot) {
            tearDown(); setUp();
            fs::FS filesystem(g_tempRoot);
            storage.setFilesystem(&filesystem, sd);
            if (!sd) storage.setLittleFS(&filesystem);
            TEST_ASSERT_TRUE(profiles.begin(storage));
            SettingsManager manager(storage, profiles);
            makeCurrentV21Source(manager.mutableSettings());
            TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(slot, "OpenNet", "", "Saved", 0));
            if (!explicitEmpty) {
                Preferences prefs;
                TEST_ASSERT_TRUE(prefs.begin(WIFI_CLIENT_NS, false));
                TEST_ASSERT_TRUE(prefs.remove(kNvsWifiStaSlotPassword[slot]));
                prefs.end();
                filesystem.remove(WIFI_CLIENT_SD_SECRET_PATH);
                filesystem.remove(StorageManager::rollbackPathFor(WIFI_CLIENT_SD_SECRET_PATH));
            }
            JsonDocument incoming;
            downloadCurrentHttpBackup(manager, incoming);
            TEST_ASSERT_TRUE(manager.resolveStorageTransactionsForMutation());
            TEST_ASSERT_EQUAL_INT(200, restoreHttpBackup(manager, incoming));
            TEST_ASSERT_EQUAL(explicitEmpty, mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[slot]));
            assertRestoredPassword(manager, slot, "");
        }
    }
}

void test_http_restore_explicit_replacement_empty_and_changed_ssid_take_precedence() {
    for (size_t slot = 0; slot < kWifiStaSlotCount; ++slot) for (int change = 0; change < 3; ++change) {
        tearDown(); setUp();
        fs::FS filesystem(g_tempRoot);
        storage.setFilesystem(&filesystem, true);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        SettingsManager manager(storage, profiles);
        prepareRestorePasswordSource(filesystem, manager, slot, RestorePasswordSource::PreviousSidecar);
        // Legacy envelopes allow embedded passwords; current HTTP exports intentionally omit them.
        JsonDocument incoming;
        incoming["_type"] = "v1simple_http_backup";
        JsonObject entry = incoming["wifiStaSlots"].to<JsonArray>().add<JsonObject>();
        entry["index"] = slot;
        entry["ssid"] = change == 2 ? "DifferentNet" : "RestoreNet";
        if (change != 2) entry["passwordObf"] = encodeObfuscatedForStorage(change == 0 ? "replacement" : "");
        TEST_ASSERT_TRUE(manager.resolveStorageTransactionsForMutation());
        if (change == 1) {
            // An explicit empty encoded import is invalid under the existing
            // backup format; it must leave the recoverable password untouched.
            TEST_ASSERT_NOT_EQUAL(200, restoreHttpBackup(manager, incoming));
            assertRestoredPassword(manager, slot, "retained-secret");
        } else {
            TEST_ASSERT_EQUAL_INT(200, restoreHttpBackup(manager, incoming));
            TEST_ASSERT_EQUAL(change != 2, mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[slot]));
            assertRestoredPassword(manager, slot, change == 0 ? "replacement" : "");
        }
    }
}

void test_http_restore_credential_source_errors_preserve_bytes_then_retry() {
    for (int fault = 0; fault < 5; ++fault) {
        tearDown(); setUp();
        fs::FS filesystem(g_tempRoot);
        storage.setFilesystem(&filesystem, true);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        SettingsManager manager(storage, profiles);
        const auto source = fault == 3 ? RestorePasswordSource::BackupLegacy : RestorePasswordSource::PreviousSidecar;
        prepareRestorePasswordSource(filesystem, manager, 0, source);
        JsonDocument incoming;
        downloadCurrentHttpBackup(manager, incoming);
        const V1Settings before = manager.get();
        const String previous = StorageManager::rollbackPathFor(WIFI_CLIENT_SD_SECRET_PATH);
        const std::string secretBefore = readFileToString(filesystem, previous.c_str());
        const std::string backupBefore = readFileToString(filesystem, SETTINGS_BACKUP_PATH);
        TEST_ASSERT_TRUE(manager.resolveStorageTransactionsForMutation());
        if (fault == 0) mock_nvs::set_read_result(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[0], -77, mock_nvs::ReadStage::Presence);
        if (fault == 1) mock_nvs::set_next_namespace_open_result(WIFI_CLIENT_NS, -77);
        if (fault == 2) fs::mock_fail_next_read_open(previous.c_str());
        if (fault == 3) fs::mock_fail_next_read_open(SETTINGS_BACKUP_PATH);
        if (fault == 4) mock_preferences::set_fail_writes_for_key(kNvsBrightness);
        TEST_ASSERT_NOT_EQUAL(200, restoreHttpBackup(manager, incoming));
        assertSettingsEqual(before, manager.get());
        TEST_ASSERT_EQUAL_STRING(backupBefore.c_str(), readFileToString(filesystem, SETTINGS_BACKUP_PATH).c_str());
        // Compensation may promote a valid .prev to live; it must preserve the bytes.
        const std::string preserved = filesystem.exists(previous) ? readFileToString(filesystem, previous.c_str())
                                                                 : readFileToString(filesystem, WIFI_CLIENT_SD_SECRET_PATH);
        TEST_ASSERT_EQUAL_STRING(secretBefore.c_str(), preserved.c_str());
        mock_nvs::readResults().clear();
        mock_preferences::set_fail_writes_for_key(nullptr);
        TEST_ASSERT_TRUE(manager.resolveStorageTransactionsForMutation());
        TEST_ASSERT_EQUAL_INT(200, restoreHttpBackup(manager, incoming));
        assertRestoredPassword(manager, 0, "retained-secret");
    }
}

void test_http_restore_interruption_rolls_back_sd_only_password_before_retry() {
    for (RestorePasswordSource source : {RestorePasswordSource::Sidecar, RestorePasswordSource::PreviousSidecar,
                                        RestorePasswordSource::BackupLegacy}) {
        tearDown(); setUp();
        fs::FS filesystem(g_tempRoot);
        storage.setFilesystem(&filesystem, true);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        SettingsManager manager(storage, profiles);
        prepareRestorePasswordSource(filesystem, manager, 0, source);
        JsonDocument incoming;
        downloadCurrentHttpBackup(manager, incoming);
        TEST_ASSERT_TRUE(manager.resolveStorageTransactionsForMutation());
        manager.utInterruptRestoreAfterCredentials(true);
        TEST_ASSERT_NOT_EQUAL(200, restoreHttpBackup(manager, incoming));
        SettingsManager reboot(storage, profiles);
        reboot.begin();
        TEST_ASSERT_EQUAL(source == RestorePasswordSource::BackupLegacy,
                          mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[0]));
        TEST_ASSERT_TRUE(reboot.resolveStorageTransactionsForMutation());
        TEST_ASSERT_EQUAL_INT(200, restoreHttpBackup(reboot, incoming));
        assertRestoredPassword(reboot, 0, "retained-secret");
    }
}

void test_wifi_healing_late_namespace_faults_leave_disabled_and_retry() {
    for (size_t occurrence = 1; occurrence <= 12; ++occurrence) {
        tearDown(); setUp();
        fs::FS filesystem(g_tempRoot);
        storage.setFilesystem(&filesystem, true);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        SettingsManager manager(storage, profiles);
        prepareRestorePasswordSource(filesystem, manager, 0, RestorePasswordSource::Nvs);
        TEST_ASSERT_TRUE(manager.setWifiClientEnabled(false).success);
        const std::string backup = readFileToString(filesystem, SETTINGS_BACKUP_PATH);
        const String active = activeNamespaceOrEmpty();
        mock_nvs::set_nth_namespace_open_result(active.c_str(), -77, occurrence);
        manager.checkAndRestoreFromSD();
        TEST_ASSERT_FALSE(manager.get().wifiClientEnabled);
        TEST_ASSERT_EQUAL_STRING(backup.c_str(), readFileToString(filesystem, SETTINGS_BACKUP_PATH).c_str());
        mock_nvs::namespaceOpenResults().clear();
        manager.serviceDeferredPersist(5000);
        TEST_ASSERT_FALSE(manager.recoveryPending());
        TEST_ASSERT_FALSE(manager.get().wifiClientEnabled);
        SettingsManager reboot(storage, profiles);
        reboot.begin();
        TEST_ASSERT_FALSE(reboot.get().wifiClientEnabled);
    }
}

void test_wifi_healing_key_probe_failures_preserve_explicit_disabled_and_recover_missing_keys() {
    for (const char* key : {kNvsWifiClientEnabled, kNvsWifiStaSlotSsid[0], kNvsWifiClientSsid}) {
        for (bool missingEnabled : {false, true}) for (size_t occurrence = 1; occurrence <= 6; ++occurrence) {
            tearDown(); setUp();
            fs::FS filesystem(g_tempRoot);
            storage.setFilesystem(&filesystem, true);
            TEST_ASSERT_TRUE(profiles.begin(storage));
            SettingsManager manager(storage, profiles);
            prepareRestorePasswordSource(filesystem, manager, 0, RestorePasswordSource::Nvs);
            TEST_ASSERT_TRUE(manager.setWifiClientEnabled(false).success);
            const String active = activeNamespaceOrEmpty();
            if (missingEnabled) {
                Preferences prefs;
                TEST_ASSERT_TRUE(prefs.begin(active.c_str(), false));
                TEST_ASSERT_TRUE(prefs.remove(kNvsWifiClientEnabled));
                prefs.end();
            }
            mock_nvs::set_nth_read_result(active.c_str(), key, -77, mock_nvs::ReadStage::Presence, occurrence);
            manager.checkAndRestoreFromSD();
            if (!missingEnabled) TEST_ASSERT_FALSE(manager.get().wifiClientEnabled);
            mock_nvs::readResults().clear();
            manager.serviceDeferredPersist(5000);
            TEST_ASSERT_FALSE(manager.recoveryPending());
            TEST_ASSERT_EQUAL(missingEnabled, manager.get().wifiClientEnabled);
            SettingsManager reboot(storage, profiles);
            reboot.begin();
            TEST_ASSERT_EQUAL(missingEnabled, reboot.get().wifiClientEnabled);
        }
    }
}

void test_http_legacy_password_replacement_targets_each_primary_slot_and_preserves_other_slots() {
    for (size_t primary = 0; primary < kWifiStaSlotCount; ++primary) for (bool plain : {false, true}) {
        tearDown(); setUp();
        fs::FS filesystem(g_tempRoot);
        storage.setFilesystem(&filesystem, true);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        SettingsManager manager(storage, profiles);
        makeCurrentV21Source(manager.mutableSettings());
        JsonDocument incoming;
        incoming["_type"] = "v1simple_http_backup";
        JsonArray slots = incoming["wifiStaSlots"].to<JsonArray>();
        for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
            const String ssid = String("Network ") + String(static_cast<int>(index));
            const String password = String("password-") + String(static_cast<int>(index));
            TEST_ASSERT_TRUE(manager.setWifiStaSlotCredentials(index, ssid, password, "Saved", index));
            JsonObject slot = slots.add<JsonObject>();
            slot["index"] = index;
            slot["ssid"] = ssid;
            slot["priority"] = index == primary ? 0 : 1;
            // Leave non-primary slots in SD-only storage to exercise a mixed
            // full restore without a getter materializing them in advance.
            if (index != primary) {
                Preferences prefs;
                TEST_ASSERT_TRUE(prefs.begin(WIFI_CLIENT_NS, false));
                TEST_ASSERT_TRUE(prefs.remove(kNvsWifiStaSlotPassword[index]));
                prefs.end();
            }
        }
        incoming["wifiClientSSID"] = String("Network ") + String(static_cast<int>(primary));
        if (plain) incoming["stationPassword"] = "replacement";
        else incoming["wifiClientPasswordObf"] = encodeObfuscatedForStorage("replacement");
        TEST_ASSERT_TRUE(manager.resolveStorageTransactionsForMutation());
        TEST_ASSERT_EQUAL_INT(200, restoreHttpBackup(manager, incoming));
        for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
            const String expected = index == primary ? String("replacement")
                                                      : String("password-") + String(static_cast<int>(index));
            assertRestoredPassword(manager, index, expected.c_str());
        }
    }
}
