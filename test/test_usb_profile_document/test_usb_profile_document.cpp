#include <unity.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <limits>
#include <string>
#include <vector>
#include <ArduinoJson.h>

#include "../mocks/Arduino.h"
#include "../mocks/Preferences.h"
#include "../mocks/nvs.h"
#include "../mocks/storage_manager.h"

namespace ArduinoJson {
inline void convertFromJson(JsonVariantConst src, ::String& dst) { dst = ::String(src.as<const char*>()); }
inline bool canConvertFromJson(JsonVariantConst src, const ::String&) { return src.is<const char*>(); }
}

#include "../../src/usb_profile_document.h"
#include "../../src/usb_profile_json_document.h"
#include "../../src/v1_profiles.cpp"
#include "../../src/backup_payload_builder.cpp"
#include "../../src/psram_freertos_alloc.cpp"
#include "../../src/settings.cpp"
#include "../../src/settings_setters.cpp"
#include "../../src/settings_nvs.cpp"
#include "../../src/settings_backup.cpp"
#include "../../src/settings_backup_doc.cpp"
#include "../../src/settings_restore.cpp"
#include "../../src/usb_profile_document.cpp"
#include "../../src/modules/wifi/wifi_v1_profile_api_service.cpp"

SerialClass Serial;
unsigned long mockMillis = 1000;
unsigned long mockMicros = 1000000;

namespace {
std::filesystem::path root;
std::unique_ptr<fs::FS> primaryFs;
std::unique_ptr<fs::FS> secondaryFs;
std::unique_ptr<V1ProfileManager> profileManager;
std::unique_ptr<SettingsManager> manager;
int fixtureNumber = 0;

String jsonText(const JsonDocument& doc) {
    String text;
    serializeJson(doc, text);
    return text;
}

std::map<std::string, std::string> filesSnapshot() {
    std::map<std::string, std::string> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream file(entry.path(), std::ios::binary);
        result[entry.path().lexically_relative(root).string()] =
            std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    return result;
}

bool preferencesEqual(const mock_preferences::Store& before) {
    const auto& after = mock_preferences::store();
    if (before.size() != after.size()) return false;
    for (const auto& ns : before) {
        const auto found = after.find(ns.first);
        if (found == after.end() || found->second.size() != ns.second.size()) return false;
        for (const auto& value : ns.second) {
            const auto current = found->second.find(value.first);
            if (current == found->second.end() || current->second.type != value.second.type ||
                current->second.value != value.second.value) return false;
        }
    }
    return true;
}

String snapshot() {
    JsonDocument doc;
    String error;
    TEST_ASSERT_TRUE_MESSAGE(buildUsbProfileDocument(doc, *manager, *profileManager, error), error.c_str());
    return jsonText(doc);
}

String unrelatedSettingsSnapshot() {
    V1Settings snapshotSettings;
    TEST_ASSERT_TRUE(copySettingsChecked(manager->get(), snapshotSettings));
    const bool legacyProxyObdConflict = snapshotSettings.proxyBLE && snapshotSettings.obdEnabled;
    // Profiles-only restore deliberately leaves unrelated legacy conflicts in
    // place.  Current v21 backups cannot truthfully encode that state, so make
    // only the test serialization copy canonical and record the conflict
    // explicitly for the before/after comparison.
    if (legacyProxyObdConflict) snapshotSettings.proxyBLE = false;
    JsonDocument doc;
    const auto result = BackupPayloadBuilder::buildBackupDocument(
        doc, snapshotSettings, *profileManager, BackupPayloadBuilder::BackupTransport::SdBackup, 1000);
    TEST_ASSERT_TRUE(result.safeToCommit);
    doc["legacyProxyObdConflict"] = legacyProxyObdConflict;
    doc.remove("profiles");
    doc.remove("activeSlot");
    doc.remove("autoPushEnabled");
    doc.remove("_crc32");
    std::vector<String> keys;
    for (JsonPair pair : doc.as<JsonObject>()) {
        if (String(pair.key().c_str()).startsWith("slot")) keys.push_back(pair.key().c_str());
    }
    for (const auto& key : keys) doc.remove(key);
    return jsonText(doc);
}

void seed() {
    V1Profile profile("Original");
    profile.description = "Preserve metadata";
    profile.displayOn = false;
    profile.mainVolume = 0;
    profile.mutedVolume = 255;
    const uint8_t raw[] = {0, 1, 127, 128, 254, 42};
    memcpy(profile.settings.bytes, raw, sizeof(raw));
    TEST_ASSERT_TRUE(profileManager->saveProfile(profile).success);
    V1Profile spare("Spare");
    spare.description = "Unassigned profile must also round trip";
    spare.mainVolume = 9;
    spare.mutedVolume = 0;
    spare.settings.bytes[5] = 91;
    TEST_ASSERT_TRUE(profileManager->saveProfile(spare).success);
    auto& state = manager->mutableSettings();
    state.autoPushEnabled = false;
    state.activeSlot = 2;
    for (int index = 0; index < 3; ++index) {
        auto slot = state.autoPushSlotView(index);
        slot.config.profileName = "Original";
        slot.config.mode = static_cast<V1Mode>(index + 1);
        slot.color = static_cast<uint16_t>(0x1111 * (index + 1));
        slot.alertPersist = static_cast<uint8_t>(index + 1);
        slot.priorityArrow = index != 1;
        slot.darkMode = index == 1;
        slot.muteToZero = index == 2;
        slot.volume = index == 0 ? 255 : index + 3;
        slot.muteVolume = index == 0 ? 255 : index;
    }
    TEST_ASSERT_TRUE(manager->setWifiStaSlotCredentials(0, "TestNetwork", "fixture-password", "LAB", 0));
    state.wifiClientEnabled = false;
    state.brightness = 73;
    state.stealthEnabled = true;
    state.voiceVolume = 39;
    state.alpAlertPersistSec = 4;
    TEST_ASSERT_TRUE(manager->saveDeferredBackup());
    TEST_ASSERT_TRUE(manager->migrateAutoPushProfilesToV2());
    TEST_ASSERT_TRUE(BackupPayloadBuilder::settingsTextIsSerializable(manager->get()));
    TEST_ASSERT_TRUE(BackupPayloadBuilder::settingsCurrentBackupStateIsCanonical(manager->get()));
}

void replacement(JsonDocument& doc) {
    String error;
    TEST_ASSERT_TRUE(buildUsbProfileDocument(doc, *manager, *profileManager, error));
    doc["profiles"][0]["name"] = "A Replacement";
    doc["profiles"][0]["rawBytes"][5] = 219;
    doc["profiles"][0]["description"] = "New metadata";
    doc["profiles"][0]["detector"]["display"] = "on";
    doc["autoPushEnabled"] = true;
    doc["activeSlot"] = 0;
    for (JsonObject slot : doc["slots"].as<JsonArray>()) slot["profile"] = "A Replacement";
    doc["slots"][0]["alertPersist"] = 5;
}

void reboot() {
    manager.reset();
    profileManager = std::make_unique<V1ProfileManager>();
    TEST_ASSERT_TRUE(profileManager->begin(storage));
    manager = std::make_unique<SettingsManager>(storage, *profileManager);
    manager->load();
    manager->checkAndRestoreFromSD();
}

void writeLegacyProfileFile(const char* name, const char* description, const uint8_t bytes[6]) {
    JsonDocument legacy;
    legacy["name"] = name;
    legacy["description"] = description;
    legacy["displayOn"] = false;
    legacy["mainVolume"] = 7;
    legacy["mutedVolume"] = 2;
    JsonArray raw = legacy["bytes"].to<JsonArray>();
    for (int i = 0; i < 6; ++i) raw.add(bytes[i]);
    const String path = String("/v1profiles/") + name + ".json";
    File file = primaryFs->open(path, FILE_WRITE);
    TEST_ASSERT_TRUE(file);
    TEST_ASSERT_EQUAL_UINT(measureJson(legacy), serializeJson(legacy, file));
    file.close();
}

void writeV2ProfileFile(const char* name = "V2 Road") {
    V1DetectorConfiguration detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Value;
    detector.modePolicy = V1ModePolicy::Value;
    detector.mode = 2;
    detector.displayPolicy = V1DisplayPolicy::Off;
    detector.volumePolicy = V1VolumePolicy::Temporary;
    detector.mainVolume = 7;
    detector.mutedVolume = 2;
    const uint8_t bytes[] = {0xBF, 0xE1, 0xF2, 0x73, 0xA5, 0x5A};
    JsonDocument v2;
    v2["schemaVersion"] = V1_PROFILE_PREVIOUS_SCHEMA_VERSION;
    v2["name"] = name;
    v2["description"] = "validated schema two";
    appendV1DetectorConfigurationV2(v2["detector"].to<JsonObject>(), detector);
    uint32_t detectorCrc = 0;
    TEST_ASSERT_TRUE(detectorConfigurationCrc(detector, detectorCrc, V1_PROFILE_PREVIOUS_SCHEMA_VERSION));
    v2["detectorCrc32"] = detectorCrc;
    JsonArray raw = v2["bytes"].to<JsonArray>();
    for (uint8_t byte : bytes) raw.add(byte);
    v2["crc32"] = computeCrc32(bytes, sizeof(bytes));
    const String path = String("/v1profiles/") + name + ".json";
    File file = primaryFs->open(path, FILE_WRITE);
    TEST_ASSERT_TRUE(file);
    TEST_ASSERT_EQUAL_UINT(measureJson(v2), serializeJson(v2, file));
    file.close();

    auto& state = manager->mutableSettings();
    state.autoPushEnabled = true;
    state.autoPushProfileSchemaVersion = V1_PROFILE_PREVIOUS_SCHEMA_VERSION;
    state.slot0_default.profileName = name;
    state.slot0_default.mode = V1_MODE_UNKNOWN;
    state.slot0Volume = 0xFF;
    state.slot0MuteVolume = 0xFF;
    state.slot0DarkMode = false;
    state.slot0MuteToZero = false;
    TEST_ASSERT_TRUE(manager->saveDeferredBackup());
}

uint8_t persistedProfileSchema(const char* name = "V2 Road") {
    const String path = String("/v1profiles/") + name + ".json";
    File file = primaryFs->open(path, FILE_READ);
    TEST_ASSERT_TRUE(file);
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, file));
    file.close();
    return doc["schemaVersion"].as<uint8_t>();
}

void assertMigratedApplication(const String& profileName, const uint8_t expectedBytes[6],
                               V1ModePolicy modePolicy, uint8_t mode,
                               V1DisplayPolicy displayPolicy, V1VolumePolicy volumePolicy,
                               uint8_t mainVolume = 0, uint8_t mutedVolume = 0) {
    V1Profile migrated;
    TEST_ASSERT_TRUE(profileManager->loadProfile(profileName, migrated));
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_SCHEMA_VERSION, migrated.schemaVersion);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expectedBytes, migrated.settings.bytes, 6);
    TEST_ASSERT_EQUAL_INT(V1UserSettingsPolicy::Value, migrated.detector.userSettingsPolicy);
    TEST_ASSERT_EQUAL_INT(modePolicy, migrated.detector.modePolicy);
    TEST_ASSERT_EQUAL_UINT8(mode, migrated.detector.mode);
    TEST_ASSERT_EQUAL_INT(displayPolicy, migrated.detector.displayPolicy);
    TEST_ASSERT_EQUAL_INT(volumePolicy, migrated.detector.volumePolicy);
    if (volumePolicy != V1VolumePolicy::Unchanged) {
        TEST_ASSERT_EQUAL_UINT8(mainVolume, migrated.detector.mainVolume);
        TEST_ASSERT_EQUAL_UINT8(mutedVolume, migrated.detector.mutedVolume);
    }
}
}

void setUp() {
    g_failUsbInputStringCopyEnabledForTest = false;
    g_failPreparedUsbProfileAllocationForTest = false;
    mock_preferences::reset();
    mock_nvs::reset();
    storage.reset();
    StorageManager::resetMockSdLockState();
    resetDeferredSettingsBackupStateForTest();
    fs::mock_reset_fs_write_budget();
    fs::mock_reset_fs_rename_state();
    fs::mock_reset_fs_open_state();
    fs::mock_reset_fs_remove_state();
    root = std::filesystem::temp_directory_path() / ("usb_profile_document_" + std::to_string(++fixtureNumber));
    std::filesystem::remove_all(root);
    primaryFs = std::make_unique<fs::FS>(root / "sd");
    secondaryFs = std::make_unique<fs::FS>(root / "little");
    storage.setFilesystem(primaryFs.get(), true);
    storage.setLittleFS(secondaryFs.get());
    profileManager = std::make_unique<V1ProfileManager>();
    TEST_ASSERT_TRUE(profileManager->begin(storage));
    manager = std::make_unique<SettingsManager>(storage, *profileManager);
}

void tearDown() {
    g_failUsbInputStringCopyEnabledForTest = false;
    g_failPreparedUsbProfileAllocationForTest = false;
    manager.reset();
    profileManager.reset();
    primaryFs.reset();
    secondaryFs.reset();
    std::filesystem::remove_all(root);
}

void test_usb_import_string_copy_failure_never_reaches_restore_or_mutates_state() {
    seed();
    JsonDocument incoming;
    replacement(incoming);
    const String before = snapshot();
    const auto filesBefore = filesSnapshot();
    const auto prefsBefore = mock_preferences::store();

    const UsbInputStringField failures[] = {
        UsbInputStringField::ProfileName,
        UsbInputStringField::SlotProfile,
        UsbInputStringField::SlotName,
    };
    for (UsbInputStringField failure : failures) {
        g_failUsbInputStringCopyForTest = failure;
        g_failUsbInputStringCopyEnabledForTest = true;
        String error;
        const SettingsBackupApplyResult result =
            applyUsbProfileDocument(*manager, *profileManager, incoming, error);
        g_failUsbInputStringCopyEnabledForTest = false;

        TEST_ASSERT_FALSE(result.success);
        TEST_ASSERT_TRUE(error.indexOf("allocate exact profile import string") >= 0);
        TEST_ASSERT_TRUE(filesBefore == filesSnapshot());
        TEST_ASSERT_TRUE(preferencesEqual(prefsBefore));
        TEST_ASSERT_EQUAL_STRING(before.c_str(), snapshot().c_str());
    }
}

void test_bundle_round_trip_replaces_catalog_and_preserves_unrelated_state() {
    seed();
    JsonDocument original;
    String error;
    TEST_ASSERT_TRUE(buildUsbProfileDocument(original, *manager, *profileManager, error));
    TEST_ASSERT_EQUAL_UINT(4, original["profiles"].size());
    TEST_ASSERT_EQUAL_UINT(3, original["slots"].size());
    TEST_ASSERT_EQUAL_UINT(42, original["profiles"][0]["rawBytes"][5].as<unsigned>());
    TEST_ASSERT_EQUAL_STRING("on", original["profiles"][0]["detector"]["display"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("unchanged", original["profiles"][0]["detector"]["volume"]["policy"].as<const char*>());

    // A preexisting legacy conflict is outside a profile-only restore's scope.
    manager->mutableSettings().proxyBLE = true;
    manager->mutableSettings().obdEnabled = true;
    const String unrelated = unrelatedSettingsSnapshot();
    const String secret = manager->getWifiStaSlotPassword(0);
    const auto credentialsBefore = mock_preferences::store().at("v1wificlient");
    JsonDocument incoming;
    replacement(incoming);
    const uint32_t revision = manager->displayConfigurationRevision();
    TEST_ASSERT_TRUE_MESSAGE(applyUsbProfileDocument(*manager, *profileManager, incoming, error).success, error.c_str());
    TEST_ASSERT_EQUAL_STRING(jsonText(incoming).c_str(), snapshot().c_str());
    TEST_ASSERT_EQUAL_STRING(unrelated.c_str(), unrelatedSettingsSnapshot().c_str());
    TEST_ASSERT_EQUAL_STRING(secret.c_str(), manager->getWifiStaSlotPassword(0).c_str());
    TEST_ASSERT_GREATER_THAN(revision, manager->displayConfigurationRevision());
    const auto credentialsAfter = mock_preferences::store().at("v1wificlient");
    TEST_ASSERT_EQUAL_UINT(credentialsBefore.size(), credentialsAfter.size());
    for (const auto& item : credentialsBefore) {
        TEST_ASSERT_TRUE(item.second.value == credentialsAfter.at(item.first).value);
    }
    V1Profile missing;
    TEST_ASSERT_EQUAL(ProfileStorageStatus::NotFound, profileManager->loadProfileResult("Original", missing).status);

    TEST_ASSERT_TRUE(applyUsbProfileDocument(*manager, *profileManager, original, error).success);
    TEST_ASSERT_EQUAL_STRING(jsonText(original).c_str(), snapshot().c_str());
    TEST_ASSERT_EQUAL(ProfileStorageStatus::NotFound, profileManager->loadProfileResult("A Replacement", missing).status);
    TEST_ASSERT_EQUAL_UINT(255, manager->getSlotVolume(0));
}

void setSlotModifiers(int slot, uint8_t main, uint8_t muted, bool dark) {
    AutoPushSlotUpdate update;
    update.slot = slot;
    update.hasVolume = update.hasMuteVolume = update.hasVolumeOverride = true;
    update.volume = main;
    update.muteVolume = muted;
    update.volumeOverride = true;
    update.hasDarkMode = update.hasDarkModeOverride = true;
    update.darkMode = dark;
    update.darkModeOverride = true;
    TEST_ASSERT_TRUE(manager->applyAutoPushSlotUpdatePersisted(update).success);
}

void assertSlotModifiers(int index, bool volumeOverride, uint8_t main, uint8_t muted,
                        bool darkOverride, bool dark) {
    const auto slot = manager->get().autoPushSlotView(index);
    TEST_ASSERT_EQUAL(volumeOverride, slot.volumeOverride);
    TEST_ASSERT_EQUAL_UINT8(main, slot.volume);
    TEST_ASSERT_EQUAL_UINT8(muted, slot.muteVolume);
    TEST_ASSERT_EQUAL(darkOverride, slot.darkModeOverride);
    TEST_ASSERT_EQUAL(dark, slot.darkMode);
}

void assertFullBackupAvailable() {
    JsonDocument backup;
    TEST_ASSERT_TRUE(BackupPayloadBuilder::buildBackupDocument(
        backup, manager->get(), *profileManager,
        BackupPayloadBuilder::BackupTransport::HttpDownload, 1000).safeToCommit);
    TEST_ASSERT_TRUE(manager->migrateAutoPushProfilesToV2());
}

void checkModifierPreservingRoundTrip(bool editAnotherSlot) {
    seed();
    setSlotModifiers(1, 6, 2, true);
    // Explicit zero volumes and an explicit dark-mode-off override must also
    // remain distinct from "use profile".
    setSlotModifiers(2, 0, 0, false);
    assertFullBackupAvailable();
    JsonDocument bundle;
    String error;
    TEST_ASSERT_TRUE(buildUsbProfileDocument(bundle, *manager, *profileManager, error));
    TEST_ASSERT_EQUAL_UINT8(4, bundle["version"].as<uint8_t>());
    TEST_ASSERT_EQUAL_UINT8(3, bundle["profiles"][0]["schemaVersion"].as<uint8_t>());
    if (editAnotherSlot) bundle["slots"][0]["alertPersist"] = 5;
    // Cross the production runtime's exact-JSON and PSRAM document boundary;
    // applying the exporter-owned JsonDocument directly would bypass it.
    const String payload = jsonText(bundle);
    UsbProfileJson::ParseStatus parseStatus = UsbProfileJson::ParseStatus::Invalid;
    const bool applied = UsbProfileJson::parseAndConsume(
        reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length(), parseStatus,
        [&](const JsonDocument& parsed) {
            return applyUsbProfileDocument(*manager, *profileManager, parsed, error).success;
        });
    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsbProfileJson::ParseStatus::Ok),
                          static_cast<int>(parseStatus));
    TEST_ASSERT_TRUE_MESSAGE(applied, error.c_str());
    for (int load = 0; load < 2; ++load) {
        assertSlotModifiers(0, false, 255, 255, false, false);
        assertSlotModifiers(1, true, 6, 2, true, true);
        assertSlotModifiers(2, true, 0, 0, true, false);
        TEST_ASSERT_EQUAL_UINT8(editAnotherSlot ? 5 : 1, manager->get().slot0AlertPersist);
        assertFullBackupAvailable();
        TEST_ASSERT_EQUAL_STRING(jsonText(bundle).c_str(), snapshot().c_str());
        // Load directly from committed NVS so an SD backup cannot hide loss.
        if (load == 0) {
            manager = std::make_unique<SettingsManager>(storage, *profileManager);
            manager->load();
        }
    }
}

void test_usb_round_trip_preserves_real_slot_modifiers_and_nvs() {
    checkModifierPreservingRoundTrip(false);
}

void test_usb_set_slot_preserves_other_slots_and_full_backup_validity() {
    checkModifierPreservingRoundTrip(true);
}

void checkBundleDisablesRecipientOverrides(int version) {
    seed();
    JsonDocument old;
    String error;
    TEST_ASSERT_TRUE(buildUsbProfileDocument(old, *manager, *profileManager, error));
    old["version"] = version;
    if (version < 4) {
        for (JsonObject slot : old["slots"].as<JsonArray>()) {
            for (const char* key : {"volumeOverride", "volume", "muteVolume", "darkModeOverride", "darkMode"}) {
                slot.remove(key);
            }
        }
    }
    if (version == 2) {
        for (JsonObject profile : old["profiles"].as<JsonArray>()) {
            profile["schemaVersion"] = 2;
            profile["detector"]["volume"].remove("feedback");
            profile["detector"]["volume"].remove("disconnect");
            profile["detector"]["bluetoothLed"] = "unchanged";
            profile["detector"]["customFrequencies"] = "unchanged";
        }
    }
    setSlotModifiers(1, 6, 2, true);
    TEST_ASSERT_TRUE_MESSAGE(applyUsbProfileDocument(*manager, *profileManager, old, error).success,
                             error.c_str());
    for (int load = 0; load < 2; ++load) {
        for (int slot = 0; slot < 3; ++slot) assertSlotModifiers(slot, false, 255, 255, false, false);
        assertFullBackupAvailable();
        if (load == 0) {
            manager = std::make_unique<SettingsManager>(storage, *profileManager);
            manager->load();
        }
    }
}

void test_v2_bundle_does_not_inherit_recipient_overrides() {
    checkBundleDisablesRecipientOverrides(2);
}

void test_v3_bundle_does_not_inherit_recipient_overrides() {
    checkBundleDisablesRecipientOverrides(3);
}

void test_v4_explicit_disabled_overrides_replace_recipient_values() {
    checkBundleDisablesRecipientOverrides(4);
}

void test_usb_modifiers_require_explicit_profiles_only_scope() {
    seed();
    setSlotModifiers(1, 6, 2, true);
    JsonDocument bundle, restore;
    String error;
    TEST_ASSERT_TRUE(buildUsbProfileDocument(bundle, *manager, *profileManager, error));
    TEST_ASSERT_TRUE(toRestoreDocument(bundle, restore, error));
    const auto filesBefore = filesSnapshot();
    const auto prefsBefore = mock_preferences::store();
    const String before = snapshot();
    // This partial internal document must not become a valid full backup just
    // because USB is allowed to carry schema-3 slot overrides.
    TEST_ASSERT_FALSE(manager->applyBackupDocument(restore, true).success);
    TEST_ASSERT_TRUE(filesBefore == filesSnapshot());
    TEST_ASSERT_TRUE(preferencesEqual(prefsBefore));
    TEST_ASSERT_EQUAL_STRING(before.c_str(), snapshot().c_str());
    TEST_ASSERT_TRUE(applyUsbProfileDocument(*manager, *profileManager, bundle, error).success);
}

void test_exact_legacy_profile_and_shared_slots_migrate_without_command_drift() {
    const uint8_t raw[] = {0xBF, 0xE1, 0x92, 0x73, 0xA5, 0x5A};
    writeLegacyProfileFile("Road", "Exact pre-v2 file", raw);

    V1Profile collision("Road - Slot 2");
    memset(collision.settings.bytes, 0xCC, 6);
    TEST_ASSERT_TRUE(profileManager->saveProfile(collision).success);

    auto& state = manager->mutableSettings();
    state.slot0Name = sanitizeSlotNameValue("?..|* HOSTILE SLOT LABEL THAT IS LEGAL AND VERY VERY VERY VERY LONG");
    state.slot1Name = sanitizeSlotNameValue("SECOND?SLOT");
    state.slot2Name = sanitizeSlotNameValue("THIRD:SLOT");
    state.slot0_default.profileName = "Road";
    state.slot0_default.mode = V1_MODE_ALL_BOGEYS;
    state.slot1_highway.profileName = "Road";
    state.slot1_highway.mode = V1_MODE_LOGIC;
    state.slot2_comfort.profileName = "Road";
    state.slot2_comfort.mode = V1_MODE_UNKNOWN;
    state.slot0AlertPersist = 1;
    state.slot1AlertPersist = 4;
    state.slot2AlertPersist = 5;
    state.slot0PriorityArrow = true;
    state.slot1DarkMode = true;
    state.slot2MuteToZero = true;
    state.slot0Volume = state.slot0MuteVolume = 0xFF;
    state.slot1Volume = state.slot1MuteVolume = 0;
    state.slot2Volume = 9;
    state.slot2MuteVolume = 3;
    TEST_ASSERT_TRUE(manager->saveDeferredBackup());

    TEST_ASSERT_TRUE(manager->migrateAutoPushProfilesToV2());
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_SCHEMA_VERSION, manager->get().autoPushProfileSchemaVersion);
    TEST_ASSERT_EQUAL_INT(V1_MODE_UNKNOWN, manager->get().slot0_default.mode);
    TEST_ASSERT_EQUAL_UINT8(0xFF, manager->getSlotVolume(1));
    TEST_ASSERT_FALSE(manager->getSlotDarkMode(1));
    TEST_ASSERT_FALSE(manager->getSlotMuteToZero(2));
    TEST_ASSERT_EQUAL_UINT8(1, manager->get().slot0AlertPersist);
    TEST_ASSERT_EQUAL_UINT8(4, manager->get().slot1AlertPersist);
    TEST_ASSERT_EQUAL_UINT8(5, manager->get().slot2AlertPersist);
    TEST_ASSERT_TRUE(manager->get().slot0PriorityArrow);

    const String slot0 = manager->get().slot0_default.profileName;
    const String slot1 = manager->get().slot1_highway.profileName;
    const String slot2 = manager->get().slot2_comfort.profileName;
    TEST_ASSERT_EQUAL_STRING("Road", slot0.c_str());
    TEST_ASSERT_EQUAL_STRING("Road - Slot 2 #2", slot1.c_str());
    TEST_ASSERT_EQUAL_STRING("Road - Slot 3", slot2.c_str());
    TEST_ASSERT_FALSE(slot0 == slot1);
    TEST_ASSERT_FALSE(slot1 == slot2);

    const uint8_t muteVolumeBytes[] = {0xBF, 0xE1, 0x92, 0x73, 0xA5, 0x5A};
    const uint8_t muteZeroBytes[] = {0xAF, 0xE1, 0x92, 0x73, 0xA5, 0x5A};
    assertMigratedApplication(slot0, muteVolumeBytes, V1ModePolicy::Value, 1,
                              V1DisplayPolicy::On, V1VolumePolicy::Unchanged);
    assertMigratedApplication(slot1, muteVolumeBytes, V1ModePolicy::Value, 2,
                              V1DisplayPolicy::Off, V1VolumePolicy::Temporary, 0, 0);
    assertMigratedApplication(slot2, muteZeroBytes, V1ModePolicy::Unchanged, 0,
                              V1DisplayPolicy::On, V1VolumePolicy::Temporary, 9, 3);

    const String firstExport = snapshot();
    TEST_ASSERT_TRUE(manager->migrateAutoPushProfilesToV2());
    TEST_ASSERT_EQUAL_STRING(firstExport.c_str(), snapshot().c_str());
    reboot();
    TEST_ASSERT_TRUE(manager->migrateAutoPushProfilesToV2());
    TEST_ASSERT_EQUAL_STRING(firstExport.c_str(), snapshot().c_str());
}

void test_multibyte_legacy_migration_names_preserve_suffixes_on_utf8_boundaries() {
    String sourceName;
    for (int index = 0; index < 32; ++index) sourceName += "\xC3\xA9";
    TEST_ASSERT_EQUAL_UINT(64, sourceName.length());
    String expectedFirst;
    for (int index = 0; index < 27; ++index) expectedFirst += "\xC3\xA9";
    expectedFirst += " - Slot 2";
    String expectedSecond;
    for (int index = 0; index < 26; ++index) expectedSecond += "\xC3\xA9";
    expectedSecond += " - Slot 2 #2";
    String expectedTenth;
    for (int index = 0; index < 25; ++index) expectedTenth += "\xC3\xA9";
    expectedTenth += " - Slot 2 #10";

    String candidate;
    TEST_ASSERT_TRUE(buildMigratedProfileNameCandidate(sourceName, " - Slot 2", 1, candidate));
    TEST_ASSERT_EQUAL_STRING(expectedFirst.c_str(), candidate.c_str());
    TEST_ASSERT_TRUE(buildMigratedProfileNameCandidate(sourceName, " - Slot 2", 2, candidate));
    TEST_ASSERT_EQUAL_STRING(expectedSecond.c_str(), candidate.c_str());
    TEST_ASSERT_TRUE(buildMigratedProfileNameCandidate(sourceName, " - Slot 2", 10, candidate));
    TEST_ASSERT_EQUAL_STRING(expectedTenth.c_str(), candidate.c_str());

    const uint8_t raw[] = {0xBF, 0xE1, 0x92, 0x73, 0xA5, 0x5A};
    writeLegacyProfileFile(sourceName.c_str(), "Multibyte source", raw);
    V1Profile collision(expectedFirst);
    TEST_ASSERT_TRUE(profileManager->saveProfile(collision).success);
    auto& state = manager->mutableSettings();
    state.slot0_default.profileName = sourceName;
    state.slot0_default.mode = V1_MODE_ALL_BOGEYS;
    state.slot1_highway.profileName = sourceName;
    state.slot1_highway.mode = V1_MODE_LOGIC;
    state.slot2_comfort.profileName = sourceName;
    state.slot2_comfort.mode = V1_MODE_ALL_BOGEYS;
    TEST_ASSERT_TRUE(manager->saveDeferredBackup());
    TEST_ASSERT_TRUE(manager->migrateAutoPushProfilesToV2());
    TEST_ASSERT_EQUAL_STRING(sourceName.c_str(), manager->get().slot0_default.profileName.c_str());
    TEST_ASSERT_EQUAL_STRING(expectedSecond.c_str(), manager->get().slot1_highway.profileName.c_str());
    TEST_ASSERT_EQUAL_STRING(sourceName.c_str(), manager->get().slot2_comfort.profileName.c_str());
    V1Profile migrated;
    TEST_ASSERT_TRUE(profileManager->loadProfile(expectedSecond, migrated));
    TEST_ASSERT_EQUAL_STRING(expectedSecond.c_str(), migrated.name.c_str());
}

void test_exact_v1_usb_import_is_immediately_exportable_with_same_effective_commands() {
    seed();
    setSlotModifiers(1, 6, 2, true);
    JsonDocument legacy;
    legacy["format"] = "v1simple-profiles";
    legacy["version"] = 1;
    legacy["autoPushEnabled"] = true;
    legacy["activeSlot"] = 2;
    JsonArray profiles = legacy["profiles"].to<JsonArray>();
    JsonObject profile = profiles.add<JsonObject>();
    profile["name"] = "Road";
    profile["description"] = "Exact USB v1";
    profile["displayOn"] = false;
    profile["mainVolume"] = 7;
    profile["mutedVolume"] = 2;
    JsonArray bytes = profile["rawBytes"].to<JsonArray>();
    const uint8_t raw[] = {0xBF, 1, 2, 3, 4, 5};
    for (uint8_t byte : raw) bytes.add(byte);
    JsonArray slots = legacy["slots"].to<JsonArray>();
    for (int index = 0; index < 3; ++index) {
        JsonObject slot = slots.add<JsonObject>();
        slot["name"] = index == 0 ? "HOME" : (index == 1 ? "HIGHWAY" : "COMFORT");
        slot["profile"] = "Road";
        slot["mode"] = index == 2 ? 0 : index + 1;
        slot["color"] = 100 + index;
        slot["volumeConfigured"] = index != 0;
        slot["volume"] = index == 0 ? 0 : (index == 1 ? 0 : 9);
        slot["muteVolume"] = index == 0 ? 0 : (index == 1 ? 0 : 3);
        slot["darkMode"] = index == 1;
        slot["muteToZero"] = index == 2;
        slot["alertPersist"] = index;
        slot["priorityArrowOnly"] = index == 0;
    }

    String error;
    const SettingsBackupApplyResult applied =
        applyUsbProfileDocument(*manager, *profileManager, legacy, error);
    TEST_ASSERT_TRUE_MESSAGE(applied.success, error.c_str());
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_SCHEMA_VERSION, manager->get().autoPushProfileSchemaVersion);
    for (int index = 0; index < 3; ++index) assertSlotModifiers(index, false, 255, 255, false, false);
    assertFullBackupAvailable();

    JsonDocument immediate;
    TEST_ASSERT_TRUE_MESSAGE(buildUsbProfileDocument(immediate, *manager, *profileManager, error), error.c_str());
    TEST_ASSERT_EQUAL_INT(kUsbProfileDocumentVersion, immediate["version"].as<int>());
    const String slot0 = manager->get().slot0_default.profileName;
    const String slot1 = manager->get().slot1_highway.profileName;
    const String slot2 = manager->get().slot2_comfort.profileName;
    const uint8_t muteBytes[] = {0xBF, 1, 2, 3, 4, 5};
    const uint8_t zeroBytes[] = {0xAF, 1, 2, 3, 4, 5};
    assertMigratedApplication(slot0, muteBytes, V1ModePolicy::Value, 1,
                              V1DisplayPolicy::On, V1VolumePolicy::Unchanged);
    assertMigratedApplication(slot1, muteBytes, V1ModePolicy::Value, 2,
                              V1DisplayPolicy::Off, V1VolumePolicy::Temporary, 0, 0);
    assertMigratedApplication(slot2, zeroBytes, V1ModePolicy::Unchanged, 0,
                              V1DisplayPolicy::On, V1VolumePolicy::Temporary, 9, 3);
}

void test_v1_usb_expansion_over_catalog_cap_is_rejected_before_any_commit() {
    seed();
    const String beforeBundle = snapshot();
    const auto beforeFiles = filesSnapshot();
    const auto beforePreferences = mock_preferences::store();

    JsonDocument legacy;
    legacy["format"] = "v1simple-profiles";
    legacy["version"] = 1;
    legacy["autoPushEnabled"] = true;
    legacy["activeSlot"] = 0;
    JsonArray profiles = legacy["profiles"].to<JsonArray>();
    for (size_t index = 0; index < V1_PROFILE_CATALOG_MAX_COUNT; ++index) {
        JsonObject profile = profiles.add<JsonObject>();
        String name = String("Profile ") + String(index + 1);
        profile["name"] = name;
        profile["description"] = "legacy expansion preflight";
        profile["displayOn"] = true;
        profile["mainVolume"] = 255;
        profile["mutedVolume"] = 255;
        JsonArray bytes = profile["rawBytes"].to<JsonArray>();
        for (int byte = 0; byte < 6; ++byte) bytes.add(byte == 0 ? 0xBF : byte);
    }
    JsonArray slots = legacy["slots"].to<JsonArray>();
    for (int index = 0; index < 3; ++index) {
        JsonObject slot = slots.add<JsonObject>();
        slot["name"] = index == 0 ? "DEFAULT" : (index == 1 ? "HIGHWAY" : "COMFORT");
        slot["profile"] = index < 2 ? "Profile 1" : "Profile 2";
        slot["mode"] = index < 2 ? index + 1 : 0;
        slot["color"] = 100 + index;
        slot["volumeConfigured"] = false;
        slot["volume"] = 0;
        slot["muteVolume"] = 0;
        slot["darkMode"] = index == 1;
        slot["muteToZero"] = false;
        slot["alertPersist"] = 0;
        slot["priorityArrowOnly"] = false;
    }

    String error;
    const SettingsBackupApplyResult result =
        applyUsbProfileDocument(*manager, *profileManager, legacy, error);
    TEST_ASSERT_FALSE(result.success);
    TEST_ASSERT_TRUE(error.indexOf("exceeds") >= 0);
    TEST_ASSERT_EQUAL_STRING(beforeBundle.c_str(), snapshot().c_str());
    TEST_ASSERT_TRUE(filesSnapshot() == beforeFiles);
    TEST_ASSERT_TRUE(preferencesEqual(beforePreferences));
}

void test_v1_usb_reports_pending_migration_and_reboot_recovers_pre_authority_state() {
    JsonDocument legacy;
    legacy["format"] = "v1simple-profiles";
    legacy["version"] = 1;
    legacy["autoPushEnabled"] = true;
    legacy["activeSlot"] = 0;
    JsonObject profile = legacy["profiles"].to<JsonArray>().add<JsonObject>();
    profile["name"] = "Road";
    profile["description"] = "Interrupted migration";
    profile["displayOn"] = true;
    profile["mainVolume"] = 255;
    profile["mutedVolume"] = 255;
    JsonArray bytes = profile["rawBytes"].to<JsonArray>();
    const uint8_t raw[] = {0xBF, 1, 2, 3, 4, 5};
    for (uint8_t byte : raw) bytes.add(byte);
    JsonArray slots = legacy["slots"].to<JsonArray>();
    for (int index = 0; index < 3; ++index) {
        JsonObject slot = slots.add<JsonObject>();
        slot["name"] = index == 0 ? "DEFAULT" : (index == 1 ? "HIGHWAY" : "COMFORT");
        slot["profile"] = "Road";
        slot["mode"] = index + 1;
        slot["color"] = 100 + index;
        slot["volumeConfigured"] = false;
        slot["volume"] = 0;
        slot["muteVolume"] = 0;
        slot["darkMode"] = false;
        slot["muteToZero"] = false;
        slot["alertPersist"] = 0;
        slot["priorityArrowOnly"] = false;
    }

    manager->utInterruptAutoPushMigrationAfterProfiles(true);
    String error;
    const SettingsBackupApplyResult applied =
        applyUsbProfileDocument(*manager, *profileManager, legacy, error);
    TEST_ASSERT_TRUE(applied.success);
    TEST_ASSERT_TRUE(applied.migrationPending);
    TEST_ASSERT_TRUE(error.indexOf("migration is pending") >= 0);
    TEST_ASSERT_EQUAL_UINT8(0, manager->get().autoPushProfileSchemaVersion);
    TEST_ASSERT_TRUE(primaryFs->exists("/v1restore_transaction.json"));

    JsonDocument unavailable;
    TEST_ASSERT_FALSE(buildUsbProfileDocument(unavailable, *manager, *profileManager, error));
    TEST_ASSERT_TRUE(unavailable.isNull());

    reboot();
    TEST_ASSERT_EQUAL_UINT8(0, manager->get().autoPushProfileSchemaVersion);
    TEST_ASSERT_FALSE(primaryFs->exists("/v1restore_transaction.json"));
    V1Profile restored;
    TEST_ASSERT_TRUE(profileManager->loadProfile("Road", restored));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(raw, restored.settings.bytes, 6);

    TEST_ASSERT_TRUE(manager->migrateAutoPushProfilesToV2());
    JsonDocument exported;
    TEST_ASSERT_TRUE_MESSAGE(buildUsbProfileDocument(exported, *manager, *profileManager, error), error.c_str());
    TEST_ASSERT_EQUAL_INT(kUsbProfileDocumentVersion, exported["version"].as<int>());
}

void test_invalid_complete_bundle_never_mutates_settings_profiles_or_credentials() {
    seed();
    JsonDocument original;
    String error;
    TEST_ASSERT_TRUE(buildUsbProfileDocument(original, *manager, *profileManager, error));
    const auto filesBefore = filesSnapshot();
    const auto prefsBefore = mock_preferences::store();
    const String before = snapshot();
    const std::vector<std::function<void(JsonDocument&)>> faults = {
        [](JsonDocument& d) { d["version"] = 5; },
        [](JsonDocument& d) { d["format"] = std::string("v1simple-profiles\0x", 19); },
        [](JsonDocument& d) { d["wifiClientEnabled"] = true; },
        [](JsonDocument& d) { d["slots"][0]["alertPersist"] = 6; },
        [](JsonDocument& d) { d["slots"][0]["alertPersist"] = "2"; },
        [](JsonDocument& d) { d["slots"][0]["color"] = 0; },
        [](JsonDocument& d) { d["slots"][0]["volume"] = 1; },
        [](JsonDocument& d) { d["slots"][0].remove("volumeOverride"); },
        [](JsonDocument& d) { d["slots"][0]["volumeOverride"] = true; },
        [](JsonDocument& d) { d["slots"][0]["volumeOverride"] = 1; },
        [](JsonDocument& d) { d["slots"][0]["muteVolume"] = -1; },
        [](JsonDocument& d) { d["slots"][0]["volume"] = "255"; },
        [](JsonDocument& d) { d["slots"][0]["darkModeOverride"] = "true"; },
        [](JsonDocument& d) { d["slots"][0]["darkMode"] = true; },
        [](JsonDocument& d) { d["slots"][0]["mode"] = 4; },
        [](JsonDocument& d) { d["slots"][0]["profile"] = "Absent"; },
        [](JsonDocument& d) { d["slots"][0]["darkMode"] = 1; },
        [](JsonDocument& d) { d["slots"].as<JsonArray>().remove(2); },
        [](JsonDocument& d) { d["profiles"][0]["rawBytes"][5] = 256; },
        [](JsonDocument& d) { d["profiles"][0]["rawBytes"].as<JsonArray>().remove(5); },
        [](JsonDocument& d) { d["profiles"][0]["description"] = 161; },
        [](JsonDocument& d) { d["profiles"][0]["description"] = std::string("note\0tail", 9); },
        [](JsonDocument& d) { d["profiles"][0]["name"] = "../Original"; },
        [](JsonDocument& d) { d["profiles"][0]["detector"]["volume"]["main"] = 10; },
        [](JsonDocument& d) {
            JsonObject custom = d["profiles"][0]["detector"]["customFrequencies"].to<JsonObject>();
            custom["policy"] = "value";
            JsonObject definition = custom["definitions"].to<JsonArray>().add<JsonObject>();
            definition["index"] = 0;
            definition["lowerMHz"] = 0;
            definition["upperMHz"] = 1;
        },
        [](JsonDocument& d) { d["profiles"][0].remove("schemaVersion"); },
        [](JsonDocument& d) { d["profiles"][0].remove("detector"); },
        [](JsonDocument& d) { d["profiles"][0]["extra"] = true; },
        [](JsonDocument& d) {
            JsonDocument copy;
            copy.set(d["profiles"][0]);
            d["profiles"].as<JsonArray>().add(copy.as<JsonObjectConst>());
            d["profiles"][d["profiles"].size() - 1]["name"] = "ORIGINAL";
        },
        [](JsonDocument& d) {
            d["profiles"][0]["description"] = std::string(V1_PROFILE_DESCRIPTION_MAX_BYTES + 1u, 'x');
        },
    };
    for (const auto& fault : faults) {
        JsonDocument broken;
        broken.set(original);
        fault(broken);
        TEST_ASSERT_FALSE(applyUsbProfileDocument(*manager, *profileManager, broken, error).success);
        TEST_ASSERT_GREATER_THAN(0, error.length());
        TEST_ASSERT_TRUE(filesBefore == filesSnapshot());
        TEST_ASSERT_TRUE(preferencesEqual(prefsBefore));
        TEST_ASSERT_EQUAL_STRING(before.c_str(), snapshot().c_str());
    }
}

void test_v3_export_refuses_noncanonical_zero_slot_color() {
    seed();
    manager->mutableSettings().slot1Color = 0;
    JsonDocument output;
    String error;
    TEST_ASSERT_FALSE(buildUsbProfileDocument(output, *manager, *profileManager, error));
    TEST_ASSERT_TRUE(output.isNull());
    TEST_ASSERT_GREATER_THAN(0, error.length());
}

void test_nvs_failure_rolls_back_created_deleted_profiles_and_settings() {
    seed();
    const String before = snapshot();
    const String unrelated = unrelatedSettingsSnapshot();
    JsonDocument incoming;
    replacement(incoming);
    String error;
    const uint32_t revisionBefore = manager->displayConfigurationRevision();
    mock_preferences::set_fail_writes_for_key(kNvsBrightness);
    TEST_ASSERT_FALSE(applyUsbProfileDocument(*manager, *profileManager, incoming, error).success);
    mock_preferences::set_fail_writes_for_key(nullptr);
    TEST_ASSERT_GREATER_THAN(revisionBefore, manager->displayConfigurationRevision());
    TEST_ASSERT_EQUAL_STRING(before.c_str(), snapshot().c_str());
    TEST_ASSERT_EQUAL_STRING(unrelated.c_str(), unrelatedSettingsSnapshot().c_str());
    reboot();
    TEST_ASSERT_EQUAL_STRING(before.c_str(), snapshot().c_str());
    TEST_ASSERT_EQUAL_STRING("fixture-password", manager->getWifiStaSlotPassword(0).c_str());
}

void test_profile_storage_failure_during_replacement_rolls_back_catalog() {
    seed();
    const String before = snapshot();
    JsonDocument incoming;
    replacement(incoming);
    for (int failAtFeed : {2, 3}) {
        struct FailurePoint { int calls; int failAt; } point{0, failAtFeed};
        SettingsRestoreWatchdog watchdog{
            [](void* ctx) {
                auto& point = *static_cast<FailurePoint*>(ctx);
                if (++point.calls == point.failAt) fs::mock_fail_next_rename();
            }, &point};
        String error;
        TEST_ASSERT_FALSE(applyUsbProfileDocument(*manager, *profileManager, incoming, error, watchdog).success);
        TEST_ASSERT_EQUAL_STRING(before.c_str(), snapshot().c_str());
        TEST_ASSERT_EQUAL_STRING("fixture-password", manager->getWifiStaSlotPassword(0).c_str());
        TEST_ASSERT_FALSE(primaryFs->exists("/v1restore_transaction.json"));
        TEST_ASSERT_FALSE(secondaryFs->exists("/v1restore_transaction.json"));
    }
}

void test_interrupted_replacement_recovers_both_catalog_and_slots_from_mirrored_journal() {
    seed();
    const String before = snapshot();
    JsonDocument incoming;
    replacement(incoming);
    String error;
    manager->utInterruptRestoreAfterProfiles(true);
    TEST_ASSERT_FALSE(applyUsbProfileDocument(*manager, *profileManager, incoming, error).success);
    V1Profile missing;
    TEST_ASSERT_EQUAL(ProfileStorageStatus::NotFound, profileManager->loadProfileResult("Original", missing).status);
    TEST_ASSERT_TRUE(primaryFs->exists("/v1restore_transaction.json"));
    TEST_ASSERT_TRUE(secondaryFs->exists("/v1restore_transaction.json"));
    reboot();
    TEST_ASSERT_EQUAL_STRING(before.c_str(), snapshot().c_str());
    TEST_ASSERT_EQUAL_STRING("fixture-password", manager->getWifiStaSlotPassword(0).c_str());
    TEST_ASSERT_FALSE(primaryFs->exists("/v1restore_transaction.json"));
    TEST_ASSERT_FALSE(secondaryFs->exists("/v1restore_transaction.json"));
}

void test_committed_replacement_survives_reboot_and_stale_journal_cleanup() {
    seed();
    JsonDocument incoming;
    replacement(incoming);
    String error;
    manager->utLeaveRestoreJournalAfterCommit(true);
    TEST_ASSERT_TRUE(applyUsbProfileDocument(*manager, *profileManager, incoming, error).success);
    TEST_ASSERT_TRUE(primaryFs->exists("/v1restore_transaction.json"));
    reboot();
    TEST_ASSERT_EQUAL_STRING(jsonText(incoming).c_str(), snapshot().c_str());
    TEST_ASSERT_FALSE(primaryFs->exists("/v1restore_transaction.json"));
    TEST_ASSERT_FALSE(secondaryFs->exists("/v1restore_transaction.json"));
}

void test_case_only_catalog_replacement_recovers_exact_original_name() {
    seed();
    const String before = snapshot();
    JsonDocument incoming;
    replacement(incoming);
    incoming["profiles"][0]["name"] = "ORIGINAL";
    for (JsonObject slot : incoming["slots"].as<JsonArray>()) slot["profile"] = "ORIGINAL";
    String error;
    manager->utInterruptRestoreAfterProfiles(true);
    TEST_ASSERT_FALSE(applyUsbProfileDocument(*manager, *profileManager, incoming, error).success);
    reboot();
    TEST_ASSERT_EQUAL_STRING(before.c_str(), snapshot().c_str());
    TEST_ASSERT_TRUE(applyUsbProfileDocument(*manager, *profileManager, incoming, error).success);
    reboot();
    TEST_ASSERT_EQUAL_STRING(jsonText(incoming).c_str(), snapshot().c_str());
}

void test_empty_catalog_restore_deletes_extras_and_clears_assignments() {
    seed();
    JsonDocument incoming;
    replacement(incoming);
    incoming["profiles"].to<JsonArray>();
    for (JsonObject slot : incoming["slots"].as<JsonArray>()) slot["profile"] = "";
    String error;
    TEST_ASSERT_TRUE(applyUsbProfileDocument(*manager, *profileManager, incoming, error).success);
    TEST_ASSERT_EQUAL_STRING(jsonText(incoming).c_str(), snapshot().c_str());
    reboot();
    TEST_ASSERT_EQUAL_STRING(jsonText(incoming).c_str(), snapshot().c_str());
}

void test_unavailable_catalog_export_returns_no_partial_bundle() {
    seed();
    V1ProfileManager unavailable;
    JsonDocument doc;
    doc["stale"] = true;
    String error;
    TEST_ASSERT_FALSE(buildUsbProfileDocument(doc, *manager, unavailable, error));
    TEST_ASSERT_TRUE(doc.isNull());
    TEST_ASSERT_GREATER_THAN(0, error.length());
}

void test_profile_validation_staging_allocation_failure_returns_no_partial_bundle() {
    seed();
    JsonDocument doc;
    doc["stale"] = true;
    String error;
    g_failPreparedUsbProfileAllocationForTest = true;
    TEST_ASSERT_FALSE(buildUsbProfileDocument(doc, *manager, *profileManager, error));
    TEST_ASSERT_TRUE(doc.isNull());
    TEST_ASSERT_TRUE(error.indexOf("profile import staging") >= 0);
    TEST_ASSERT_FALSE(g_failPreparedUsbProfileAllocationForTest);

    TEST_ASSERT_TRUE_MESSAGE(
        buildUsbProfileDocument(doc, *manager, *profileManager, error), error.c_str());
    TEST_ASSERT_EQUAL_UINT(4, doc["profiles"].size());
}

void test_export_refuses_failed_recovery_then_exports_recovered_complete_state() {
    seed();
    const String before = snapshot();
    JsonDocument incoming;
    replacement(incoming);
    String error;
    manager->utInterruptRestoreAfterProfiles(true);
    TEST_ASSERT_FALSE(applyUsbProfileDocument(*manager, *profileManager, incoming, error).success);
    manager.reset();
    profileManager = std::make_unique<V1ProfileManager>();
    TEST_ASSERT_TRUE(profileManager->begin(storage));
    manager = std::make_unique<SettingsManager>(storage, *profileManager);
    manager->load();

    JsonDocument output;
    fs::mock_set_fs_write_budget(0);
    TEST_ASSERT_FALSE(buildUsbProfileDocument(output, *manager, *profileManager, error));
    TEST_ASSERT_TRUE(output.isNull());
    fs::mock_reset_fs_write_budget();
    TEST_ASSERT_TRUE_MESSAGE(buildUsbProfileDocument(output, *manager, *profileManager, error), error.c_str());
    TEST_ASSERT_EQUAL_STRING(before.c_str(), jsonText(output).c_str());
    TEST_ASSERT_FALSE(primaryFs->exists("/v1restore_transaction.json"));
    TEST_ASSERT_FALSE(secondaryFs->exists("/v1restore_transaction.json"));
}


void test_http_profile_metadata_round_trips_storage_usb_and_backup_without_truncation() {
    WifiV1ProfileApiService::Runtime runtime{};
    runtime.parseSettingsJson = [](const JsonObject& object, uint8_t bytes[6], void*) {
        V1UserSettings parsed;
        if (!profileManager->jsonToSettings(object, parsed)) return false;
        memcpy(bytes, parsed.bytes, 6);
        return true;
    };
    runtime.saveProfile = [](const String& name, const String& description,
                            const V1DetectorConfiguration& detector, const uint8_t bytes[6],
                            bool createOnly, String& error, void*) {
        V1Profile profile(name);
        profile.description = description;
        profile.detector = detector;
        memcpy(profile.settings.bytes, bytes, 6);
        const auto result = profileManager->saveProfile(profile, createOnly);
        error = result.error;
        return result.success;
    };
    for (const std::string& description : {std::string(160, 'a'), std::string(161, 'b'),
                                           std::string(1024, 'c') + " \xc3\xa9\n",
                                           std::string(V1_PROFILE_DESCRIPTION_MAX_BYTES, 'd')}) {
        JsonDocument request;
        request["name"] = "Road";
        request["description"] = description;
        request["schemaVersion"] = V1_PROFILE_SCHEMA_VERSION;
        V1DetectorConfiguration detector;
        detector.displayPolicy = V1DisplayPolicy::On;
        appendV1DetectorConfiguration(request["detector"].to<JsonObject>(), detector);
        request["settings"]["xBand"] = true;
        WebServer server(80);
        server.setArg("plain", jsonText(request));
        WifiV1ProfileApiService::handleApiProfileSave(server, runtime, nullptr, nullptr);
        TEST_ASSERT_EQUAL_INT(200, server.lastStatusCode);
        reboot();
        V1Profile stored;
        TEST_ASSERT_TRUE(profileManager->loadProfile("Road", stored));
        TEST_ASSERT_EQUAL_STRING(description.c_str(), stored.description.c_str());

        if (manager->get().autoPushProfileSchemaVersion != V1_PROFILE_SCHEMA_VERSION) {
            TEST_ASSERT_TRUE(manager->migrateAutoPushProfilesToV2());
        }

        JsonDocument usb;
        String error;
        TEST_ASSERT_TRUE_MESSAGE(buildUsbProfileDocument(usb, *manager, *profileManager, error), error.c_str());
        stored.description = "Intervening edit";
        TEST_ASSERT_TRUE(profileManager->saveProfile(stored).success);
        TEST_ASSERT_TRUE_MESSAGE(applyUsbProfileDocument(*manager, *profileManager, usb, error).success, error.c_str());
        TEST_ASSERT_TRUE(profileManager->loadProfile("Road", stored));
        TEST_ASSERT_EQUAL_STRING(description.c_str(), stored.description.c_str());

        JsonDocument backup;
        const auto built = BackupPayloadBuilder::buildBackupDocument(
            backup, manager->get(), *profileManager, BackupPayloadBuilder::BackupTransport::HttpDownload, 1000);
        TEST_ASSERT_TRUE(built.safeToCommit);
        stored.description = "Another edit";
        TEST_ASSERT_TRUE(profileManager->saveProfile(stored).success);
        TEST_ASSERT_TRUE(manager->applyBackupDocument(backup, true).success);
        reboot();
        TEST_ASSERT_TRUE(profileManager->loadProfile("Road", stored));
        TEST_ASSERT_EQUAL_STRING(description.c_str(), stored.description.c_str());
    }
}

void test_interrupted_restore_recovers_long_description_from_journal() {
    seed();
    JsonDocument incoming;
    replacement(incoming);
    V1Profile original;
    TEST_ASSERT_TRUE(profileManager->loadProfile("Original", original));
    const std::string description = std::string(1024, 'r') + " \xc3\xa9\n";
    original.description = description.c_str();
    TEST_ASSERT_TRUE(profileManager->saveProfile(original).success);
    manager->utInterruptRestoreAfterProfiles(true);
    String error;
    TEST_ASSERT_FALSE(applyUsbProfileDocument(*manager, *profileManager, incoming, error).success);
    TEST_ASSERT_TRUE(primaryFs->exists("/v1restore_transaction.json"));
    reboot();
    V1Profile recovered;
    TEST_ASSERT_TRUE(profileManager->loadProfile("Original", recovered));
    TEST_ASSERT_EQUAL_STRING(description.c_str(), recovered.description.c_str());
    TEST_ASSERT_EQUAL_MEMORY(original.settings.bytes, recovered.settings.bytes, 6);
    TEST_ASSERT_FALSE(primaryFs->exists("/v1restore_transaction.json"));
}

void test_maximum_profile_delete_journal_recovers_after_interruption() {
    V1Profile profile(String(std::string(MAX_PROFILE_NAME_LEN, 'D')));
    profile.description = String(std::string(V1_PROFILE_DESCRIPTION_MAX_BYTES, '"'));
    const uint8_t maximumRaw[] = {192, 217, 252, 255, 255, 255};
    memcpy(profile.settings.bytes, maximumRaw, sizeof(maximumRaw));
    profile.detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    profile.detector.modePolicy = V1ModePolicy::Value;
    profile.detector.mode = 3;
    profile.detector.displayPolicy = V1DisplayPolicy::Unchanged;
    profile.detector.volumePolicy = V1VolumePolicy::Temporary;
    profile.detector.mainVolume = 9;
    profile.detector.mutedVolume = 9;
    profile.detector.volumeFeedback = V1VolumeFeedbackPolicy::ChangedOnly;
    profile.detector.volumeDisconnect = V1VolumeDisconnectPolicy::RestoreSaved;
    profile.detector.bluetoothLedPolicy = V1BluetoothLedPolicy::Unchanged;
    profile.detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    for (uint8_t index = 0; index < 64; ++index) {
        profile.detector.customFrequencyDefinitions.push_back({index, 65534, 65535});
    }
    TEST_ASSERT_TRUE(profileManager->saveProfile(profile).success);
    manager->mutableSettings().autoPushProfileSchemaVersion = V1_PROFILE_SCHEMA_VERSION;
    manager->mutableSettings().slot0_default.profileName = profile.name;
    TEST_ASSERT_TRUE(manager->saveDeferredBackup());

    manager->utInterruptProfileDeleteAfterJournal(true);
    TEST_ASSERT_FALSE(manager->deleteProfileAndReferences(profile.name).success());
    TEST_ASSERT_TRUE(primaryFs->exists(PROFILE_DELETE_TRANSACTION_PATH));
    File journal = primaryFs->open(PROFILE_DELETE_TRANSACTION_PATH, FILE_READ);
    TEST_ASSERT_TRUE(journal);
    TEST_ASSERT_EQUAL_UINT(11742u, journal.size());
    journal.close();

    reboot();
    TEST_ASSERT_FALSE(primaryFs->exists(PROFILE_DELETE_TRANSACTION_PATH));
    TEST_ASSERT_EQUAL_STRING(profile.name.c_str(), manager->get().slot0_default.profileName.c_str());
    V1Profile recovered;
    TEST_ASSERT_TRUE(profileManager->loadProfile(profile.name, recovered));
    TEST_ASSERT_EQUAL_STRING(profile.description.c_str(), recovered.description.c_str());
    TEST_ASSERT_EQUAL_UINT(64u, recovered.detector.customFrequencyDefinitions.size());
}

void test_shared_description_limit_rejects_oversized_metadata_without_loss() {
    seed();
    const String before = snapshot();
    JsonDocument incoming;
    replacement(incoming);
    incoming["profiles"][0]["description"] = std::string(V1_PROFILE_DESCRIPTION_MAX_BYTES + 1u, 'x');
    TEST_ASSERT_LESS_THAN(kUsbProfileDocumentMaxBytes, measureJson(incoming));
    String error;
    TEST_ASSERT_FALSE(applyUsbProfileDocument(*manager, *profileManager, incoming, error).success);
    TEST_ASSERT_EQUAL_STRING(before.c_str(), snapshot().c_str());
    reboot();
    TEST_ASSERT_EQUAL_STRING(before.c_str(), snapshot().c_str());
}

void test_schema_v2_catalog_builds_self_consistent_v3_backup_and_round_trips() {
    writeV2ProfileFile();
    JsonDocument unsafeBackup;
    const auto unsafeBuilt = BackupPayloadBuilder::buildBackupDocument(
        unsafeBackup, manager->get(), *profileManager,
        BackupPayloadBuilder::BackupTransport::HttpDownload, 1234);
    TEST_ASSERT_FALSE(unsafeBuilt.safeToCommit);
    TEST_ASSERT_TRUE(manager->migrateAutoPushProfilesToV2());
    JsonDocument backup;
    const auto built = BackupPayloadBuilder::buildBackupDocument(
        backup, manager->get(), *profileManager,
        BackupPayloadBuilder::BackupTransport::HttpDownload, 1234);
    TEST_ASSERT_TRUE(built.safeToCommit);
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_SCHEMA_VERSION,
                            backup["autoPushProfileSchemaVersion"].as<uint8_t>());
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_SCHEMA_VERSION,
                            backup["profiles"][0]["schemaVersion"].as<uint8_t>());
    TEST_ASSERT_EQUAL_STRING("off", backup["profiles"][0]["detector"]["display"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("off", backup["profiles"][0]["detector"]["bluetoothLed"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("none",
                             backup["profiles"][0]["detector"]["volume"]["feedback"].as<const char*>());

    V1Profile edited;
    TEST_ASSERT_TRUE(profileManager->loadProfile("V2 Road", edited));
    edited.description = "intervening edit";
    TEST_ASSERT_TRUE(profileManager->saveProfile(edited).success);
    manager->mutableSettings().slot0_default.profileName = "";
    TEST_ASSERT_TRUE(manager->saveDeferredBackup());
    TEST_ASSERT_TRUE(manager->applyBackupDocument(backup, true).success);
    reboot();
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_SCHEMA_VERSION,
                            manager->get().autoPushProfileSchemaVersion);
    V1Profile restored;
    TEST_ASSERT_TRUE(profileManager->loadProfile("V2 Road", restored));
    TEST_ASSERT_EQUAL_STRING("validated schema two", restored.description.c_str());
    TEST_ASSERT_EQUAL_INT(V1BluetoothLedPolicy::Off, restored.detector.bluetoothLedPolicy);
    TEST_ASSERT_EQUAL_INT(V1VolumeFeedbackPolicy::None, restored.detector.volumeFeedback);
}

void test_persisted_schema_v2_catalog_migration_is_atomic_and_reboot_durable() {
    writeV2ProfileFile();
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_PREVIOUS_SCHEMA_VERSION, persistedProfileSchema());
    manager->utInterruptAutoPushMigrationAfterProfiles(true);
    TEST_ASSERT_FALSE(manager->migrateAutoPushProfilesToV2());
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_PREVIOUS_SCHEMA_VERSION,
                            manager->get().autoPushProfileSchemaVersion);
    TEST_ASSERT_TRUE(primaryFs->exists("/v1restore_transaction.json"));

    reboot();
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_PREVIOUS_SCHEMA_VERSION,
                            manager->get().autoPushProfileSchemaVersion);
    // Profile files may already be durably staged as v3, but the marker stays
    // v2 and remains the command authority until a later transaction commits.
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_SCHEMA_VERSION, persistedProfileSchema());
    TEST_ASSERT_FALSE(primaryFs->exists("/v1restore_transaction.json"));
    TEST_ASSERT_TRUE(manager->migrateAutoPushProfilesToV2());
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_SCHEMA_VERSION,
                            manager->get().autoPushProfileSchemaVersion);
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_SCHEMA_VERSION, persistedProfileSchema());
    reboot();
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_SCHEMA_VERSION,
                            manager->get().autoPushProfileSchemaVersion);
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_SCHEMA_VERSION, persistedProfileSchema());
    V1Profile migrated;
    TEST_ASSERT_TRUE(profileManager->loadProfile("V2 Road", migrated));
    TEST_ASSERT_EQUAL_INT(V1DisplayPolicy::Off, migrated.detector.displayPolicy);
    TEST_ASSERT_EQUAL_INT(V1BluetoothLedPolicy::Off, migrated.detector.bluetoothLedPolicy);
    TEST_ASSERT_EQUAL_INT(V1VolumeFeedbackPolicy::None, migrated.detector.volumeFeedback);
    TEST_ASSERT_EQUAL_INT(V1VolumeDisconnectPolicy::RestoreSaved,
                          migrated.detector.volumeDisconnect);
}

void test_usb_bulk_document_oom_never_dispatches_restore_consumer() {
    static constexpr uint8_t VALID[] = "{\"version\":3,\"profiles\":[]}";
    bool consumerCalled = false;
    UsbProfileJson::ParseStatus status = UsbProfileJson::ParseStatus::Invalid;
    g_mock_heap_caps_fail_all_allocations = true;
    const bool parsed = UsbProfileJson::parseAndConsume(
        VALID, sizeof(VALID) - 1u, status, [&](const JsonDocument&) {
            consumerCalled = true;
            return true;
        });
    g_mock_heap_caps_fail_all_allocations = false;

    TEST_ASSERT_FALSE(parsed);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(UsbProfileJson::ParseStatus::MemoryUnavailable),
                          static_cast<int>(status));
    TEST_ASSERT_FALSE(consumerCalled);
}

void test_measure_maximum_profile_catalog_transport_shapes() {
    auto& maximumSettings = manager->mutableSettings();
    maximumSettings.autoPushProfileSchemaVersion = V1_PROFILE_SCHEMA_VERSION;
    maximumSettings.apSSID = String(std::string(MAX_WIFI_SSID_LEN, '"'));
    maximumSettings.apPassword = String(std::string(MAX_AP_PASSWORD_LEN, 'Q'));
    maximumSettings.wifiClientEnabled = false;
    maximumSettings.proxyBLE = false;
    maximumSettings.proxyName = String(std::string(MAX_PROXY_NAME_LEN, '"'));
    maximumSettings.lastV1Address = "AA:BB:CC:DD:EE:FF";
    maximumSettings.autoPowerOffMinutes = 60;
    maximumSettings.apTimeoutMinutes = 60;
    maximumSettings.obdEnabled = false;
    maximumSettings.obdSavedAddress = "AA:BB:CC:DD:EE:FF";
    maximumSettings.obdSavedName = String(std::string(32, '"'));
    maximumSettings.obdSavedAddrType = 1;
    maximumSettings.obdMinRssi = -100;
    maximumSettings.obdScanWindowMs = kConnectionCycleObdScanWindowMsMax;
    maximumSettings.obdRetryIntervalMs = kConnectionCycleObdRetryIntervalMsMax;
    maximumSettings.proxyOpenWindowMs = kConnectionCycleProxyOpenWindowMsMax;
    maximumSettings.v1SettleQuietMs = kConnectionCycleV1SettleQuietMsMax;
    maximumSettings.v1SettleFallbackMs = kConnectionCycleV1SettleFallbackMsMax;
    maximumSettings.cycleTeardownAckTimeoutMs = kConnectionCycleTeardownAckTimeoutMsMax;
    maximumSettings.alpEnabled = false;
    maximumSettings.alpAlertPersistSec = 5;
    maximumSettings.alpDisableV1LaserOnPush = false;
    maximumSettings.gpsEnabled = false;
    maximumSettings.gpsBaud = 115200;
    maximumSettings.brightness = 255;
    maximumSettings.colorBogey = maximumSettings.colorFrequency = 65535;
    maximumSettings.colorArrowFront = maximumSettings.colorArrowSide = maximumSettings.colorArrowRear = 65535;
    maximumSettings.colorBandL = maximumSettings.colorBandKa = maximumSettings.colorBandK = 65535;
    maximumSettings.colorBandX = maximumSettings.colorBandPhoto = 65535;
    maximumSettings.colorWiFiConnected = maximumSettings.colorBleConnected = 65535;
    maximumSettings.colorBleDisconnected = 65535;
    for (uint16_t& color : maximumSettings.colorBars) color = 65535;
    maximumSettings.colorMuted = maximumSettings.colorPersisted = 65535;
    maximumSettings.colorVolumeMain = maximumSettings.colorVolumeMute = 65535;
    maximumSettings.colorRssiV1 = maximumSettings.colorRssiProxy = 65535;
    maximumSettings.colorObd = maximumSettings.colorAlpConnected = 65535;
    maximumSettings.colorAlpDli = maximumSettings.colorAlpLidActive = maximumSettings.colorAlpAlert = 65535;
    maximumSettings.freqUseBandColor = false;
    maximumSettings.hideWifiIcon = maximumSettings.hideProfileIndicator = false;
    maximumSettings.hideBatteryIcon = maximumSettings.showBatteryPercent = false;
    maximumSettings.hideBleIcon = maximumSettings.hideVolumeIndicator = false;
    maximumSettings.hideRssiIndicator = false;
    maximumSettings.voiceAlertMode = VOICE_MODE_BAND_FREQ;
    maximumSettings.voiceDirectionEnabled = maximumSettings.announceBogeyCount = false;
    maximumSettings.muteVoiceIfVolZero = false;
    maximumSettings.voiceVolume = 100;
    maximumSettings.announceSecondaryAlerts = maximumSettings.secondaryLaser = false;
    maximumSettings.secondaryKa = maximumSettings.secondaryK = maximumSettings.secondaryX = false;
    maximumSettings.alertVolumeFadeEnabled = false;
    maximumSettings.alertVolumeFadeDelaySec = 10;
    maximumSettings.alertVolumeFadeVolume = 9;
    maximumSettings.speedMuteEnabled = false;
    maximumSettings.speedMuteThresholdMph = 60;
    maximumSettings.speedMuteHysteresisMph = 10;
    maximumSettings.speedMuteVolume = 9;
    maximumSettings.speedMuteVoice = maximumSettings.stealthEnabled = false;
    maximumSettings.autoPushEnabled = false;
    maximumSettings.activeSlot = 2;
    const String maximumSlotName(std::string(MAX_SLOT_NAME_LEN, '"'));
    const String maximumStaLabel(std::string(MAX_WIFI_STA_LABEL_LEN, '"'));
    const String maximumPassword(std::string(MAX_WIFI_PASSWORD_LEN, 'P'));
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        const char prefix[] = {static_cast<char>('A' + index), '\0'};
        String ssid(prefix);
        ssid += String(std::string(MAX_WIFI_SSID_LEN - 1u, '"'));
        TEST_ASSERT_TRUE(manager->setWifiStaSlotCredentials(index, ssid, maximumPassword,
                                                           maximumStaLabel, 255));
        maximumSettings.wifiStaSlots[index].lastConnectedAtSec =
            std::numeric_limits<uint32_t>::max();
    }
    maximumSettings.wifiClientSSID = maximumSettings.wifiStaSlots[0].ssid;
    for (int slotIndex = 0; slotIndex < 3; ++slotIndex) {
        auto slot = maximumSettings.autoPushSlotView(slotIndex);
        slot.name = maximumSlotName;
        slot.color = 65535;
        slot.volume = 255;
        slot.muteVolume = 255;
        slot.darkMode = false;
        slot.muteToZero = false;
        slot.alertPersist = 5;
        slot.priorityArrow = false;
        slot.config.mode = V1_MODE_UNKNOWN;
    }
    for (uint8_t count = 1; count <= V1_PROFILE_CATALOG_MAX_COUNT; ++count) {
        String name = String("P") + String(count);
        while (name.length() < MAX_PROFILE_NAME_LEN) name += 'N';
        V1Profile profile(name);
        profile.description = String(std::string(V1_PROFILE_DESCRIPTION_MAX_BYTES, '"'));
        const uint8_t maximumRaw[] = {192, 217, 252, 255, 255, 255};
        memcpy(profile.settings.bytes, maximumRaw, sizeof(maximumRaw));
        profile.detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
        profile.detector.modePolicy = V1ModePolicy::Value;
        profile.detector.mode = 3;
        profile.detector.displayPolicy = V1DisplayPolicy::Unchanged;
        profile.detector.volumePolicy = V1VolumePolicy::Temporary;
        profile.detector.mainVolume = 9;
        profile.detector.mutedVolume = 9;
        profile.detector.volumeFeedback = V1VolumeFeedbackPolicy::ChangedOnly;
        profile.detector.volumeDisconnect = V1VolumeDisconnectPolicy::RestoreSaved;
        profile.detector.bluetoothLedPolicy = V1BluetoothLedPolicy::Unchanged;
        profile.detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
        for (uint8_t index = 0; index < 64; ++index) {
            profile.detector.customFrequencyDefinitions.push_back({index, 65534, 65535});
        }
        TEST_ASSERT_TRUE(profileManager->saveProfile(profile).success);
        if (count == 1) {
            for (int slotIndex = 0; slotIndex < 3; ++slotIndex) {
                maximumSettings.autoPushSlotView(slotIndex).config.profileName = name;
            }
        }

        PsramJson::Document usb;
        String error;
        const bool usbBuilt = buildUsbProfileDocument(usb, *manager, *profileManager, error);
        const String context = String("count=") + String(count) + " error=" + error;
        TEST_ASSERT_TRUE_MESSAGE(usbBuilt, context.c_str());
        PsramJson::Document backup;
        const auto result = BackupPayloadBuilder::buildBackupDocument(
            backup, manager->get(), *profileManager,
            BackupPayloadBuilder::BackupTransport::HttpDownload,
            std::numeric_limits<uint32_t>::max());
        TEST_ASSERT_TRUE(result.safeToCommit);
        if (count == V1_PROFILE_CATALOG_MAX_COUNT) {
            TEST_ASSERT_EQUAL_UINT(117187, measureJson(usb));
            TEST_ASSERT_EQUAL_UINT(120645, measureJson(backup));
            PsramJson::Document sdBackup;
            const auto sdResult = BackupPayloadBuilder::buildBackupDocument(
                sdBackup, manager->get(), *profileManager,
                BackupPayloadBuilder::BackupTransport::SdBackup,
                std::numeric_limits<uint32_t>::max());
            TEST_ASSERT_TRUE(sdResult.safeToCommit);
            TEST_ASSERT_EQUAL_UINT(120814, measureJson(sdBackup));
            TEST_ASSERT_GREATER_THAN(4096u, 128u * 1024u - measureJson(sdBackup));
        }
    }

    // The restore journal is the binding complete-catalog consumer: it embeds
    // the ten-profile snapshot plus every credential store needed for exact
    // rollback. Exercise the actual writer with maximally long, JSON-escaped
    // SSIDs and maximum passwords so the shared 128 KiB limit is not justified
    // by export documents alone.
    RestoreCredentialSnapshot credentials;
    TEST_ASSERT_TRUE(captureRestoreCredentialSnapshot(storage, credentials));
    std::vector<V1Profile> profilesBefore;
    TEST_ASSERT_TRUE(profileManager->snapshotProfiles(profilesBefore).success());
    TEST_ASSERT_EQUAL_UINT(V1_PROFILE_CATALOG_MAX_COUNT, profilesBefore.size());
    TEST_ASSERT_TRUE(writeRestoreTransactionJournal(storage, 1, true, credentials, true, profilesBefore));
    File restoreJournal = primaryFs->open(RESTORE_TRANSACTION_PATH, FILE_READ);
    TEST_ASSERT_TRUE(restoreJournal);
    const size_t restoreJournalBytes = restoreJournal.size();
    restoreJournal.close();
    TEST_ASSERT_EQUAL_UINT(118759u, restoreJournalBytes);
    TEST_ASSERT_LESS_THAN_UINT(RESTORE_TRANSACTION_MAX_BYTES, restoreJournalBytes);
    TEST_ASSERT_GREATER_THAN_UINT(4096u, RESTORE_TRANSACTION_MAX_BYTES - restoreJournalBytes);

    TEST_ASSERT_TRUE(writeProfileDeleteTransactionJournal(storage, 2, profilesBefore.front(), true));
    File deleteJournal = primaryFs->open(PROFILE_DELETE_TRANSACTION_PATH, FILE_READ);
    TEST_ASSERT_TRUE(deleteJournal);
    const size_t deleteJournalBytes = deleteJournal.size();
    deleteJournal.close();
    TEST_ASSERT_EQUAL_UINT(11742u, deleteJournalBytes);
    TEST_ASSERT_LESS_THAN_UINT(PROFILE_DELETE_TRANSACTION_MAX_BYTES, deleteJournalBytes);
    TEST_ASSERT_GREATER_THAN_UINT(4096u, PROFILE_DELETE_TRANSACTION_MAX_BYTES - deleteJournalBytes);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_http_profile_metadata_round_trips_storage_usb_and_backup_without_truncation);
    RUN_TEST(test_interrupted_restore_recovers_long_description_from_journal);
    RUN_TEST(test_maximum_profile_delete_journal_recovers_after_interruption);
    RUN_TEST(test_shared_description_limit_rejects_oversized_metadata_without_loss);
    RUN_TEST(test_schema_v2_catalog_builds_self_consistent_v3_backup_and_round_trips);
    RUN_TEST(test_persisted_schema_v2_catalog_migration_is_atomic_and_reboot_durable);
    RUN_TEST(test_usb_bulk_document_oom_never_dispatches_restore_consumer);
    RUN_TEST(test_usb_import_string_copy_failure_never_reaches_restore_or_mutates_state);
    RUN_TEST(test_measure_maximum_profile_catalog_transport_shapes);
    RUN_TEST(test_bundle_round_trip_replaces_catalog_and_preserves_unrelated_state);
    RUN_TEST(test_usb_round_trip_preserves_real_slot_modifiers_and_nvs);
    RUN_TEST(test_usb_set_slot_preserves_other_slots_and_full_backup_validity);
    RUN_TEST(test_v2_bundle_does_not_inherit_recipient_overrides);
    RUN_TEST(test_v3_bundle_does_not_inherit_recipient_overrides);
    RUN_TEST(test_v4_explicit_disabled_overrides_replace_recipient_values);
    RUN_TEST(test_usb_modifiers_require_explicit_profiles_only_scope);
    RUN_TEST(test_exact_legacy_profile_and_shared_slots_migrate_without_command_drift);
    RUN_TEST(test_multibyte_legacy_migration_names_preserve_suffixes_on_utf8_boundaries);
    RUN_TEST(test_exact_v1_usb_import_is_immediately_exportable_with_same_effective_commands);
    RUN_TEST(test_v1_usb_expansion_over_catalog_cap_is_rejected_before_any_commit);
    RUN_TEST(test_v1_usb_reports_pending_migration_and_reboot_recovers_pre_authority_state);
    RUN_TEST(test_invalid_complete_bundle_never_mutates_settings_profiles_or_credentials);
    RUN_TEST(test_v3_export_refuses_noncanonical_zero_slot_color);
    RUN_TEST(test_nvs_failure_rolls_back_created_deleted_profiles_and_settings);
    RUN_TEST(test_profile_storage_failure_during_replacement_rolls_back_catalog);
    RUN_TEST(test_interrupted_replacement_recovers_both_catalog_and_slots_from_mirrored_journal);
    RUN_TEST(test_committed_replacement_survives_reboot_and_stale_journal_cleanup);
    RUN_TEST(test_case_only_catalog_replacement_recovers_exact_original_name);
    RUN_TEST(test_empty_catalog_restore_deletes_extras_and_clears_assignments);
    RUN_TEST(test_unavailable_catalog_export_returns_no_partial_bundle);
    RUN_TEST(test_profile_validation_staging_allocation_failure_returns_no_partial_bundle);
    RUN_TEST(test_export_refuses_failed_recovery_then_exports_recovered_complete_state);
    return UNITY_END();
}
