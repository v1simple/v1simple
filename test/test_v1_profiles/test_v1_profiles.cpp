#include <unity.h>

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
    g_tempRoot = nextTempRoot();
    std::filesystem::remove_all(g_tempRoot);
    std::filesystem::create_directories(g_tempRoot);
}

void tearDown() {
    fs::mock_reset_fs_rename_state();
    fs::mock_reset_fs_write_budget();
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

void test_rename_sanitized_collision_updates_name_in_place() {
    fs::FS fs(g_tempRoot);
    V1ProfileManager manager;
    TEST_ASSERT_TRUE(manager.begin(&fs));

    TEST_ASSERT_TRUE(manager.saveProfile(makeProfile("Road/1", 50, "collision")).success);
    const uint32_t beforeRevision = manager.catalogRevision();

    TEST_ASSERT_TRUE(manager.renameProfile("Road/1", "Road_1"));
    TEST_ASSERT_TRUE(manager.catalogRevision() > beforeRevision);
    TEST_ASSERT_TRUE(fs.exists("/v1profiles/Road_1.json"));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(countFilesInProfileDir(".json")));

    V1Profile loaded;
    TEST_ASSERT_TRUE(manager.loadProfile("Road_1", loaded));
    TEST_ASSERT_EQUAL_STRING("Road_1", loaded.name.c_str());
    TEST_ASSERT_EQUAL_STRING("collision", loaded.description.c_str());
    TEST_ASSERT_EQUAL_UINT8(50, loaded.settings.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(55, loaded.settings.bytes[5]);
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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_save_profile_short_write_new_file_leaves_no_live_json);
    RUN_TEST(test_save_profile_short_write_existing_file_preserves_previous_profile);
    RUN_TEST(test_save_profile_normal_path_still_succeeds);
    RUN_TEST(test_rename_same_name_is_successful_noop);
    RUN_TEST(test_rename_sanitized_collision_updates_name_in_place);
    RUN_TEST(test_rename_existing_distinct_destination_fails_without_mutation);
    RUN_TEST(test_rename_normal_path_succeeds_and_advances_revision);
    return UNITY_END();
}
