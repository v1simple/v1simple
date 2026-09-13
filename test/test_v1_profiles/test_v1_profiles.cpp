#include <unity.h>

#include <cstring>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
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

std::map<std::string, std::string> filesSnapshot() {
    std::map<std::string, std::string> result;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(g_tempRoot)) {
        if (!entry.is_regular_file()) continue;
        std::ifstream file(entry.path(), std::ios::binary);
        result[entry.path().lexically_relative(g_tempRoot).string()] =
            std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
    return result;
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
    mock_reset_heap_caps();
    g_tempRoot = nextTempRoot();
    std::filesystem::remove_all(g_tempRoot);
    std::filesystem::create_directories(g_tempRoot);
}

void test_cursor_pages_keep_grandfathered_over_limit_catalog_discoverable_and_prunable() {
    fs::FS catalogFs(g_tempRoot / "catalog");
    std::filesystem::create_directories(g_tempRoot / "catalog");
    TEST_ASSERT_TRUE(catalogFs.mkdir("/v1profiles"));
    for (int index = 11; index >= 0; --index) {
        const String name = String("P") + (index < 10 ? "0" : "") + String(index);
        const std::filesystem::path seedRoot = g_tempRoot / ("seed_" + std::to_string(index));
        std::filesystem::create_directories(seedRoot);
        fs::FS seedFs(seedRoot);
        V1ProfileManager seed;
        TEST_ASSERT_TRUE(seed.begin(&seedFs));
        TEST_ASSERT_TRUE(seed.saveProfile(makeProfile(name, static_cast<uint8_t>(index), "legacy")).success);
        writeFileFromString(catalogFs, (String("/v1profiles/") + name + ".json").c_str(),
                            readFileToString(seedFs, (String("/v1profiles/") + name + ".json").c_str()).c_str());
        writeFileFromString(catalogFs, (String("/v1profiles/") + name + ".json.meta").c_str(),
                            readFileToString(seedFs, (String("/v1profiles/") + name + ".json.meta").c_str()).c_str());
    }

    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&catalogFs));
    const ProfileListResult complete = manager.listProfilesResult();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::IoError),
                          static_cast<int>(complete.status));
    TEST_ASSERT_EQUAL_UINT(0u, complete.profiles.size());
    String cursor;
    size_t seen = 0;
    do {
        const ProfilePageResult page = manager.listProfilesPageResult(cursor, 3);
        TEST_ASSERT_TRUE(page.success());
        TEST_ASSERT_EQUAL_UINT(12u, page.total);
        TEST_ASSERT_GREATER_THAN(0u, page.profiles.size());
        for (const String& name : page.profiles) {
            if (cursor.length() > 0) TEST_ASSERT_TRUE(std::strcmp(name.c_str(), cursor.c_str()) > 0);
            cursor = name;
            ++seen;
        }
        if (!page.hasMore) break;
        TEST_ASSERT_EQUAL_STRING(cursor.c_str(), page.nextCursor.c_str());
    } while (seen < 20u);
    TEST_ASSERT_EQUAL_UINT(12u, seen);
    TEST_ASSERT_TRUE(manager.deleteProfileResult("P11").success());
    TEST_ASSERT_EQUAL_UINT(11u, manager.listProfilesPageResult("", 3).total);
}

void test_reconcile_union_over_catalog_bound_is_bounded_and_preserves_both_stores() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd_over_cap_union";
    const std::filesystem::path littleRoot = g_tempRoot / "little_over_cap_union";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);
    V1ProfileManager sdOnly;
    V1ProfileManager littleOnly;
    TEST_ASSERT_TRUE(sdOnly.begin(&sd));
    TEST_ASSERT_TRUE(littleOnly.begin(&little));
    for (int index = 0; index < 6; ++index) {
        const String sdName = String("SD") + String(index);
        const String littleName = String("LF") + String(index);
        TEST_ASSERT_TRUE(sdOnly.saveProfile(makeProfile(sdName, static_cast<uint8_t>(index), "sd")).success);
        TEST_ASSERT_TRUE(littleOnly.saveProfile(
            makeProfile(littleName, static_cast<uint8_t>(index + 10), "little")).success);
    }
    const std::string sdSentinel = readFileToString(sd, "/v1profiles/SD0.json");
    const std::string littleSentinel = readFileToString(little, "/v1profiles/LF0.json");

    StorageManager storage;
    storage.setFilesystem(&sd, true);
    storage.setLittleFS(&little);
    V1ProfileManager combined;
    TEST_ASSERT_TRUE(combined.begin(storage));

    TEST_ASSERT_EQUAL_STRING(sdSentinel.c_str(), readFileToString(sd, "/v1profiles/SD0.json").c_str());
    TEST_ASSERT_EQUAL_STRING(littleSentinel.c_str(),
                             readFileToString(little, "/v1profiles/LF0.json").c_str());
    TEST_ASSERT_FALSE(sd.exists("/v1profiles/LF0.json"));
    TEST_ASSERT_FALSE(little.exists("/v1profiles/SD0.json"));
    TEST_ASSERT_EQUAL_UINT(6u, combined.listProfilesPageResult("", 10).total);
 }

void test_profile_sync_generation_exhaustion_preserves_live_profile_and_tombstone_state() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));
    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("Road", 10, "original")).success);

    ProfileSyncState exhausted;
    exhausted.version = std::numeric_limits<uint32_t>::max();
    exhausted.deleted = false;
    TEST_ASSERT_TRUE(writeSyncState(fs, "/v1profiles/Road.json", exhausted));
    const std::string originalJson = readFileToString(fs, "/v1profiles/Road.json");
    const std::string originalMeta = readFileToString(fs, "/v1profiles/Road.json.meta");

    const ProfileSaveResult saved = manager.saveProfile(makeProfile("Road", 20, "replacement"));
    TEST_ASSERT_FALSE(saved.success);
    TEST_ASSERT_TRUE(saved.error.indexOf("generation exhausted") >= 0);
    const ProfileOperationResult deleted = manager.deleteProfileResult("Road");
    TEST_ASSERT_FALSE(deleted.success());
    TEST_ASSERT_TRUE(deleted.error.indexOf("generation exhausted") >= 0);
    TEST_ASSERT_EQUAL_STRING(originalJson.c_str(), readFileToString(fs, "/v1profiles/Road.json").c_str());
    TEST_ASSERT_EQUAL_STRING(originalMeta.c_str(), readFileToString(fs, "/v1profiles/Road.json.meta").c_str());

    V1Profile loaded;
    TEST_ASSERT_TRUE(manager.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("original", loaded.description.c_str());
}

void test_profile_reconcile_generation_exhaustion_does_not_choose_or_overwrite_either_copy() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd_max_generation";
    const std::filesystem::path littleRoot = g_tempRoot / "little_max_generation";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);
    V1ProfileManager sdOnly;
    V1ProfileManager littleOnly;
    TEST_ASSERT_TRUE(sdOnly.begin(&sd));
    TEST_ASSERT_TRUE(littleOnly.begin(&little));
    TEST_ASSERT_TRUE(sdOnly.saveProfile(makeProfile("Road", 10, "sd-copy")).success);
    TEST_ASSERT_TRUE(littleOnly.saveProfile(makeProfile("Road", 20, "little-copy")).success);
    ProfileSyncState exhausted;
    exhausted.version = std::numeric_limits<uint32_t>::max();
    exhausted.deleted = false;
    TEST_ASSERT_TRUE(writeSyncState(sd, "/v1profiles/Road.json", exhausted));
    TEST_ASSERT_TRUE(writeSyncState(little, "/v1profiles/Road.json", exhausted));
    const std::string sdJson = readFileToString(sd, "/v1profiles/Road.json");
    const std::string sdMeta = readFileToString(sd, "/v1profiles/Road.json.meta");
    const std::string littleJson = readFileToString(little, "/v1profiles/Road.json");
    const std::string littleMeta = readFileToString(little, "/v1profiles/Road.json.meta");

    StorageManager storage;
    storage.setFilesystem(&sd, true);
    storage.setLittleFS(&little);
    V1ProfileManager combined;
    TEST_ASSERT_TRUE(combined.begin(storage));
    TEST_ASSERT_EQUAL_STRING(sdJson.c_str(), readFileToString(sd, "/v1profiles/Road.json").c_str());
    TEST_ASSERT_EQUAL_STRING(sdMeta.c_str(), readFileToString(sd, "/v1profiles/Road.json.meta").c_str());
    TEST_ASSERT_EQUAL_STRING(littleJson.c_str(), readFileToString(little, "/v1profiles/Road.json").c_str());
    TEST_ASSERT_EQUAL_STRING(littleMeta.c_str(), readFileToString(little, "/v1profiles/Road.json.meta").c_str());
}

void test_interrupted_save_recovery_uses_bounded_scans_and_never_replaces_live_profile() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager seed;
    TEST_ASSERT_TRUE(seed.begin(&fs));
    TEST_ASSERT_TRUE(seed.saveProfile(makeProfile("Road", 10, "live")).success);
    const std::string liveJson = readFileToString(fs, "/v1profiles/Road.json");
    writeFileFromString(fs, "/v1profiles/Road.json.bak", "stale-backup");
    for (int index = 0; index < 64; ++index) {
        const String path = String("/v1profiles/T") + String(index) + ".json.tmp";
        writeFileFromString(fs, path.c_str(), "partial");
    }

    V1ProfileManager recovered;
    TEST_ASSERT_TRUE(recovered.begin(&fs));
    TEST_ASSERT_EQUAL_STRING(liveJson.c_str(), readFileToString(fs, "/v1profiles/Road.json").c_str());
    TEST_ASSERT_TRUE(fs.exists("/v1profiles/Road.json.bak"));
    TEST_ASSERT_EQUAL_UINT(0u, countFilesInProfileDir(".tmp"));
    V1Profile loaded;
    TEST_ASSERT_TRUE(recovered.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("live", loaded.description.c_str());
}

void test_reconcile_psram_unavailable_never_overwrites_newer_profile_or_tombstone() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd_oom";
    const std::filesystem::path littleRoot = g_tempRoot / "little_oom";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);
    V1ProfileManager sdOnly;
    V1ProfileManager littleOnly;
    TEST_ASSERT_TRUE(sdOnly.begin(&sd));
    TEST_ASSERT_TRUE(littleOnly.begin(&little));
    TEST_ASSERT_TRUE(sdOnly.saveProfile(makeProfile("Road", 1, "older")).success);
    TEST_ASSERT_TRUE(littleOnly.saveProfile(makeProfile("Road", 2, "newer")).success);
    TEST_ASSERT_TRUE(littleOnly.saveProfile(makeProfile("Road", 3, "newest")).success);
    const std::string sdJson = readFileToString(sd, "/v1profiles/Road.json");
    const std::string sdMeta = readFileToString(sd, "/v1profiles/Road.json.meta");
    const std::string littleJson = readFileToString(little, "/v1profiles/Road.json");
    const std::string littleMeta = readFileToString(little, "/v1profiles/Road.json.meta");

    StorageManager storage;
    storage.setFilesystem(&sd, true);
    storage.setLittleFS(&little);
    mock_reset_heap_caps_tracking();
    g_mock_heap_caps_fail_all_allocations = true;
    V1ProfileManager combined;
    TEST_ASSERT_TRUE(combined.begin(storage));
    g_mock_heap_caps_fail_all_allocations = false;

    TEST_ASSERT_EQUAL_STRING(sdJson.c_str(), readFileToString(sd, "/v1profiles/Road.json").c_str());
    TEST_ASSERT_EQUAL_STRING(sdMeta.c_str(), readFileToString(sd, "/v1profiles/Road.json.meta").c_str());
    TEST_ASSERT_EQUAL_STRING(littleJson.c_str(), readFileToString(little, "/v1profiles/Road.json").c_str());
    TEST_ASSERT_EQUAL_STRING(littleMeta.c_str(), readFileToString(little, "/v1profiles/Road.json.meta").c_str());
}

void test_secondary_rollback_path_allocation_failure_precedes_every_store_mutation() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd_syncbak_oom";
    const std::filesystem::path littleRoot = g_tempRoot / "little_syncbak_oom";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);
    StorageManager storage;
    storage.setFilesystem(&sd, true);
    storage.setLittleFS(&little);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(storage));

    g_failProfilePathSuffixForTest = ".syncbak";
    const auto beforeNew = filesSnapshot();
    TEST_ASSERT_FALSE(manager.saveProfile(makeProfile("New", 1, "new")).success);
    TEST_ASSERT_TRUE(beforeNew == filesSnapshot());
    g_failProfilePathSuffixForTest = nullptr;

    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("Road", 2, "before")).success);
    const auto beforeExisting = filesSnapshot();
    g_failProfilePathSuffixForTest = ".syncbak";
    TEST_ASSERT_FALSE(manager.saveProfile(makeProfile("Road", 3, "after")).success);
    g_failProfilePathSuffixForTest = nullptr;
    TEST_ASSERT_TRUE(beforeExisting == filesSnapshot());
    V1Profile loaded;
    TEST_ASSERT_TRUE(manager.loadProfile("Road", loaded));
    TEST_ASSERT_EQUAL_STRING("before", loaded.description.c_str());
}

void tearDown() {
    g_failProfilePathSuffixForTest = nullptr;
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

void test_maximum_description_and_64_definitions_fit_but_one_extra_byte_preserves_live_profile() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    const String maximumName(std::string(MAX_PROFILE_NAME_LEN, 'M'));
    V1Profile maximum = makeProfile(maximumName.c_str(), 0xFF,
                                    String(std::string(V1_PROFILE_DESCRIPTION_MAX_BYTES, '\"')));
    const uint8_t maximumRaw[] = {192, 217, 252, 255, 255, 255};
    memcpy(maximum.settings.bytes, maximumRaw, sizeof(maximumRaw));
    maximum.detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    maximum.detector.modePolicy = V1ModePolicy::Value;
    maximum.detector.mode = 3;
    maximum.detector.displayPolicy = V1DisplayPolicy::Unchanged;
    maximum.detector.volumePolicy = V1VolumePolicy::Temporary;
    maximum.detector.mainVolume = 9;
    maximum.detector.mutedVolume = 9;
    maximum.detector.volumeFeedback = V1VolumeFeedbackPolicy::ChangedOnly;
    maximum.detector.volumeDisconnect = V1VolumeDisconnectPolicy::RestoreSaved;
    maximum.detector.bluetoothLedPolicy = V1BluetoothLedPolicy::Unchanged;
    maximum.detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    for (uint8_t index = 0; index < 64; ++index) {
        maximum.detector.customFrequencyDefinitions.push_back({index, 65534, 65535});
    }
    const String compactRequest = manager.profileToJson(maximum);
    TEST_ASSERT_EQUAL_UINT(12195, compactRequest.length());
    TEST_ASSERT_EQUAL_UINT(4189, V1_PROFILE_HTTP_SAVE_MAX_BYTES - compactRequest.length());
    TEST_ASSERT_LESS_THAN(V1_PROFILE_HTTP_SAVE_MAX_BYTES, compactRequest.length());
    TEST_ASSERT_TRUE(manager.saveProfile(maximum).success);
    const String path = String("/v1profiles/") + maximumName + ".json";
    const std::string before = readFileToString(fs, path.c_str());
    TEST_ASSERT_EQUAL_UINT(16415, before.size());
    TEST_ASSERT_EQUAL_UINT(8161, V1_PROFILE_FILE_MAX_BYTES - before.size());
    TEST_ASSERT_LESS_THAN(V1_PROFILE_FILE_MAX_BYTES, before.size());

    maximum.description = String(std::string(V1_PROFILE_DESCRIPTION_MAX_BYTES + 1u, 'x'));
    const ProfileSaveResult rejected = manager.saveProfile(maximum);
    TEST_ASSERT_FALSE(rejected.success);
    TEST_ASSERT_TRUE(rejected.error.indexOf("4096") >= 0);
    TEST_ASSERT_EQUAL_STRING(before.c_str(), readFileToString(fs, path.c_str()).c_str());
    TEST_ASSERT_FALSE(fs.exists(path + ".tmp"));

    V1Profile loaded;
    TEST_ASSERT_TRUE(manager.loadProfile(maximumName, loaded));
    TEST_ASSERT_EQUAL_UINT32(V1_PROFILE_DESCRIPTION_MAX_BYTES, loaded.description.length());
    TEST_ASSERT_EQUAL_UINT32(64, static_cast<uint32_t>(loaded.detector.customFrequencyDefinitions.size()));

    // The file reader admits the exact published cap (JSON permits trailing
    // whitespace) and rejects the next byte before replacing caller output.
    std::string atFileCap = before;
    atFileCap.append(V1_PROFILE_FILE_MAX_BYTES - atFileCap.size(), ' ');
    writeFileFromString(fs, path.c_str(), atFileCap.c_str());
    V1Profile capLoaded;
    TEST_ASSERT_TRUE(manager.loadProfile(maximumName, capLoaded));
    TEST_ASSERT_EQUAL_UINT32(V1_PROFILE_DESCRIPTION_MAX_BYTES, capLoaded.description.length());

    std::string aboveFileCap = atFileCap;
    aboveFileCap.push_back(' ');
    writeFileFromString(fs, path.c_str(), aboveFileCap.c_str());
    V1Profile untouched("Sentinel");
    untouched.description = "untouched";
    TEST_ASSERT_FALSE(manager.loadProfile(maximumName, untouched));
    TEST_ASSERT_EQUAL_STRING("Sentinel", untouched.name.c_str());
    TEST_ASSERT_EQUAL_STRING("untouched", untouched.description.c_str());
}

void test_schema_v3_detector_policy_round_trips_with_authoritative_raw_bytes() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));
    V1Profile profile = makeProfile("Policy", 0x91, "detector policy");
    profile.detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    profile.detector.modePolicy = V1ModePolicy::Value;
    profile.detector.mode = 3;
    profile.detector.displayPolicy = V1DisplayPolicy::Off;
    profile.detector.volumePolicy = V1VolumePolicy::Saved;
    profile.detector.mainVolume = 9;
    profile.detector.mutedVolume = 0;
    profile.detector.volumeFeedback = V1VolumeFeedbackPolicy::Always;
    profile.detector.volumeDisconnect = V1VolumeDisconnectPolicy::RestoreSaved;
    profile.detector.bluetoothLedPolicy = V1BluetoothLedPolicy::On;
    profile.detector.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    const std::array<V1CustomFrequencyDefinition, 2> profileDefinitions{{
        {0, 24050, 24150}, {1, 0, 0}}};
    TEST_ASSERT_TRUE(profile.detector.customFrequencyDefinitions.assign(profileDefinitions));
    TEST_ASSERT_TRUE(manager.saveProfile(profile).success);

    V1Profile loaded;
    TEST_ASSERT_TRUE(manager.loadProfile("Policy", loaded));
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_SCHEMA_VERSION, loaded.schemaVersion);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(profile.settings.bytes, loaded.settings.bytes, 6);
    TEST_ASSERT_TRUE(profile.detector == loaded.detector);

    JsonDocument api;
    const String apiJson = manager.profileToJson(loaded);
    TEST_ASSERT_FALSE(deserializeJson(api, apiJson));
    TEST_ASSERT_EQUAL_INT(3, api["schemaVersion"].as<int>());
    TEST_ASSERT_EQUAL_STRING("unchanged", api["detector"]["userSettings"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("saved", api["detector"]["volume"]["policy"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("always", api["detector"]["volume"]["feedback"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("on", api["detector"]["bluetoothLed"].as<const char*>());
    TEST_ASSERT_EQUAL_UINT32(2, api["detector"]["customFrequencies"]["definitions"].size());
    TEST_ASSERT_EQUAL_UINT8(0x91, api["settings"]["bytes"][0].as<uint8_t>());
}

void test_detector_configuration_rejects_half_zero_sweep_without_mutating_output() {
    V1DetectorConfiguration valid;
    valid.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
    const std::array<V1CustomFrequencyDefinition, 1> validDefinitions{{{0, 24000, 24100}}};
    TEST_ASSERT_TRUE(valid.customFrequencyDefinitions.assign(validDefinitions));
    JsonDocument doc;
    appendV1DetectorConfiguration(doc.to<JsonObject>(), valid);
    doc["customFrequencies"]["definitions"][0]["lowerMHz"] = 0;
    doc["customFrequencies"]["definitions"][0]["upperMHz"] = 1;
    V1DetectorConfiguration output;
    output.modePolicy = V1ModePolicy::Value;
    output.mode = 2;
    const V1DetectorConfiguration before = output;
    TEST_ASSERT_FALSE(parseV1DetectorConfiguration(doc.as<JsonObjectConst>(), output));
    TEST_ASSERT_TRUE(output == before);
}

void test_genuine_schema_v2_profile_migrates_without_reinterpretation() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));
    V1DetectorConfiguration detector;
    detector.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    detector.modePolicy = V1ModePolicy::Value;
    detector.mode = 2;
    detector.displayPolicy = V1DisplayPolicy::Off;
    detector.volumePolicy = V1VolumePolicy::Temporary;
    detector.mainVolume = 7;
    detector.mutedVolume = 2;
    const uint8_t bytes[] = {0xBF, 0xE1, 0xF2, 0x73, 0xA5, 0x5A};
    JsonDocument v2;
    v2["schemaVersion"] = V1_PROFILE_PREVIOUS_SCHEMA_VERSION;
    v2["name"] = "V2 Fixture";
    v2["description"] = "genuine prior shape";
    appendV1DetectorConfigurationV2(v2["detector"].to<JsonObject>(), detector);
    uint32_t detectorCrc = 0;
    TEST_ASSERT_TRUE(detectorConfigurationCrc(detector, detectorCrc, V1_PROFILE_PREVIOUS_SCHEMA_VERSION));
    v2["detectorCrc32"] = detectorCrc;
    JsonArray raw = v2["bytes"].to<JsonArray>();
    for (uint8_t byte : bytes) raw.add(byte);
    v2["crc32"] = computeCrc32(bytes, sizeof(bytes));
    String serialized;
    serializeJson(v2, serialized);
    writeFileFromString(fs, "/v1profiles/V2 Fixture.json", serialized.c_str());

    V1Profile loaded;
    TEST_ASSERT_TRUE(manager.loadProfile("V2 Fixture", loaded));
    TEST_ASSERT_EQUAL_UINT8(V1_PROFILE_SCHEMA_VERSION, loaded.schemaVersion);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(bytes, loaded.settings.bytes, 6);
    TEST_ASSERT_EQUAL_INT(V1DisplayPolicy::Off, loaded.detector.displayPolicy);
    TEST_ASSERT_EQUAL_INT(V1BluetoothLedPolicy::Off, loaded.detector.bluetoothLedPolicy);
    TEST_ASSERT_EQUAL_INT(V1VolumePolicy::Temporary, loaded.detector.volumePolicy);
    TEST_ASSERT_EQUAL_INT(V1VolumeFeedbackPolicy::None, loaded.detector.volumeFeedback);
    TEST_ASSERT_EQUAL_INT(V1VolumeDisconnectPolicy::RestoreSaved, loaded.detector.volumeDisconnect);
    TEST_ASSERT_EQUAL_INT(V1CustomFrequencyPolicy::Unchanged, loaded.detector.customFrequencyPolicy);

    V1UserSettings authoritative;
    std::memcpy(authoritative.bytes, bytes, sizeof(bytes));
    v2["xBand"] = !authoritative.xBandEnabled();
    serialized = "";
    serializeJson(v2, serialized);
    writeFileFromString(fs, "/v1profiles/V2 Fixture.json", serialized.c_str());
    V1Profile unchanged = makeProfile("Sentinel", 90, "unchanged");
    const V1Profile before = unchanged;
    TEST_ASSERT_FALSE(manager.loadProfile("V2 Fixture", unchanged));
    TEST_ASSERT_EQUAL_STRING(before.name.c_str(), unchanged.name.c_str());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(before.settings.bytes, unchanged.settings.bytes, 6);
}

void test_schema_v3_rejects_crc_valid_unknown_missing_or_conflicting_readable_fields() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));
    const V1Profile profile = makeProfile("StrictReadable", 0xA0, "strict readable");
    TEST_ASSERT_TRUE(manager.saveProfile(profile).success);
    const std::string original = readFileToString(fs, "/v1profiles/StrictReadable.json");

    const auto rejects = [&](const std::function<void(JsonDocument&)>& mutate) {
        JsonDocument broken;
        TEST_ASSERT_FALSE(deserializeJson(broken, original.c_str()));
        mutate(broken);
        broken.remove("profileCrc32");
        uint32_t crc = 0;
        TEST_ASSERT_TRUE(profileDocumentCrc(broken, crc));
        broken["profileCrc32"] = crc;
        String text;
        serializeJson(broken, text);
        writeFileFromString(fs, "/v1profiles/StrictReadable.json", text.c_str());
        V1Profile output = makeProfile("Sentinel", 90, "unchanged");
        const V1Profile before = output;
        TEST_ASSERT_FALSE(manager.loadProfile("StrictReadable", output));
        TEST_ASSERT_EQUAL_STRING(before.name.c_str(), output.name.c_str());
        TEST_ASSERT_EQUAL_STRING(before.description.c_str(), output.description.c_str());
        TEST_ASSERT_EQUAL_UINT8_ARRAY(before.settings.bytes, output.settings.bytes, 6);
    };

    rejects([](JsonDocument& doc) { doc["xBand"] = !doc["xBand"].as<bool>(); });
    rejects([](JsonDocument& doc) { doc["xBand"] = "true"; });
    rejects([](JsonDocument& doc) { doc.remove("xBand"); });
    rejects([](JsonDocument& doc) { doc["xBnnd"] = true; });
}

void test_schema_v2_mixed_markers_crc_omissions_and_malformed_policy_reject_atomically() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));
    V1Profile profile = makeProfile("Strict", 0xA0, "strict");
    profile.detector.modePolicy = V1ModePolicy::Value;
    profile.detector.mode = 2;
    profile.detector.displayPolicy = V1DisplayPolicy::On;
    profile.detector.volumePolicy = V1VolumePolicy::Temporary;
    profile.detector.mainVolume = 0;
    profile.detector.mutedVolume = 0;
    TEST_ASSERT_TRUE(manager.saveProfile(profile).success);
    const std::string original = readFileToString(fs, "/v1profiles/Strict.json");

    const auto rejects = [&](const std::function<void(JsonDocument&)>& mutate) {
        JsonDocument broken;
        TEST_ASSERT_FALSE(deserializeJson(broken, original.c_str()));
        mutate(broken);
        String text;
        serializeJson(broken, text);
        writeFileFromString(fs, "/v1profiles/Strict.json", text.c_str());
        V1Profile output = makeProfile("Sentinel", 0x30, "unchanged");
        const V1Profile before = output;
        const ProfileOperationResult result = manager.loadProfileResult("Strict", output);
        TEST_ASSERT_EQUAL_INT(ProfileStorageStatus::Corrupt, result.status);
        TEST_ASSERT_EQUAL_STRING(before.name.c_str(), output.name.c_str());
        TEST_ASSERT_EQUAL_STRING(before.description.c_str(), output.description.c_str());
        TEST_ASSERT_EQUAL_UINT8_ARRAY(before.settings.bytes, output.settings.bytes, 6);
        writeFileFromString(fs, "/v1profiles/Strict.json", original.c_str());
    };

    rejects([](JsonDocument& d) { d.remove("schemaVersion"); });
    rejects([](JsonDocument& d) { d.remove("detector"); });
    rejects([](JsonDocument& d) { d.remove("detectorCrc32"); });
    rejects([](JsonDocument& d) { d.remove("bytes"); });
    rejects([](JsonDocument& d) { d.remove("crc32"); });
    rejects([](JsonDocument& d) { d["schemaVersion"] = 1; });
    rejects([](JsonDocument& d) { d["detector"]["display"] = "off"; });
    rejects([](JsonDocument& d) { d["detector"]["bluetoothLed"] = "on"; });
    rejects([](JsonDocument& d) { d["detector"]["volume"].remove("muted"); });

    V1Profile intact;
    TEST_ASSERT_TRUE(manager.loadProfile("Strict", intact));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(profile.settings.bytes, intact.settings.bytes, 6);
    TEST_ASSERT_TRUE(profile.detector == intact.detector);
}

void test_exact_legacy_profile_without_schema_or_crc_remains_backward_readable() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));
    writeFileFromString(fs, "/v1profiles/Legacy.json",
                        "{\"name\":\"Legacy\",\"description\":\"pre-v2\",\"displayOn\":false,"
                        "\"mainVolume\":7,\"mutedVolume\":2,\"bytes\":[191,225,146,115,165,90]}");
    V1Profile loaded;
    TEST_ASSERT_TRUE(manager.loadProfile("Legacy", loaded));
    TEST_ASSERT_EQUAL_UINT8(1, loaded.schemaVersion);
    TEST_ASSERT_FALSE(loaded.displayOn);
    TEST_ASSERT_EQUAL_UINT8(7, loaded.mainVolume);
    TEST_ASSERT_EQUAL_UINT8(2, loaded.mutedVolume);
    const uint8_t expected[] = {191, 225, 146, 115, 165, 90};
    TEST_ASSERT_EQUAL_UINT8_ARRAY(expected, loaded.settings.bytes, 6);
}

void test_persisted_legacy_profile_shape_is_strict_and_never_mutates_output() {
    const char* invalidProfiles[] = {
        "{\"name\":\"Legacy\",\"description\":\"bad\",\"mainVolume\":7,\"mutedVolume\":2,\"bytes\":[0,0,0,0,0,0]}",
        "{\"name\":\"Legacy\",\"description\":\"bad\",\"displayOn\":\"false\",\"mainVolume\":7,\"mutedVolume\":2,\"bytes\":[0,0,0,0,0,0]}",
        "{\"name\":\"Legacy\",\"description\":\"bad\",\"displayOn\":false,\"mainVolume\":10,\"mutedVolume\":2,\"bytes\":[0,0,0,0,0,0]}",
        "{\"name\":\"Legacy\",\"description\":\"bad\",\"displayOn\":false,\"mainVolume\":7,\"mutedVolume\":-1,\"bytes\":[0,0,0,0,0,0]}",
        "{\"name\":\"Legacy\",\"description\":\"bad\",\"displayOn\":false,\"mainVolume\":7,\"mutedVolume\":2}",
        "{\"name\":\"Legacy\",\"description\":\"bad\",\"displayOn\":false,\"mainVolume\":7,\"mutedVolume\":2,\"crc32\":0}",
        "{\"name\":\"Legacy\",\"description\":\"bad\",\"displayOn\":false,\"mainVolume\":7,\"mutedVolume\":2,\"bytes\":[0,0,0,0,0,0],\"crc32\":\"0\"}",
        "{\"name\":\"Legacy\",\"description\":\"bad\",\"displayOn\":false,\"mainVolume\":7,\"mutedVolume\":2,\"bytes\":[0,0,0,0,0,0],\"xBand\":\"true\"}",
        "{\"name\":\"Legacy\",\"description\":\"bad\",\"displayOn\":false,\"mainVolume\":7,\"mutedVolume\":2,\"bytes\":[0,0,0,0,0,0],\"xBand\":true}",
        "{\"name\":\"Legacy\",\"description\":\"bad\",\"displayOn\":false,\"mainVolume\":7,\"mutedVolume\":2,\"bytes\":[0,0,0,0,0,0],\"mystery\":true}",
    };

    for (const char* profileJson : invalidProfiles) {
        fs::FS fs(g_tempRoot);
        fs.mkdir("/v1profiles");
        writeFileFromString(fs, "/v1profiles/Legacy.json", profileJson);
        V1ProfileManager manager;
        TEST_ASSERT_TRUE(manager.begin(&fs));
        V1Profile output = makeProfile("Sentinel", 90, "unchanged");
        const V1Profile before = output;
        TEST_ASSERT_FALSE_MESSAGE(manager.loadProfile("Legacy", output), profileJson);
        TEST_ASSERT_EQUAL_STRING(before.name.c_str(), output.name.c_str());
        TEST_ASSERT_EQUAL_STRING(before.description.c_str(), output.description.c_str());
        TEST_ASSERT_EQUAL_UINT8_ARRAY(before.settings.bytes, output.settings.bytes, 6);
        std::filesystem::remove_all(g_tempRoot);
        g_tempRoot = nextTempRoot();
        std::filesystem::create_directories(g_tempRoot);
    }
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

void test_json_to_settings_strictly_validates_human_readable_fields_without_mutation() {
    const char* booleanFields[] = {
        "xBand",          "kBand",           "kaBand",          "laser",
        "kuBand",         "euro",            "kVerifier",       "laserRear",
        "customFreqs",    "kaAlwaysPriority", "fastLaserDetect", "muteToMuteVolume",
        "bogeyLockLoud",  "muteXKRear",       "startupSequence", "restingDisplay",
        "bsmPlus",         "mrct",             "driveSafe3D",      "driveSafe3DHD",
        "redflexHalo",     "redflexNK7",       "ekin",             "photoVerifier",
        "gatsoRT4",        "photoIntersectionFilter",
    };
    const char* enumFields[] = {"kaSensitivity", "kSensitivity", "xSensitivity", "autoMute"};

    V1ProfileManager manager;
    for (const char* field : booleanFields) {
        const std::string invalid = std::string("{\"") + field + "\":1}";
        V1UserSettings settings = makeProfile("Sentinel", 100).settings;
        const V1UserSettings before = settings;
        TEST_ASSERT_FALSE_MESSAGE(manager.jsonToSettings(String(invalid.c_str()), settings), field);
        TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(before.bytes, settings.bytes, 6, field);
    }

    for (const char* field : enumFields) {
        const char* invalidValues[] = {"0", "4", "true", "null", "1.5", "\"2\""};
        for (const char* value : invalidValues) {
            const std::string invalid = std::string("{\"") + field + "\":" + value + ",\"xBand\":true}";
            V1UserSettings settings = makeProfile("Sentinel", 100).settings;
            const V1UserSettings before = settings;
            TEST_ASSERT_FALSE_MESSAGE(manager.jsonToSettings(String(invalid.c_str()), settings), field);
            TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(before.bytes, settings.bytes, 6, field);
        }
    }

    V1UserSettings typo = makeProfile("Sentinel", 100).settings;
    const V1UserSettings typoBefore = typo;
    TEST_ASSERT_FALSE(manager.jsonToSettings(String("{\"xBand\":false,\"xBnnd\":true}"), typo));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(typoBefore.bytes, typo.bytes, 6);

    V1UserSettings valid;
    TEST_ASSERT_TRUE(manager.jsonToSettings(
        String("{\"muteToMuteVolume\":true,\"kaSensitivity\":1,\"kSensitivity\":2,"
               "\"xSensitivity\":3,\"autoMute\":2}"), valid));
    TEST_ASSERT_TRUE(valid.muteToMuteVolume());
    TEST_ASSERT_EQUAL_UINT8(1, valid.kaSensitivity());
    TEST_ASSERT_EQUAL_UINT8(2, valid.kSensitivity());
    TEST_ASSERT_EQUAL_UINT8(3, valid.xSensitivity());
    TEST_ASSERT_EQUAL_UINT8(2, valid.autoMute());
}

void test_conflicting_legacy_human_metadata_is_rejected_without_rewriting_raw_bytes() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    // Reproduce a profile written before the accessor fix: the canonical raw
    // byte says "mute to muted volume" while its redundant readable field says
    // false. The CRC remains valid because it protects the raw bytes.
    const uint8_t rawBytes[6] = {0x10, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    JsonDocument preFix;
    preFix["name"] = "Existing";
    preFix["description"] = "pre-fix readable metadata";
    preFix["displayOn"] = true;
    preFix["mainVolume"] = 255;
    preFix["mutedVolume"] = 255;
    JsonArray bytes = preFix["bytes"].to<JsonArray>();
    for (uint8_t value : rawBytes) bytes.add(value);
    preFix["muteToMuteVolume"] = false;
    preFix["crc32"] = computeCrc32(rawBytes, sizeof(rawBytes));
    String preFixJson;
    serializeJson(preFix, preFixJson);
    writeFileFromString(fs, "/v1profiles/Existing.json", preFixJson.c_str());

    V1Profile loaded = makeProfile("Sentinel", 90, "unchanged");
    const V1Profile before = loaded;
    TEST_ASSERT_FALSE(manager.loadProfile("Existing", loaded));
    TEST_ASSERT_EQUAL_STRING(before.name.c_str(), loaded.name.c_str());
    TEST_ASSERT_EQUAL_STRING(before.description.c_str(), loaded.description.c_str());
    TEST_ASSERT_EQUAL_UINT8_ARRAY(before.settings.bytes, loaded.settings.bytes, 6);
    TEST_ASSERT_EQUAL_STRING(preFixJson.c_str(), readFileToString(fs, "/v1profiles/Existing.json").c_str());
}

void test_json_to_settings_requires_redundant_raw_and_readable_fields_to_match() {
    V1ProfileManager manager;
    V1UserSettings settings = makeProfile("Sentinel", 77).settings;
    const V1UserSettings before = settings;

    TEST_ASSERT_FALSE(manager.jsonToSettings(
        String("{\"bytes\":[16,255,255,255,255,255],"
               "\"muteToMuteVolume\":false,\"autoMute\":1}"), settings));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(before.bytes, settings.bytes, 6);

    TEST_ASSERT_TRUE(manager.jsonToSettings(
        String("{\"bytes\":[16,255,255,255,255,255],"
               "\"muteToMuteVolume\":true,\"autoMute\":3}"), settings));
    TEST_ASSERT_EQUAL_UINT8(0x10, settings.bytes[0]);
    TEST_ASSERT_TRUE(settings.muteToMuteVolume());
    TEST_ASSERT_EQUAL_UINT8(3, settings.autoMute());

    TEST_ASSERT_FALSE(manager.jsonToSettings(
        String("{\"bytes\":[16,255,255,255,255,255],"
               "\"baseBytes\":[16,255,255,255,255,255]}"), settings));
}

void test_captured_base_bytes_preserve_reserved_bits_through_edit_save_and_reload() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    V1UserSettings captured;
    TEST_ASSERT_TRUE(manager.jsonToSettings(
        String("{\"baseBytes\":[255,255,255,255,165,90],\"gatsoRT4\":true}"), captured));
    // gatsoRT4 clears only byte 4 bit 0. Reserved/high bits and the wholly
    // opaque sixth byte must retain their detector-captured values.
    TEST_ASSERT_EQUAL_HEX8(0xA4, captured.bytes[4]);
    TEST_ASSERT_EQUAL_HEX8(0x5A, captured.bytes[5]);

    V1Profile draft("Captured draft");
    draft.settings = captured;
    TEST_ASSERT_TRUE(manager.saveProfile(draft).success);

    V1Profile reloaded;
    TEST_ASSERT_TRUE(manager.loadProfile("Captured draft", reloaded));
    TEST_ASSERT_EQUAL_HEX8(0xA4, reloaded.settings.bytes[4]);
    TEST_ASSERT_EQUAL_HEX8(0x5A, reloaded.settings.bytes[5]);
    TEST_ASSERT_TRUE(reloaded.settings.gatsoRT4());
}

void test_malformed_base_bytes_are_rejected_without_partial_mutation() {
    V1ProfileManager manager;
    V1UserSettings settings = makeProfile("Sentinel", 100).settings;
    const V1UserSettings before = settings;

    TEST_ASSERT_FALSE(manager.jsonToSettings(
        String("{\"baseBytes\":[255,255,255,255,165,\"bad\"],\"gatsoRT4\":true}"), settings));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(before.bytes, settings.bytes, 6);
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

void test_catalog_collection_allocation_failures_return_unavailable_without_throw_or_mutation() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));
    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("Road", 10, "durable")).success);

    manager.utFailAllocation(V1ProfileAllocationFailurePoint::ListGrowth);
    const ProfileListResult list = manager.listProfilesResult();
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::IoError),
                          static_cast<int>(list.status));
    TEST_ASSERT_EQUAL_UINT(0u, list.profiles.size());

    manager.utFailAllocation(V1ProfileAllocationFailurePoint::PageGrowth);
    const ProfilePageResult page = manager.listProfilesPageResult("", 10);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::IoError),
                          static_cast<int>(page.status));
    TEST_ASSERT_EQUAL_UINT(0u, page.profiles.size());

    std::vector<V1Profile> snapshot;
    manager.utFailAllocation(V1ProfileAllocationFailurePoint::SnapshotGrowth);
    const ProfileOperationResult snapshotResult = manager.snapshotProfiles(snapshot);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::IoError),
                          static_cast<int>(snapshotResult.status));
    TEST_ASSERT_EQUAL_UINT(0u, snapshot.size());

    V1Profile durable;
    TEST_ASSERT_TRUE(manager.loadProfile("Road", durable));
    TEST_ASSERT_EQUAL_STRING("durable", durable.description.c_str());
}

void test_save_scratch_allocation_failure_precedes_new_save_files() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    manager.utFailAllocation(V1ProfileAllocationFailurePoint::SaveScratch);
    const ProfileSaveResult result = manager.saveProfile(makeProfile("Road", 10, "candidate"));
    TEST_ASSERT_FALSE(result.success);
    TEST_ASSERT_EQUAL_INT(static_cast<int>(ProfileStorageStatus::IoError),
                          static_cast<int>(result.status));
    TEST_ASSERT_EQUAL_STRING("Profile save scratch memory unavailable", result.error.c_str());
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Road.json"));
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Road.json.tmp"));
    TEST_ASSERT_FALSE(fs.exists("/v1profiles/Road.json.meta"));
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
    RUN_TEST(test_maximum_description_and_64_definitions_fit_but_one_extra_byte_preserves_live_profile);
    RUN_TEST(test_schema_v3_detector_policy_round_trips_with_authoritative_raw_bytes);
    RUN_TEST(test_detector_configuration_rejects_half_zero_sweep_without_mutating_output);
    RUN_TEST(test_genuine_schema_v2_profile_migrates_without_reinterpretation);
    RUN_TEST(test_schema_v3_rejects_crc_valid_unknown_missing_or_conflicting_readable_fields);
    RUN_TEST(test_schema_v2_mixed_markers_crc_omissions_and_malformed_policy_reject_atomically);
    RUN_TEST(test_exact_legacy_profile_without_schema_or_crc_remains_backward_readable);
    RUN_TEST(test_persisted_legacy_profile_shape_is_strict_and_never_mutates_output);
    RUN_TEST(test_save_requires_final_file_reopen_and_crc_validation);
    RUN_TEST(test_load_profile_rejects_invalid_raw_bytes_without_mutating_output);
    RUN_TEST(test_json_to_settings_rejects_invalid_raw_bytes_without_mutating_output);
    RUN_TEST(test_json_to_settings_strictly_validates_human_readable_fields_without_mutation);
    RUN_TEST(test_conflicting_legacy_human_metadata_is_rejected_without_rewriting_raw_bytes);
    RUN_TEST(test_json_to_settings_requires_redundant_raw_and_readable_fields_to_match);
    RUN_TEST(test_captured_base_bytes_preserve_reserved_bits_through_edit_save_and_reload);
    RUN_TEST(test_malformed_base_bytes_are_rejected_without_partial_mutation);
    RUN_TEST(test_v41039_photo_settings_round_trip_through_json);
    RUN_TEST(test_path_like_name_is_rejected_without_creating_a_profile);
    RUN_TEST(test_profile_name_contract_rejects_hidden_long_blank_and_canonical_collisions);
    RUN_TEST(test_sd_contention_returns_busy_for_every_profile_transaction);
    RUN_TEST(test_catalog_collection_allocation_failures_return_unavailable_without_throw_or_mutation);
    RUN_TEST(test_save_scratch_allocation_failure_precedes_new_save_files);
    RUN_TEST(test_littlefs_fallback_edits_and_deletion_reconcile_without_resurrection);
    RUN_TEST(test_equal_generation_divergence_converges_to_offline_fallback_edit);
    RUN_TEST(test_corrupt_newer_profile_cannot_replace_valid_older_mirror);
    RUN_TEST(test_profile_sync_metadata_short_write_is_rejected);
    RUN_TEST(test_secondary_profile_save_failure_is_reported_and_rolls_back_both_copies);
    RUN_TEST(test_secondary_tombstone_failure_is_reported_and_preserves_profile);
    RUN_TEST(test_existing_malformed_sync_metadata_fails_closed_without_resurrecting_json);
    RUN_TEST(test_sync_metadata_crc_mutation_and_oversize_fail_closed);
    RUN_TEST(test_valid_tombstone_remains_authoritative_when_json_removal_was_incomplete);
    RUN_TEST(test_valid_profile_metadata_mirror_repairs_corrupt_primary_and_legacy_is_rewritten);
    RUN_TEST(test_cursor_pages_keep_grandfathered_over_limit_catalog_discoverable_and_prunable);
    RUN_TEST(test_reconcile_union_over_catalog_bound_is_bounded_and_preserves_both_stores);
    RUN_TEST(test_reconcile_psram_unavailable_never_overwrites_newer_profile_or_tombstone);
    RUN_TEST(test_secondary_rollback_path_allocation_failure_precedes_every_store_mutation);
    RUN_TEST(test_profile_sync_generation_exhaustion_preserves_live_profile_and_tombstone_state);
    RUN_TEST(test_profile_reconcile_generation_exhaustion_does_not_choose_or_overwrite_either_copy);
    RUN_TEST(test_interrupted_save_recovery_uses_bounded_scans_and_never_replaces_live_profile);
    return UNITY_END();
}
