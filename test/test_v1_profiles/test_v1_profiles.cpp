#include <unity.h>

#include <cstring>
#include <filesystem>
#include <string>

#include <ArduinoJson.h>

#include "../mocks/Arduino.h"
#include "../mocks/Preferences.h"
#include "../mocks/nvs.h"
#include "../mocks/storage_manager.h"

namespace ArduinoJson {

inline void convertFromJson(JsonVariantConst src, ::String& dst) {
    const char* raw = src.as<const char*>();
    dst = ::String(raw ? raw : "");
}

inline bool canConvertFromJson(JsonVariantConst src, const ::String&) {
    return src.is<const char*>();
}

}  // namespace ArduinoJson

#include "../../src/v1_profiles.cpp"
#include "../../src/backup_payload_builder.cpp"
#include "../../src/psram_freertos_alloc.cpp"
#include "../../src/settings.cpp"
#include "../../src/settings_nvs.cpp"
#include "../../src/settings_backup.cpp"
#include "../../src/settings_backup_doc.cpp"
#include "../../src/settings_restore.cpp"

#ifndef ARDUINO
SerialClass Serial;
#endif

unsigned long mockMillis = 0;
unsigned long mockMicros = 0;

namespace {

std::filesystem::path g_tempRoot;
int g_tempRootIndex = 0;

std::filesystem::path nextTempRoot() {
    return std::filesystem::temp_directory_path() /
           ("v1_profiles_" + std::to_string(++g_tempRootIndex));
}

std::string readFileToString(fs::FS& fs, const char* path) {
    File file = fs.open(path, FILE_READ);
    if (!file) {
        return {};
    }

    std::string output;
    while (file.available()) {
        output.push_back(static_cast<char>(file.read()));
    }
    file.close();
    return output;
}

void writeFileFromString(fs::FS& fs, const char* path, const char* contents) {
    File file = fs.open(path, FILE_WRITE);
    TEST_ASSERT_TRUE(file);
    TEST_ASSERT_EQUAL_UINT(std::strlen(contents), file.print(contents));
    file.close();
}

size_t countFilesInProfileDir(const char* suffix = nullptr) {
    const std::filesystem::path profileDir = g_tempRoot / "v1profiles";
    if (!std::filesystem::exists(profileDir)) {
        return 0;
    }

    size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(profileDir)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string filename = entry.path().filename().string();
        if (suffix != nullptr) {
            const std::string wantedSuffix(suffix);
            if (filename.size() < wantedSuffix.size() ||
                filename.compare(filename.size() - wantedSuffix.size(),
                                 wantedSuffix.size(),
                                 wantedSuffix) != 0) {
                continue;
            }
        }
        ++count;
    }
    return count;
}

V1Profile makeProfile(const String& name,
                      uint8_t baseByte,
                      const String& description = "profile") {
    V1Profile profile(name);
    profile.description = description;
    profile.displayOn = true;
    profile.mainVolume = 6;
    profile.mutedVolume = 2;
    for (int i = 0; i < 6; ++i) {
        profile.settings.bytes[i] = static_cast<uint8_t>(baseByte + i);
    }
    return profile;
}

}  // namespace

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    fs::mock_reset_fs_rename_state();
    fs::mock_reset_fs_write_budget();
    fs::mock_reset_fs_open_state();
    mock_reset_semaphore_state();
    g_tempRoot = nextTempRoot();
    std::filesystem::remove_all(g_tempRoot);
    std::filesystem::create_directories(g_tempRoot);
}

void tearDown() {
    fs::mock_reset_fs_rename_state();
    fs::mock_reset_fs_write_budget();
    fs::mock_reset_fs_open_state();
    if (!g_tempRoot.empty()) {
        std::filesystem::remove_all(g_tempRoot);
    }
}

void test_save_profile_short_write_new_file_leaves_no_live_json() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    fs::mock_set_fs_write_budget(32);

    const ProfileSaveResult result = manager.saveProfile(makeProfile("Road", 10, "new"));

    TEST_ASSERT_FALSE(result.success);
    TEST_ASSERT_TRUE(result.error.indexOf("Partial write detected") >= 0);
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Road.json"));
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Road.json.tmp"));
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Road.json.bak"));
}

void test_save_profile_short_write_existing_file_preserves_previous_profile() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    const V1Profile original = makeProfile("Road", 20, "original");
    ProfileSaveResult initialSave = manager.saveProfile(original);
    TEST_ASSERT_TRUE(initialSave.success);
    const std::string before = readFileToString(fs, "/v1profiles/Road.json");

    fs::mock_set_fs_write_budget(32);
    const ProfileSaveResult result = manager.saveProfile(makeProfile("Road", 80, "updated"));

    TEST_ASSERT_FALSE(result.success);
    TEST_ASSERT_TRUE(result.error.indexOf("Partial write detected") >= 0);
    TEST_ASSERT_EQUAL_STRING(before.c_str(), readFileToString(fs, "/v1profiles/Road.json").c_str());
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Road.json.tmp"));
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Road.json.bak"));

    V1Profile loaded;
    TEST_ASSERT_TRUE(manager.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("original", loaded.description.c_str());
    TEST_ASSERT_EQUAL_UINT8(20, loaded.settings.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(25, loaded.settings.bytes[5]);
}

void test_save_profile_normal_path_still_succeeds() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    const V1Profile profile = makeProfile("Quiet", 30, "normal");
    const ProfileSaveResult result = manager.saveProfile(profile);

    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_TRUE(fs.exists("/v1profiles/Quiet.json"));

    V1Profile loaded;
    TEST_ASSERT_TRUE(manager.loadProfile("Quiet", loaded));
    TEST_ASSERT_EQUAL_STRING("normal", loaded.description.c_str());
    TEST_ASSERT_EQUAL_UINT8(30, loaded.settings.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(35, loaded.settings.bytes[5]);
}

void test_save_requires_final_file_reopen_and_crc_validation() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));
    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("Road", 10, "original")).success);

    fs::mock_fail_next_read_open("/v1profiles/Road.json");
    const ProfileSaveResult result = manager.saveProfile(makeProfile("Road", 40, "replacement"));

    TEST_ASSERT_FALSE(result.success);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::IoError), static_cast<int>(result.status));
    V1Profile restored;
    TEST_ASSERT_TRUE(manager.loadProfile("Road", restored));
    TEST_ASSERT_EQUAL_STRING("original", restored.description.c_str());
    TEST_ASSERT_EQUAL_UINT8(10, restored.settings.bytes[0]);
}

void test_load_profile_rejects_invalid_raw_bytes_without_mutating_output() {
    const char* invalidProfiles[] = {
        "{\"description\":\"bad-string\",\"bytes\":[1,2,3,4,5,\"6\"],\"crc32\":0}",
        "{\"description\":\"bad-bool\",\"bytes\":[1,2,3,4,5,true],\"crc32\":0}",
        "{\"description\":\"bad-null\",\"bytes\":[1,2,3,4,5,null],\"crc32\":0}",
        "{\"description\":\"bad-fraction\",\"bytes\":[1,2,3,4,5,5.5],\"crc32\":0}",
        "{\"description\":\"bad-low\",\"bytes\":[1,2,3,4,5,-1],\"crc32\":0}",
        "{\"description\":\"bad-high\",\"bytes\":[1,2,3,4,5,256],\"crc32\":0}",
    };

    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    for (const char* profileJson : invalidProfiles) {
        writeFileFromString(fs, "/v1profiles/Invalid.json", profileJson);
        V1Profile output = makeProfile("Sentinel", 90, "unchanged");
        const V1Profile before = output;

        TEST_ASSERT_FALSE(manager.loadProfile("Invalid", output));
        TEST_ASSERT_EQUAL_STRING(before.name.c_str(), output.name.c_str());
        TEST_ASSERT_EQUAL_STRING(before.description.c_str(), output.description.c_str());
        TEST_ASSERT_EQUAL_UINT8_ARRAY(before.settings.bytes, output.settings.bytes, 6);
    }
}

void test_json_to_settings_rejects_invalid_raw_bytes_without_mutating_output() {
    const char* invalidSettings[] = {
        "{\"bytes\":[1,2,3,4,5,\"6\"]}",
        "{\"bytes\":[1,2,3,4,5,true]}",
        "{\"bytes\":[1,2,3,4,5,null]}",
        "{\"bytes\":[1,2,3,4,5,5.5]}",
        "{\"bytes\":[1,2,3,4,5,-1]}",
        "{\"bytes\":[1,2,3,4,5,256]}",
    };

    V1ProfileManager manager;
    for (const char* settingsJson : invalidSettings) {
        V1UserSettings settings = makeProfile("Sentinel", 100).settings;
        const V1UserSettings before = settings;

        TEST_ASSERT_FALSE(manager.jsonToSettings(String(settingsJson), settings));
        TEST_ASSERT_EQUAL_UINT8_ARRAY(before.bytes, settings.bytes, 6);
    }
}

void test_v41039_photo_settings_round_trip_through_json() {
    V1ProfileManager manager;
    V1UserSettings settings;

    TEST_ASSERT_TRUE(manager.jsonToSettings(
        String("{\"gatsoRT4\":true,\"photoIntersectionFilter\":true}"), settings));
    TEST_ASSERT_TRUE(settings.gatsoRT4());
    TEST_ASSERT_TRUE(settings.photoIntersectionFilter());
    TEST_ASSERT_EQUAL_UINT8(0xFC, settings.bytes[4]);

    const String json = manager.settingsToJson(settings);
    JsonDocument doc;
    TEST_ASSERT_FALSE(deserializeJson(doc, json));
    TEST_ASSERT_TRUE(doc["gatsoRT4"].as<bool>());
    TEST_ASSERT_TRUE(doc["photoIntersectionFilter"].as<bool>());
}

void test_rename_same_name_is_successful_noop() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("City", 40, "same-name")).success);
    const uint32_t beforeRevision = manager.catalogRevision();
    const std::string before = readFileToString(fs, "/v1profiles/City.json");

    TEST_ASSERT_TRUE(manager.renameProfile("City", "City"));
    TEST_ASSERT_EQUAL_UINT32(beforeRevision, manager.catalogRevision());
    TEST_ASSERT_EQUAL_STRING(before.c_str(), readFileToString(fs, "/v1profiles/City.json").c_str());
}

void test_path_like_name_is_rejected_without_creating_a_profile() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    const ProfileSaveResult result = manager.saveProfile(makeProfile("Road/1", 50, "invalid"));
    TEST_ASSERT_FALSE(result.success);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::InvalidName), static_cast<int>(result.status));
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(countFilesInProfileDir(".json")));
}

void test_profile_name_contract_rejects_hidden_long_blank_and_canonical_collisions() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    String longName;
    for (int i = 0; i < 65; ++i) longName += 'A';
    TEST_ASSERT_FALSE(manager.saveProfile(makeProfile("   ", 1)).success);
    TEST_ASSERT_FALSE(manager.saveProfile(makeProfile(".hidden", 1)).success);
    TEST_ASSERT_FALSE(manager.saveProfile(makeProfile("_hidden", 1)).success);
    TEST_ASSERT_FALSE(manager.saveProfile(makeProfile("dir\\name", 1)).success);
    TEST_ASSERT_FALSE(manager.saveProfile(makeProfile(longName, 1)).success);
    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("Road", 2)).success);
    TEST_ASSERT_FALSE(manager.saveProfile(makeProfile("road", 3)).success);
    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("  Quiet  ", 4)).success);
    V1Profile quiet;
    TEST_ASSERT_TRUE(manager.loadProfile("Quiet", quiet));
    TEST_ASSERT_EQUAL_STRING("Quiet", quiet.name.c_str());
}

void test_sd_contention_returns_busy_for_every_profile_transaction() {
    fs::FS fs(g_tempRoot);
    StorageManager localStorage;
    localStorage.setFilesystem(&fs, true);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(localStorage));
    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("Road", 10)).success);

    V1Profile loaded;
    std::vector<V1Profile> snapshot;
    for (int i = 0; i < 5; ++i) mock_queue_semaphore_take_result(pdFALSE);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::Busy),
                          static_cast<int>(manager.listProfilesResult().status));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::Busy),
                          static_cast<int>(manager.loadProfileResult("Road", loaded).status));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::Busy),
                          static_cast<int>(manager.saveProfile(makeProfile("Other", 20)).status));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::Busy),
                          static_cast<int>(manager.deleteProfileResult("Road", 0).status));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::Busy),
                          static_cast<int>(manager.snapshotProfiles(snapshot).status));
}

void test_littlefs_fallback_edits_and_deletion_reconcile_without_resurrection() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd";
    const std::filesystem::path littleRoot = g_tempRoot / "little";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);

    V1ProfileManager fallback;
    TEST_ASSERT_TRUE(fallback.begin(&little));
    TEST_ASSERT_TRUE(fallback.saveProfile(makeProfile("Road", 10, "fallback-new")).success);

    StorageManager localStorage;
    localStorage.setFilesystem(&sd, true);
    localStorage.setLittleFS(&little);
    V1ProfileManager onSd;
    TEST_ASSERT_TRUE(onSd.begin(localStorage));
    V1Profile loaded;
    TEST_ASSERT_TRUE(onSd.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("fallback-new", loaded.description.c_str());

    TEST_ASSERT_TRUE(onSd.saveProfile(makeProfile("Road", 30, "sd-newer")).success);
    V1ProfileManager fallbackAgain;
    TEST_ASSERT_TRUE(fallbackAgain.begin(&little));
    TEST_ASSERT_TRUE(fallbackAgain.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("sd-newer", loaded.description.c_str());
    const std::string stalePayload = readFileToString(little, "/v1profiles/Road.json");
    TEST_ASSERT_TRUE(fallbackAgain.deleteProfile("Road"));

    // Simulate stale bytes surviving/reappearing after deletion. The durable
    // tombstone must hide them locally and remove them when SD returns.
    writeFileFromString(little, "/v1profiles/Road.json", stalePayload.c_str());
    V1ProfileManager interruptedFallback;
    TEST_ASSERT_TRUE(interruptedFallback.begin(&little));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::NotFound),
                          static_cast<int>(interruptedFallback.loadProfileResult("Road", loaded).status));
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(interruptedFallback.listProfiles().size()));

    V1ProfileManager sdReturns;
    TEST_ASSERT_TRUE(sdReturns.begin(localStorage));
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::NotFound),
                          static_cast<int>(sdReturns.loadProfileResult("Road", loaded).status));
    TEST_ASSERT_FALSE(sd.exists("/v1profiles/Road.json"));
    TEST_ASSERT_FALSE(little.exists("/v1profiles/Road.json"));
}

void test_rename_existing_distinct_destination_fails_without_mutation() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("Alpha", 60, "alpha")).success);
    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("Beta", 70, "beta")).success);
    const uint32_t beforeRevision = manager.catalogRevision();
    const std::string alphaBefore = readFileToString(fs, "/v1profiles/Alpha.json");
    const std::string betaBefore = readFileToString(fs, "/v1profiles/Beta.json");

    TEST_ASSERT_FALSE(manager.renameProfile("Alpha", "Beta"));
    TEST_ASSERT_EQUAL_UINT32(beforeRevision, manager.catalogRevision());
    TEST_ASSERT_EQUAL_STRING(alphaBefore.c_str(), readFileToString(fs, "/v1profiles/Alpha.json").c_str());
    TEST_ASSERT_EQUAL_STRING(betaBefore.c_str(), readFileToString(fs, "/v1profiles/Beta.json").c_str());

    V1Profile loadedAlpha;
    V1Profile loadedBeta;
    TEST_ASSERT_TRUE(manager.loadProfile("Alpha", loadedAlpha));
    TEST_ASSERT_TRUE(manager.loadProfile("Beta", loadedBeta));
    TEST_ASSERT_EQUAL_STRING("alpha", loadedAlpha.description.c_str());
    TEST_ASSERT_EQUAL_STRING("beta", loadedBeta.description.c_str());
}

void test_rename_normal_path_succeeds_and_advances_revision() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("Quiet", 80, "rename")).success);
    const uint32_t beforeRevision = manager.catalogRevision();

    TEST_ASSERT_TRUE(manager.renameProfile("Quiet", "Highway"));
    TEST_ASSERT_TRUE(manager.catalogRevision() > beforeRevision);
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Quiet.json"));
    TEST_ASSERT_TRUE(fs.exists("/v1profiles/Highway.json"));

    V1Profile loaded;
    TEST_ASSERT_TRUE(manager.loadProfile("Highway", loaded));
    TEST_ASSERT_EQUAL_STRING("Highway", loaded.name.c_str());
    TEST_ASSERT_EQUAL_STRING("rename", loaded.description.c_str());
    TEST_ASSERT_EQUAL_UINT8(80, loaded.settings.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(85, loaded.settings.bytes[5]);
}

void test_equal_generation_divergence_converges_to_offline_fallback_edit() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd";
    const std::filesystem::path littleRoot = g_tempRoot / "little";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);

    V1ProfileManager sdOnly;
    TEST_ASSERT_TRUE(sdOnly.begin(&sd));
    TEST_ASSERT_TRUE(sdOnly.saveProfile(makeProfile("Road", 10, "sd-v1")).success);
    TEST_ASSERT_TRUE(sdOnly.saveProfile(makeProfile("Road", 20, "sd-v2")).success);

    V1ProfileManager fallbackOnly;
    TEST_ASSERT_TRUE(fallbackOnly.begin(&little));
    TEST_ASSERT_TRUE(fallbackOnly.saveProfile(makeProfile("Road", 10, "fallback-v1")).success);
    TEST_ASSERT_TRUE(fallbackOnly.saveProfile(makeProfile("Road", 30, "fallback-v2")).success);

    StorageManager storage;
    storage.setFilesystem(&sd, true);
    storage.setLittleFS(&little);
    V1ProfileManager reconciled;
    TEST_ASSERT_TRUE(reconciled.begin(storage));

    V1Profile loaded;
    TEST_ASSERT_TRUE(reconciled.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("fallback-v2", loaded.description.c_str());

    V1ProfileManager fallbackAfter;
    TEST_ASSERT_TRUE(fallbackAfter.begin(&little));
    TEST_ASSERT_TRUE(fallbackAfter.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("fallback-v2", loaded.description.c_str());
    TEST_ASSERT_EQUAL_STRING(readFileToString(sd, "/v1profiles/Road.json").c_str(),
                             readFileToString(little, "/v1profiles/Road.json").c_str());
}

void test_corrupt_newer_profile_cannot_replace_valid_older_mirror() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd";
    const std::filesystem::path littleRoot = g_tempRoot / "little";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);

    V1ProfileManager sdOnly;
    TEST_ASSERT_TRUE(sdOnly.begin(&sd));
    TEST_ASSERT_TRUE(sdOnly.saveProfile(makeProfile("Road", 10, "valid-sd")).success);

    V1ProfileManager fallbackOnly;
    TEST_ASSERT_TRUE(fallbackOnly.begin(&little));
    TEST_ASSERT_TRUE(fallbackOnly.saveProfile(makeProfile("Road", 20, "fallback-v1")).success);
    TEST_ASSERT_TRUE(fallbackOnly.saveProfile(makeProfile("Road", 30, "fallback-v2")).success);
    writeFileFromString(little, "/v1profiles/Road.json", "{corrupt-json");

    StorageManager storage;
    storage.setFilesystem(&sd, true);
    storage.setLittleFS(&little);
    V1ProfileManager reconciled;
    TEST_ASSERT_TRUE(reconciled.begin(storage));

    V1Profile loaded;
    TEST_ASSERT_TRUE(reconciled.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("valid-sd", loaded.description.c_str());

    V1ProfileManager fallbackAfter;
    TEST_ASSERT_TRUE(fallbackAfter.begin(&little));
    TEST_ASSERT_TRUE(fallbackAfter.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("valid-sd", loaded.description.c_str());
}

void test_profile_sync_metadata_short_write_is_rejected() {
    fs::FS fs(g_tempRoot);
    ProfileSyncState state;
    state.version = 42;
    state.deleted = true;

    fs::mock_set_fs_write_budget(2);
    TEST_ASSERT_FALSE(writeSyncState(fs, "/v1profiles/Road.json", state));
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Road.json.meta"));
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Road.json.meta.tmp"));
}

void test_secondary_profile_save_failure_is_reported_and_rolls_back_both_copies() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd";
    const std::filesystem::path littleRoot = g_tempRoot / "little";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);

    StorageManager storage;
    storage.setFilesystem(&sd, true);
    storage.setLittleFS(&little);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(storage));
    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("Road", 10, "existing")).success);

    fs::mock_reset_fs_rename_state();
    fs::mock_fail_rename_on_call(6); // secondary live-file promotion after its rollback snapshot
    const ProfileSaveResult saved = manager.saveProfile(makeProfile("Road", 20, "replacement"));
    TEST_ASSERT_FALSE(saved.success);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::IoError), static_cast<int>(saved.status));

    V1Profile loaded;
    TEST_ASSERT_TRUE(manager.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("existing", loaded.description.c_str());

    fs::mock_reset_fs_rename_state();
    V1ProfileManager fallbackOnly;
    TEST_ASSERT_TRUE(fallbackOnly.begin(&little));
    TEST_ASSERT_TRUE(fallbackOnly.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("existing", loaded.description.c_str());
}

void test_secondary_tombstone_failure_is_reported_and_preserves_profile() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd";
    const std::filesystem::path littleRoot = g_tempRoot / "little";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);

    V1ProfileManager fallback;
    TEST_ASSERT_TRUE(fallback.begin(&little));
    TEST_ASSERT_TRUE(fallback.saveProfile(makeProfile("Road", 10, "existing")).success);

    StorageManager storage;
    storage.setFilesystem(&sd, true);
    storage.setLittleFS(&little);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(storage));

    fs::mock_reset_fs_rename_state();
    fs::mock_fail_rename_on_call(3); // first rename of the secondary metadata promotion
    const ProfileOperationResult deleted = manager.deleteProfileResult("Road");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::IoError), static_cast<int>(deleted.status));

    V1Profile loaded;
    TEST_ASSERT_TRUE(manager.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("existing", loaded.description.c_str());

    fs::mock_reset_fs_rename_state();
    V1ProfileManager afterReboot;
    TEST_ASSERT_TRUE(afterReboot.begin(storage));
    TEST_ASSERT_TRUE(afterReboot.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("existing", loaded.description.c_str());
    TEST_ASSERT_TRUE(sd.exists("/v1profiles/Road.json"));
    TEST_ASSERT_TRUE(little.exists("/v1profiles/Road.json"));
}

void test_existing_malformed_sync_metadata_fails_closed_without_resurrecting_json() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));
    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("Road", 10, "preserved-bytes")).success);

    const char* corruptDocuments[] = {"{}", "{\"version\":2", "{\"version\":2,\"deleted\":false,\"extra\":1}"};
    for (const char* corrupt : corruptDocuments) {
        writeFileFromString(fs, "/v1profiles/Road.json.meta", corrupt);
        V1Profile loaded;
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::Corrupt),
                              static_cast<int>(manager.loadProfileResult("Road", loaded).status));
        const ProfileListResult catalog = manager.listProfilesResult();
        TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::Corrupt),
                              static_cast<int>(catalog.status));
        TEST_ASSERT_TRUE(fs.exists("/v1profiles/Road.json"));
    }
}

void test_sync_metadata_crc_mutation_and_oversize_fail_closed() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));
    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("Road", 20, "crc")).success);

    JsonDocument meta;
    File input = fs.open("/v1profiles/Road.json.meta", FILE_READ);
    TEST_ASSERT_TRUE(input);
    TEST_ASSERT_FALSE(deserializeJson(meta, input));
    input.close();
    meta["version"] = meta["version"].as<uint32_t>() + 1u; // Deliberately retain the old CRC.
    String mutated;
    serializeJson(meta, mutated);
    writeFileFromString(fs, "/v1profiles/Road.json.meta", mutated.c_str());
    V1Profile loaded;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::Corrupt),
                          static_cast<int>(manager.loadProfileResult("Road", loaded).status));

    std::string oversized(PROFILE_SYNC_META_MAX_BYTES + 1, 'x');
    writeFileFromString(fs, "/v1profiles/Road.json.meta", oversized.c_str());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::Corrupt),
                          static_cast<int>(manager.loadProfileResult("Road", loaded).status));
}

void test_valid_tombstone_remains_authoritative_when_json_removal_was_incomplete() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));
    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("Road", 30, "deleted")).success);
    const std::string staleJson = readFileToString(fs, "/v1profiles/Road.json");
    TEST_ASSERT_TRUE(manager.deleteProfileResult("Road").success());
    writeFileFromString(fs, "/v1profiles/Road.json", staleJson.c_str());

    V1Profile loaded;
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::NotFound),
                          static_cast<int>(manager.loadProfileResult("Road", loaded).status));
    TEST_ASSERT_TRUE(manager.listProfilesResult().genuinelyEmpty);
}

void test_valid_profile_metadata_mirror_repairs_corrupt_primary_and_legacy_is_rewritten() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd";
    const std::filesystem::path littleRoot = g_tempRoot / "little";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);
    StorageManager storage;
    storage.setFilesystem(&sd, true);
    storage.setLittleFS(&little);

    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(storage));
    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("Road", 40, "mirror")).success);
    writeFileFromString(sd, "/v1profiles/Road.json.meta", "{}");

    V1ProfileManager rebooted;
    TEST_ASSERT_TRUE(rebooted.begin(storage));
    V1Profile loaded;
    TEST_ASSERT_TRUE(rebooted.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("mirror", loaded.description.c_str());
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileSyncState::Status::Current),
                          static_cast<int>(readSyncState(sd, "/v1profiles/Road.json").status));

    writeFileFromString(sd, "/v1profiles/Road.json.meta", "{\"version\":7,\"deleted\":false}");
    storage.setLittleFS(nullptr);
    V1ProfileManager legacy;
    TEST_ASSERT_TRUE(legacy.begin(storage));
    TEST_ASSERT_TRUE(legacy.loadProfile("Road", loaded));
    TEST_ASSERT_TRUE(legacy.saveProfile(makeProfile("Road", 50, "legacy-rewritten")).success);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileSyncState::Status::Current),
                          static_cast<int>(readSyncState(sd, "/v1profiles/Road.json").status));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_save_profile_short_write_new_file_leaves_no_live_json);
    RUN_TEST(test_save_profile_short_write_existing_file_preserves_previous_profile);
    RUN_TEST(test_save_profile_normal_path_still_succeeds);
    RUN_TEST(test_save_requires_final_file_reopen_and_crc_validation);
    RUN_TEST(test_load_profile_rejects_invalid_raw_bytes_without_mutating_output);
    RUN_TEST(test_json_to_settings_rejects_invalid_raw_bytes_without_mutating_output);
    RUN_TEST(test_v41039_photo_settings_round_trip_through_json);
    RUN_TEST(test_rename_same_name_is_successful_noop);
    RUN_TEST(test_path_like_name_is_rejected_without_creating_a_profile);
    RUN_TEST(test_profile_name_contract_rejects_hidden_long_blank_and_canonical_collisions);
    RUN_TEST(test_sd_contention_returns_busy_for_every_profile_transaction);
    RUN_TEST(test_littlefs_fallback_edits_and_deletion_reconcile_without_resurrection);
    RUN_TEST(test_rename_existing_distinct_destination_fails_without_mutation);
    RUN_TEST(test_rename_normal_path_succeeds_and_advances_revision);
    RUN_TEST(test_equal_generation_divergence_converges_to_offline_fallback_edit);
    RUN_TEST(test_corrupt_newer_profile_cannot_replace_valid_older_mirror);
    RUN_TEST(test_profile_sync_metadata_short_write_is_rejected);
    RUN_TEST(test_secondary_profile_save_failure_is_reported_and_rolls_back_both_copies);
    RUN_TEST(test_secondary_tombstone_failure_is_reported_and_preserves_profile);
    RUN_TEST(test_existing_malformed_sync_metadata_fails_closed_without_resurrecting_json);
    RUN_TEST(test_sync_metadata_crc_mutation_and_oversize_fail_closed);
    RUN_TEST(test_valid_tombstone_remains_authoritative_when_json_removal_was_incomplete);
    RUN_TEST(test_valid_profile_metadata_mirror_repairs_corrupt_primary_and_legacy_is_rewritten);
    return UNITY_END();
}
