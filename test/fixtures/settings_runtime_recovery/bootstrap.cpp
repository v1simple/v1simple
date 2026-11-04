#define main original_deferred_persist_suite_main
#include "test/test_settings_deferred_persist/test_settings_deferred_persist.cpp"
#undef main
#include "test/mocks/runtime_presentation_source.h"

#include "src/persistent_bootstrap.cpp"

// Both actual runtime adapters execute below. Only their physical audio,
// display and GPS effects are substituted; recovery uses the real stores.
static std::vector<std::string> bootEffects;
static uint8_t paintedBrightness = 0;
static uint32_t startedGpsBaud = 0;
static bool catalogShouldBeReady = true;
static V1DeviceStore* bootDevices = nullptr;

void audio_init_buffers() {
    TEST_ASSERT_EQUAL(catalogShouldBeReady, profiles.isReady());
    TEST_ASSERT_NOT_NULL(bootDevices);
    TEST_ASSERT_TRUE(bootDevices->isReady());
    bootEffects.emplace_back("audio-buffers");
}
void audio_init_sd(StorageManager& owner) {
    TEST_ASSERT_TRUE(&owner == &storage);
    TEST_ASSERT_TRUE(owner.isReady());
    bootEffects.emplace_back("audio-sd");
}

struct BootstrapDisplay {
    void updateColorTheme(const DisplayStyleView&) { bootEffects.emplace_back("theme"); }
    void setBrightness(uint8_t value) {
        paintedBrightness = value;
        bootEffects.emplace_back("brightness");
    }
};
struct BootstrapGps {
    void begin(bool enabled, uint32_t baud) {
        TEST_ASSERT_EQUAL(settings.get().gpsEnabled, enabled);
        TEST_ASSERT_EQUAL_UINT32(settings.get().gpsBaud, baud);
        startedGpsBaud = baud;
        bootEffects.emplace_back("gps");
    }
};
struct BootstrapRuntimeState {
    StorageManager& storage_ = storage;
    SettingsManager& settings_ = settings;
    V1ProfileManager& profiles_ = profiles;
    V1DeviceStore devices_;
    BootstrapDisplay display_;
    BootstrapGps gps_;
};
class DriveRuntime : public BootstrapRuntimeState {
public:
    void initializeStorageAndProfiles();
};
class MaintenanceRuntime : public BootstrapRuntimeState {
public:
    void initializeStorageAndProfiles();
};

#include "persistent_bootstrap_adapters.inc"

static void resetBoot() {
    resetRuntimeState();
    fs::mock_reset_fs_write_budget();
    fs::mock_reset_fs_remove_state();
    fs::mock_reset_fs_rename_state();
    fs::mock_reset_fs_open_state();
    bootEffects.clear();
    paintedBrightness = 0;
    startedGpsBaud = 0;
    catalogShouldBeReady = true;
    bootDevices = nullptr;
    settings.begin();
    // These cases isolate bootstrap recovery, not legacy profile migration.
    // A legacy schema checkpoint legitimately persists a migration (and clears
    // its degraded fallback) before the fallback decision under test.
    settings.mutableSettings().autoPushProfileSchemaVersion = V1_PROFILE_SCHEMA_VERSION;
}

static void assertSavedBackupBrightness(fs::FS& filesystem, uint8_t brightness) {
    // save() already emits the SD backup. A second backupToSD() while startup
    // restore is provisional correctly refuses to overwrite that valid source.
    const std::string text = readFileToString(filesystem, SETTINGS_BACKUP_PATH);
    JsonDocument document;
    TEST_ASSERT_FALSE(text.empty());
    TEST_ASSERT_FALSE(deserializeJson(document, text));
    TEST_ASSERT_EQUAL_UINT8(brightness, document["brightness"].as<uint8_t>());
}

static void startBoot(bool maintenance, DriveRuntime& drive, MaintenanceRuntime& maint) {
    bootDevices = maintenance ? &maint.devices_ : &drive.devices_;
    if (maintenance) maint.initializeStorageAndProfiles();
    else drive.initializeStorageAndProfiles();
}

static void assertHardwareEffects(bool maintenance, bool mounted, bool restored) {
    std::vector<std::string> expected;
    if (mounted && !maintenance) {
        expected.emplace_back("audio-buffers");
        expected.emplace_back("audio-sd");
    }
    if (restored) {
        expected.emplace_back("theme");
        expected.emplace_back("brightness");
    }
    if (!maintenance) expected.emplace_back("gps");
    TEST_ASSERT_TRUE(bootEffects == expected);
}

void test_unavailable_storage_preserves_valid_fallback_and_mode_effects() {
    for (bool maintenance : {false, true}) {
        resetBoot();
        seedRuntimeFallback("aa-bb-cc-dd-ee-ff");
        DriveRuntime drive;
        MaintenanceRuntime maint;
        startBoot(maintenance, drive, maint);
        TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", settings.get().lastV1Address.c_str());
        TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace,
                                                         kNvsLastConnectedV1Address));
        TEST_ASSERT_FALSE(profiles.isReady());
        assertHardwareEffects(maintenance, false, false);
    }
}

void test_unavailable_storage_clears_invalid_fallback_only() {
    for (bool maintenance : {false, true}) {
        resetBoot();
        seedRuntimeFallback("invalid-address");
        settings.setLastV1Address("11:22:33:44:55:66");
        DriveRuntime drive;
        MaintenanceRuntime maint;
        startBoot(maintenance, drive, maint);
        TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", settings.get().lastV1Address.c_str());
        TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace,
                                                          kNvsLastConnectedV1Address));
        assertHardwareEffects(maintenance, false, false);
    }
}

void test_mounted_boot_uses_degraded_precedence_and_consumes_after_durable_save() {
    for (bool maintenance : {false, true}) {
        for (bool sd : {false, true}) {
            resetBoot();
            fs::FS filesystem(g_tempRoot / (std::to_string(maintenance) + std::to_string(sd)));
            settings.setLastV1Address("11:22:33:44:55:66");
            TEST_ASSERT_TRUE(settings.save());
            seedRuntimeFallback("AA:BB:CC:DD:EE:FF");
            storage.setFilesystem(&filesystem, sd);
            DriveRuntime drive;
            MaintenanceRuntime maint;
            startBoot(maintenance, drive, maint);
            TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", settings.get().lastV1Address.c_str());
            TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace,
                                                              kNvsLastConnectedV1Address));
            V1DeviceStore reloaded;
            TEST_ASSERT_TRUE(reloaded.begin(&filesystem));
            TEST_ASSERT_EQUAL_UINT(1, reloaded.listDevices().size());
            TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", reloaded.listDevices()[0].address.c_str());
            assertHardwareEffects(maintenance, true, false);
        }
    }
}

void test_failed_bootstrap_save_retains_original_fallback_for_retry() {
    for (bool maintenance : {false, true}) {
        resetBoot();
        fs::FS filesystem(g_tempRoot / std::to_string(maintenance));
        TEST_ASSERT_TRUE(settings.save());
        seedRuntimeFallback("AA:BB:CC:DD:EE:FF");
        storage.setFilesystem(&filesystem, false);
        fs::mock_set_fs_write_budget(0);
        DriveRuntime drive;
        MaintenanceRuntime maint;
        startBoot(maintenance, drive, maint);
        fs::mock_reset_fs_write_budget();
        TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", settings.loadLastV1AddressFallback().c_str());
        assertHardwareEffects(maintenance, true, false);
    }
}

void test_invalid_delete_intent_blocks_fallback_resurrection() {
    for (bool maintenance : {false, true}) {
        resetBoot();
        fs::FS filesystem(g_tempRoot / std::to_string(maintenance));
        TEST_ASSERT_TRUE(settings.save());
        seedRuntimeFallback("AA:BB:CC:DD:EE:FF");
        Preferences prefs;
        TEST_ASSERT_TRUE(prefs.begin(kSettingsV1RuntimeNamespace, false));
        TEST_ASSERT_EQUAL_UINT(1, prefs.putUChar(kNvsV1DeleteReady, 2));
        prefs.end();
        storage.setFilesystem(&filesystem, false);
        DriveRuntime drive;
        MaintenanceRuntime maint;
        startBoot(maintenance, drive, maint);
        auto& devices = maintenance ? maint.devices_ : drive.devices_;
        TEST_ASSERT_TRUE(devices.listDevices().empty());
        TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", settings.loadLastV1AddressFallback().c_str());
        TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace, kNvsV1DeleteReady));
        assertHardwareEffects(maintenance, true, false);
    }
}

void test_unreadable_fallback_is_not_cleared() {
    for (bool maintenance : {false, true}) {
        resetBoot();
        seedRuntimeFallback("AA:BB:CC:DD:EE:FF");
        mock_preferences::set_fail_string_read_for_key(kNvsLastConnectedV1Address);
        DriveRuntime drive;
        MaintenanceRuntime maint;
        startBoot(maintenance, drive, maint);
        mock_preferences::set_fail_string_read_for_key(nullptr);
        TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", settings.loadLastV1AddressFallback().c_str());
        assertHardwareEffects(maintenance, false, false);
    }
}

void test_post_mount_restore_applies_display_before_drive_gps() {
    for (bool maintenance : {false, true}) {
        resetBoot();
        fs::FS filesystem(g_tempRoot / std::to_string(maintenance));
        storage.setFilesystem(&filesystem, true);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        auto& saved = settings.mutableSettings();
        saved.autoPushProfileSchemaVersion = 4;
        saved.brightness = 87;
        saved.gpsEnabled = true;
        saved.gpsBaud = 38400;
        TEST_ASSERT_TRUE(settings.save());
        assertSavedBackupBrightness(filesystem, 87);

        // A reboot loads provisional defaults before the card is mounted.
        // The actual boot adapter must adopt the card before starting GPS.
        resetBoot();
        TEST_ASSERT_EQUAL_UINT8(200, settings.get().brightness);
        storage.setFilesystem(&filesystem, true);
        DriveRuntime drive;
        MaintenanceRuntime maint;
        startBoot(maintenance, drive, maint);
        TEST_ASSERT_EQUAL_UINT8(87, settings.get().brightness);
        TEST_ASSERT_EQUAL_UINT8(87, paintedBrightness);
        TEST_ASSERT_TRUE(settings.get().gpsEnabled);
        TEST_ASSERT_EQUAL_UINT32(38400, settings.get().gpsBaud);
        if (!maintenance) TEST_ASSERT_EQUAL_UINT32(38400, startedGpsBaud);
        assertHardwareEffects(maintenance, true, true);
    }
}

void test_missing_restore_source_does_not_claim_restored_display() {
    for (bool maintenance : {false, true}) {
        resetBoot();
        fs::FS filesystem(g_tempRoot / std::to_string(maintenance));
        storage.setFilesystem(&filesystem, true);
        DriveRuntime drive;
        MaintenanceRuntime maint;
        startBoot(maintenance, drive, maint);
        TEST_ASSERT_EQUAL_UINT8(200, settings.get().brightness);
        TEST_ASSERT_EQUAL_UINT8(0, paintedBrightness);
        assertHardwareEffects(maintenance, true, false);
    }
}

void test_populated_catalog_cleans_only_missing_reference() {
    for (bool maintenance : {false, true}) {
        resetBoot();
        fs::FS filesystem(g_tempRoot / std::to_string(maintenance));
        storage.setFilesystem(&filesystem, false);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        const V1Profile retained("Retained");
        TEST_ASSERT_TRUE(profiles.saveProfile(retained).success);
        settings.mutableSettings().autoPushProfileSchemaVersion = 4;
        settings.mutableSettings().slot0_default.profileName = "Retained";
        settings.mutableSettings().slot1_highway.profileName = "Missing";
        TEST_ASSERT_TRUE(settings.save());
        DriveRuntime drive;
        MaintenanceRuntime maint;
        startBoot(maintenance, drive, maint);
        TEST_ASSERT_EQUAL_STRING("Retained", settings.get().slot0_default.profileName.c_str());
        TEST_ASSERT_EQUAL_STRING("", settings.get().slot1_highway.profileName.c_str());
        V1Profile reloaded;
        TEST_ASSERT_TRUE(profiles.loadProfileResult("Retained", reloaded).success());
        assertHardwareEffects(maintenance, true, false);
    }
}

void test_mounted_missing_invalid_and_unreadable_fallback_keep_settings_precedence() {
    for (bool maintenance : {false, true}) {
        for (int fallbackState = 0; fallbackState < 3; ++fallbackState) {
            resetBoot();
            fs::FS filesystem(g_tempRoot / (std::to_string(maintenance) + std::to_string(fallbackState)));
            settings.setLastV1Address("11:22:33:44:55:66");
            TEST_ASSERT_TRUE(settings.save());
            if (fallbackState == 1) seedRuntimeFallback("invalid-address");
            if (fallbackState == 2) {
                seedRuntimeFallback("AA:BB:CC:DD:EE:FF");
                mock_preferences::set_fail_string_read_for_key(kNvsLastConnectedV1Address);
            }
            storage.setFilesystem(&filesystem, false);
            DriveRuntime drive;
            MaintenanceRuntime maint;
            startBoot(maintenance, drive, maint);
            mock_preferences::set_fail_string_read_for_key(nullptr);
            TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", settings.get().lastV1Address.c_str());
            TEST_ASSERT_EQUAL(fallbackState == 2,
                mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace, kNvsLastConnectedV1Address));
            if (fallbackState == 2) {
                TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", settings.loadLastV1AddressFallback().c_str());
            }
            V1DeviceStore reloaded;
            TEST_ASSERT_TRUE(reloaded.begin(&filesystem));
            TEST_ASSERT_EQUAL_UINT(1, reloaded.listDevices().size());
            TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", reloaded.listDevices()[0].address.c_str());
            assertHardwareEffects(maintenance, true, false);
        }
    }
}

void test_existing_catalog_admits_only_new_degraded_fallback() {
    for (bool maintenance : {false, true}) {
        for (bool degraded : {false, true}) {
            resetBoot();
            fs::FS filesystem(g_tempRoot / (std::to_string(maintenance) + std::to_string(degraded)));
            V1DeviceStore existing;
            TEST_ASSERT_TRUE(existing.begin(&filesystem));
            TEST_ASSERT_TRUE(existing.upsertDevice("11:22:33:44:55:66"));
            settings.setLastV1Address("AA:BB:CC:DD:EE:FF");
            TEST_ASSERT_TRUE(settings.save());
            if (degraded) seedRuntimeFallback("AA:BB:CC:DD:EE:FF");
            storage.setFilesystem(&filesystem, false);
            DriveRuntime drive;
            MaintenanceRuntime maint;
            startBoot(maintenance, drive, maint);
            V1DeviceStore reloaded;
            TEST_ASSERT_TRUE(reloaded.begin(&filesystem));
            bool present = false;
            TEST_ASSERT_TRUE(reloaded.containsDeviceChecked("AA:BB:CC:DD:EE:FF", present));
            TEST_ASSERT_EQUAL(degraded, present);
            TEST_ASSERT_EQUAL_UINT(degraded ? 2 : 1, reloaded.listDevices().size());
            TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace,
                                                              kNvsLastConnectedV1Address));
            assertHardwareEffects(maintenance, true, false);
        }
    }
}

void test_unreadable_catalog_retains_fallback_and_catalog_bytes() {
    for (bool maintenance : {false, true}) {
        resetBoot();
        fs::FS filesystem(g_tempRoot / std::to_string(maintenance));
        V1DeviceStore existing;
        TEST_ASSERT_TRUE(existing.begin(&filesystem));
        TEST_ASSERT_TRUE(existing.upsertDevice("11:22:33:44:55:66"));
        const std::string before = readFileToString(filesystem, "/v1devices.json");
        TEST_ASSERT_TRUE(settings.save());
        seedRuntimeFallback("AA:BB:CC:DD:EE:FF");
        storage.setFilesystem(&filesystem, false);
        const std::string path = filesystem.mockResolve("/v1devices.json").string();
        fs::mock_fail_read_opens_for_full_path(path.c_str());
        DriveRuntime drive;
        MaintenanceRuntime maint;
        startBoot(maintenance, drive, maint);
        TEST_ASSERT_FALSE(bootDevices->catalogReadable());
        fs::mock_reset_fs_open_state();
        TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", settings.loadLastV1AddressFallback().c_str());
        TEST_ASSERT_EQUAL_STRING(before.c_str(), readFileToString(filesystem, "/v1devices.json").c_str());
        assertHardwareEffects(maintenance, true, false);
    }
}

void test_pending_delete_retries_before_fallback_bootstrap_without_resurrection() {
    for (bool maintenance : {false, true}) {
        resetBoot();
        fs::FS filesystem(g_tempRoot / std::to_string(maintenance));
        V1DeviceStore existing;
        TEST_ASSERT_TRUE(existing.begin(&filesystem));
        TEST_ASSERT_TRUE(existing.upsertDevice("AA:BB:CC:DD:EE:FF"));
        TEST_ASSERT_TRUE(existing.upsertDevice("11:22:33:44:55:66"));
        settings.setLastV1Address("AA:BB:CC:DD:EE:FF");
        TEST_ASSERT_TRUE(settings.save());
        seedRuntimeFallback("AA:BB:CC:DD:EE:FF");
        fs::mock_set_fs_write_budget(20);
        TEST_ASSERT_EQUAL_INT(V1DeviceMutationStatus::DurablePending,
            settings.deleteV1DeviceTransactional("AA:BB:CC:DD:EE:FF", existing).status);
        fs::mock_reset_fs_write_budget();
        TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace, kNvsV1DeleteReady));
        storage.setFilesystem(&filesystem, false);

        // A still-failing primary write must retain the acknowledged deletion
        // and cannot allow the ordinary settings fallback to recreate its row.
        fs::mock_set_fs_write_budget(0);
        DriveRuntime drive;
        MaintenanceRuntime maint;
        startBoot(maintenance, drive, maint);
        fs::mock_reset_fs_write_budget();
        TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace, kNvsV1DeleteReady));
        TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace,
                                                          kNvsLastConnectedV1Address));
        assertHardwareEffects(maintenance, true, false);

        // Fresh runtime stores retry the same durable intent, then bootstrap
        // sees the authoritative remaining catalog, not an empty replacement.
        bootEffects.clear();
        DriveRuntime retryDrive;
        MaintenanceRuntime retryMaint;
        startBoot(maintenance, retryDrive, retryMaint);
        TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(kSettingsV1RuntimeNamespace, kNvsV1DeleteReady));
        V1DeviceStore reloaded;
        TEST_ASSERT_TRUE(reloaded.begin(&filesystem));
        TEST_ASSERT_EQUAL_UINT(1, reloaded.listDevices().size());
        TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", reloaded.listDevices()[0].address.c_str());
        assertHardwareEffects(maintenance, true, false);
    }
}

void test_pending_delete_unreadable_authority_stays_intact_at_boot() {
    for (bool maintenance : {false, true}) {
        for (bool presenceFault : {false, true}) {
            resetBoot();
            fs::FS filesystem(g_tempRoot / (std::to_string(maintenance) + std::to_string(presenceFault)));
            V1DeviceStore existing;
            TEST_ASSERT_TRUE(existing.begin(&filesystem));
            TEST_ASSERT_TRUE(existing.upsertDevice("AA:BB:CC:DD:EE:FF"));
            TEST_ASSERT_TRUE(settings.save());
            seedRuntimeFallback("AA:BB:CC:DD:EE:FF");
            fs::mock_set_fs_write_budget(20);
            TEST_ASSERT_EQUAL_INT(V1DeviceMutationStatus::DurablePending,
                settings.deleteV1DeviceTransactional("AA:BB:CC:DD:EE:FF", existing).status);
            fs::mock_reset_fs_write_budget();
            const auto intentBefore = mock_preferences::store().at(kSettingsV1RuntimeNamespace);
            const std::string catalogBefore = readFileToString(filesystem, "/v1devices.json");
            if (presenceFault) {
                mock_nvs::set_read_result(kSettingsV1RuntimeNamespace, kNvsV1DeleteReady, ESP_FAIL,
                                         mock_nvs::ReadStage::Presence);
            } else {
                mock_preferences::set_scalar_read_fault(kSettingsV1RuntimeNamespace,
                    kNvsV1DeleteReady, PT_U8, ESP_FAIL);
            }
            storage.setFilesystem(&filesystem, false);
            DriveRuntime drive;
            MaintenanceRuntime maint;
            startBoot(maintenance, drive, maint);
            const auto& intentAfter = mock_preferences::store().at(kSettingsV1RuntimeNamespace);
            TEST_ASSERT_EQUAL_UINT(intentBefore.size(), intentAfter.size());
            for (const auto& entry : intentBefore) {
                TEST_ASSERT_TRUE(intentAfter.count(entry.first) == 1);
                TEST_ASSERT_EQUAL_INT(entry.second.type, intentAfter.at(entry.first).type);
                TEST_ASSERT_TRUE(entry.second.value == intentAfter.at(entry.first).value);
            }
            TEST_ASSERT_EQUAL_STRING(catalogBefore.c_str(), readFileToString(filesystem, "/v1devices.json").c_str());
            assertHardwareEffects(maintenance, true, false);
        }
    }
}

void test_empty_or_unavailable_profile_catalog_preserves_references() {
    for (bool maintenance : {false, true}) {
        for (bool unavailable : {false, true}) {
            resetBoot();
            fs::FS filesystem(g_tempRoot / (std::to_string(maintenance) + std::to_string(unavailable)));
            storage.setFilesystem(&filesystem, false);
            TEST_ASSERT_TRUE(profiles.begin(storage));
            settings.mutableSettings().autoPushProfileSchemaVersion = 4;
            settings.mutableSettings().slot0_default.profileName = "Retain For Recovery";
            TEST_ASSERT_TRUE(settings.save());
            if (unavailable) {
                const std::string directory = filesystem.mockResolve("/v1profiles").string();
                fs::mock_fail_read_opens_for_full_path(directory.c_str());
                catalogShouldBeReady = false;
            }
            DriveRuntime drive;
            MaintenanceRuntime maint;
            startBoot(maintenance, drive, maint);
            fs::mock_reset_fs_open_state();
            TEST_ASSERT_EQUAL_STRING("Retain For Recovery", settings.get().slot0_default.profileName.c_str());
            SettingsManager reloaded(storage, profiles);
            reloaded.load();
            TEST_ASSERT_EQUAL_STRING("Retain For Recovery", reloaded.get().slot0_default.profileName.c_str());
            assertHardwareEffects(maintenance, true, false);
        }
    }
}

void test_unreadable_sd_backup_preserves_settings_and_reference_recovery() {
    for (bool maintenance : {false, true}) {
        resetBoot();
        fs::FS filesystem(g_tempRoot / std::to_string(maintenance));
        storage.setFilesystem(&filesystem, true);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        TEST_ASSERT_TRUE(profiles.saveProfile(V1Profile("Present")).success);
        settings.mutableSettings().autoPushProfileSchemaVersion = 4;
        settings.mutableSettings().brightness = 63;
        TEST_ASSERT_TRUE(settings.save());
        assertSavedBackupBrightness(filesystem, 63);
        const std::string before = readFileToString(filesystem, SETTINGS_BACKUP_PATH);
        settings.mutableSettings().slot0_default.profileName = "Recovery Needed";
        TEST_ASSERT_TRUE(settings.saveDeferredBackup());
        const std::string backupPath = filesystem.mockResolve(SETTINGS_BACKUP_PATH).string();
        fs::mock_fail_read_opens_for_full_path(backupPath.c_str());
        DriveRuntime drive;
        MaintenanceRuntime maint;
        startBoot(maintenance, drive, maint);
        fs::mock_reset_fs_open_state();
        TEST_ASSERT_EQUAL_UINT8(63, settings.get().brightness);
        TEST_ASSERT_EQUAL_UINT8(0, paintedBrightness);
        TEST_ASSERT_EQUAL_STRING("Recovery Needed", settings.get().slot0_default.profileName.c_str());
        TEST_ASSERT_EQUAL_STRING(before.c_str(), readFileToString(filesystem, SETTINGS_BACKUP_PATH).c_str());
        assertHardwareEffects(maintenance, true, false);
    }
}

void test_failed_sd_restore_commit_has_no_success_display_effect() {
    for (bool maintenance : {false, true}) {
        resetBoot();
        fs::FS filesystem(g_tempRoot / std::to_string(maintenance));
        storage.setFilesystem(&filesystem, true);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        settings.mutableSettings().autoPushProfileSchemaVersion = 4;
        settings.mutableSettings().brightness = 87;
        settings.mutableSettings().gpsEnabled = true;
        settings.mutableSettings().gpsBaud = 38400;
        TEST_ASSERT_TRUE(settings.save());
        assertSavedBackupBrightness(filesystem, 87);
        const std::string before = readFileToString(filesystem, SETTINGS_BACKUP_PATH);
        resetBoot();
        storage.setFilesystem(&filesystem, true);
        mock_preferences::set_fail_writes_for_key(kNvsBrightness);
        DriveRuntime drive;
        MaintenanceRuntime maint;
        startBoot(maintenance, drive, maint);
        mock_preferences::set_fail_writes_for_key(nullptr);
        TEST_ASSERT_EQUAL_UINT8(200, settings.get().brightness);
        TEST_ASSERT_EQUAL_UINT8(0, paintedBrightness);
        TEST_ASSERT_EQUAL_STRING(before.c_str(), readFileToString(filesystem, SETTINGS_BACKUP_PATH).c_str());
        assertHardwareEffects(maintenance, true, false);
    }
}

void test_pending_restore_journal_preserves_reference_until_actual_boot_retry() {
    for (bool maintenance : {false, true}) {
        resetBoot();
        fs::FS filesystem(g_tempRoot / std::to_string(maintenance));
        storage.setFilesystem(&filesystem, false);
        TEST_ASSERT_TRUE(profiles.begin(storage));
        settings.mutableSettings().autoPushProfileSchemaVersion = 4;
        JsonDocument emptyBackup;
        const auto built = BackupPayloadBuilder::buildBackupDocument(emptyBackup, settings.get(), profiles,
            BackupPayloadBuilder::BackupTransport::HttpDownload, 1000);
        TEST_ASSERT_TRUE(built.safeToCommit);
        TEST_ASSERT_TRUE(profiles.saveProfile(V1Profile("Retained")).success);
        settings.mutableSettings().slot0_default.profileName = "Retained";
        TEST_ASSERT_TRUE(settings.saveDeferredBackup());
        settings.utInterruptRestoreAfterProfiles(true);
        TEST_ASSERT_FALSE(settings.applyBackupDocument(emptyBackup, true).success);
        const std::string journalBefore = readFileToString(filesystem, RESTORE_TRANSACTION_PATH);
        TEST_ASSERT_FALSE(journalBefore.empty());
        const std::string path = filesystem.mockResolve(RESTORE_TRANSACTION_PATH).string();
        fs::mock_fail_read_opens_for_full_path(path.c_str());

        // Keep durable NVS/files but recreate the pre-mount settings/profile
        // owners, as a reboot does before either runtime starts storage.
        storage.reset();
        profiles = V1ProfileManager();
        settings = SettingsManager(storage, profiles);
        settings.begin();
        storage.setFilesystem(&filesystem, false);
        DriveRuntime drive;
        MaintenanceRuntime maint;
        startBoot(maintenance, drive, maint);
        TEST_ASSERT_TRUE(settings.recoveryPending());
        TEST_ASSERT_EQUAL_STRING("Retained", settings.get().slot0_default.profileName.c_str());
        fs::mock_reset_fs_open_state();
        TEST_ASSERT_EQUAL_STRING(journalBefore.c_str(), readFileToString(filesystem, RESTORE_TRANSACTION_PATH).c_str());
        assertHardwareEffects(maintenance, true, false);

        storage.reset();
        profiles = V1ProfileManager();
        settings = SettingsManager(storage, profiles);
        settings.begin();
        storage.setFilesystem(&filesystem, false);
        bootEffects.clear();
        DriveRuntime retryDrive;
        MaintenanceRuntime retryMaint;
        startBoot(maintenance, retryDrive, retryMaint);
        TEST_ASSERT_FALSE(settings.recoveryPending());
        TEST_ASSERT_EQUAL_STRING("Retained", settings.get().slot0_default.profileName.c_str());
        V1Profile retained;
        TEST_ASSERT_TRUE(profiles.loadProfile("Retained", retained));
        TEST_ASSERT_FALSE(filesystem.exists(RESTORE_TRANSACTION_PATH));
        assertHardwareEffects(maintenance, true, false);
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_unavailable_storage_preserves_valid_fallback_and_mode_effects);
    RUN_TEST(test_unavailable_storage_clears_invalid_fallback_only);
    RUN_TEST(test_mounted_boot_uses_degraded_precedence_and_consumes_after_durable_save);
    RUN_TEST(test_failed_bootstrap_save_retains_original_fallback_for_retry);
    RUN_TEST(test_invalid_delete_intent_blocks_fallback_resurrection);
    RUN_TEST(test_unreadable_fallback_is_not_cleared);
    RUN_TEST(test_post_mount_restore_applies_display_before_drive_gps);
    RUN_TEST(test_missing_restore_source_does_not_claim_restored_display);
    RUN_TEST(test_populated_catalog_cleans_only_missing_reference);
    RUN_TEST(test_mounted_missing_invalid_and_unreadable_fallback_keep_settings_precedence);
    RUN_TEST(test_existing_catalog_admits_only_new_degraded_fallback);
    RUN_TEST(test_unreadable_catalog_retains_fallback_and_catalog_bytes);
    RUN_TEST(test_pending_delete_retries_before_fallback_bootstrap_without_resurrection);
    RUN_TEST(test_pending_delete_unreadable_authority_stays_intact_at_boot);
    RUN_TEST(test_empty_or_unavailable_profile_catalog_preserves_references);
    RUN_TEST(test_unreadable_sd_backup_preserves_settings_and_reference_recovery);
    RUN_TEST(test_failed_sd_restore_commit_has_no_success_display_effect);
    RUN_TEST(test_pending_restore_journal_preserves_reference_until_actual_boot_retry);
    return UNITY_END();
}
