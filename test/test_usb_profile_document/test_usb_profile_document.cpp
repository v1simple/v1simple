#include <unity.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
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
    JsonDocument doc;
    const auto result = BackupPayloadBuilder::buildBackupDocument(
        doc, manager->get(), *profileManager, BackupPayloadBuilder::BackupTransport::SdBackup, 1000);
    TEST_ASSERT_TRUE(result.safeToCommit);
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
    manager.reset();
    profileManager.reset();
    primaryFs.reset();
    secondaryFs.reset();
    std::filesystem::remove_all(root);
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

void test_exact_v1_usb_import_is_immediately_exportable_with_same_effective_commands() {
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

    JsonDocument immediate;
    TEST_ASSERT_TRUE_MESSAGE(buildUsbProfileDocument(immediate, *manager, *profileManager, error), error.c_str());
    TEST_ASSERT_EQUAL_INT(2, immediate["version"].as<int>());
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
    TEST_ASSERT_EQUAL_INT(2, exported["version"].as<int>());
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
        [](JsonDocument& d) { d["version"] = 3; },
        [](JsonDocument& d) { d["format"] = std::string("v1simple-profiles\0x", 19); },
        [](JsonDocument& d) { d["wifiClientEnabled"] = true; },
        [](JsonDocument& d) { d["slots"][0]["alertPersist"] = 6; },
        [](JsonDocument& d) { d["slots"][0]["alertPersist"] = "2"; },
        [](JsonDocument& d) { d["slots"][0]["volume"] = 1; },
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
        [](JsonDocument& d) { d["profiles"][0].remove("schemaVersion"); },
        [](JsonDocument& d) { d["profiles"][0].remove("detector"); },
        [](JsonDocument& d) { d["profiles"][0]["extra"] = true; },
        [](JsonDocument& d) {
            JsonDocument copy;
            copy.set(d["profiles"][0]);
            d["profiles"].as<JsonArray>().add(copy.as<JsonObjectConst>());
            d["profiles"][d["profiles"].size() - 1]["name"] = "ORIGINAL";
        },
        [](JsonDocument& d) { d["profiles"][0]["description"] = std::string(kUsbProfileDocumentMaxBytes, 'x'); },
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
                            String& error, void*) {
        V1Profile profile(name);
        profile.description = description;
        profile.detector = detector;
        memcpy(profile.settings.bytes, bytes, 6);
        const auto result = profileManager->saveProfile(profile);
        error = result.error;
        return result.success;
    };
    for (const std::string& description : {std::string(160, 'a'), std::string(161, 'b'),
                                           std::string(1024, 'c') + " \xc3\xa9\n"}) {
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

void test_profile_file_size_limit_still_rejects_oversized_metadata_without_loss() {
    seed();
    const String before = snapshot();
    JsonDocument incoming;
    replacement(incoming);
    // The whole bundle fits USB's envelope, but the stored profile would
    // exceed its existing 4096-byte file limit. Restore must roll back.
    incoming["profiles"][0]["description"] = std::string(4096, 'x');
    TEST_ASSERT_LESS_THAN(kUsbProfileDocumentMaxBytes, measureJson(incoming));
    String error;
    TEST_ASSERT_FALSE(applyUsbProfileDocument(*manager, *profileManager, incoming, error).success);
    TEST_ASSERT_EQUAL_STRING(before.c_str(), snapshot().c_str());
    reboot();
    TEST_ASSERT_EQUAL_STRING(before.c_str(), snapshot().c_str());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_http_profile_metadata_round_trips_storage_usb_and_backup_without_truncation);
    RUN_TEST(test_interrupted_restore_recovers_long_description_from_journal);
    RUN_TEST(test_profile_file_size_limit_still_rejects_oversized_metadata_without_loss);
    RUN_TEST(test_bundle_round_trip_replaces_catalog_and_preserves_unrelated_state);
    RUN_TEST(test_exact_legacy_profile_and_shared_slots_migrate_without_command_drift);
    RUN_TEST(test_exact_v1_usb_import_is_immediately_exportable_with_same_effective_commands);
    RUN_TEST(test_v1_usb_reports_pending_migration_and_reboot_recovers_pre_authority_state);
    RUN_TEST(test_invalid_complete_bundle_never_mutates_settings_profiles_or_credentials);
    RUN_TEST(test_nvs_failure_rolls_back_created_deleted_profiles_and_settings);
    RUN_TEST(test_profile_storage_failure_during_replacement_rolls_back_catalog);
    RUN_TEST(test_interrupted_replacement_recovers_both_catalog_and_slots_from_mirrored_journal);
    RUN_TEST(test_committed_replacement_survives_reboot_and_stale_journal_cleanup);
    RUN_TEST(test_case_only_catalog_replacement_recovers_exact_original_name);
    RUN_TEST(test_empty_catalog_restore_deletes_extras_and_clears_assignments);
    RUN_TEST(test_unavailable_catalog_export_returns_no_partial_bundle);
    RUN_TEST(test_export_refuses_failed_recovery_then_exports_recovered_complete_state);
    return UNITY_END();
}
