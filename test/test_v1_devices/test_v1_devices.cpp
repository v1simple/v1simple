#include <unity.h>

#include <cstring>
#include <filesystem>
#include <string>

#include <ArduinoJson.h>

#include "../mocks/Arduino.h"
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

#include "../../src/storage_json_rollback.cpp"
#include "../../src/v1_devices.cpp"

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
           ("v1_devices_" + std::to_string(++g_tempRootIndex));
}

void writeFileFromString(fs::FS& fs, const char* path, const char* contents) {
    File file = fs.open(path, FILE_WRITE);
    TEST_ASSERT_TRUE(file);
    TEST_ASSERT_EQUAL_UINT(std::strlen(contents), file.print(contents));
    file.close();
}

std::string readFileToString(fs::FS& fs, const char* path) {
    File file = fs.open(path, FILE_READ);
    if (!file) return {};
    std::string contents;
    while (file.available()) contents.push_back(static_cast<char>(file.read()));
    file.close();
    return contents;
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

void test_touch_defers_device_write_until_flush_and_reloads_saved_record() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));

    TEST_ASSERT_TRUE(devices.touchDeviceInMemory("aa-bb-cc-dd-ee-ff"));
    TEST_ASSERT_TRUE(devices.hasPendingSave());
    TEST_ASSERT_FALSE(fs.exists("/v1devices.json"));

    TEST_ASSERT_TRUE(devices.flushPendingSave());
    TEST_ASSERT_FALSE(devices.hasPendingSave());
    TEST_ASSERT_TRUE(fs.exists("/v1devices.json"));

    V1DeviceStore reloaded;
    TEST_ASSERT_TRUE(reloaded.begin(&fs));
    const std::vector<V1DeviceRecord> records = reloaded.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(records.size()));
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", records[0].address.c_str());
    TEST_ASSERT_EQUAL_UINT32(1000u, records[0].lastSeenMs);
}

void test_failed_deferred_promotion_preserves_live_store_and_retry_succeeds() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_TRUE(devices.touchDeviceInMemory("AA:BB:CC:DD:EE:FF"));
    TEST_ASSERT_TRUE(devices.flushPendingSave());

    mockMillis = 2000;
    TEST_ASSERT_TRUE(devices.touchDeviceInMemory("AA:BB:CC:DD:EE:FF"));
    fs::mock_fail_next_rename();

    TEST_ASSERT_FALSE(devices.flushPendingSave());
    TEST_ASSERT_TRUE(devices.hasPendingSave());

    V1DeviceStore unchanged;
    TEST_ASSERT_TRUE(unchanged.begin(&fs));
    const std::vector<V1DeviceRecord> beforeRetry = unchanged.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(beforeRetry.size()));
    TEST_ASSERT_EQUAL_UINT32(1000u, beforeRetry[0].lastSeenMs);

    TEST_ASSERT_TRUE(devices.flushPendingSave());
    TEST_ASSERT_FALSE(devices.hasPendingSave());

    V1DeviceStore updated;
    TEST_ASSERT_TRUE(updated.begin(&fs));
    const std::vector<V1DeviceRecord> afterRetry = updated.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(afterRetry.size()));
    TEST_ASSERT_EQUAL_UINT32(2000u, afterRetry[0].lastSeenMs);
}

void test_load_uses_valid_rollback_when_live_store_is_invalid() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_TRUE(devices.touchDeviceInMemory("11:22:33:44:55:66"));
    TEST_ASSERT_TRUE(devices.flushPendingSave());

    TEST_ASSERT_TRUE(fs.rename("/v1devices.json", "/v1devices.json.prev"));
    writeFileFromString(fs, "/v1devices.json", "{not-json");

    V1DeviceStore reloaded;
    TEST_ASSERT_TRUE(reloaded.begin(&fs));
    const std::vector<V1DeviceRecord> records = reloaded.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(records.size()));
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", records[0].address.c_str());
}

void test_begin_migrates_device_store_from_secondary_filesystem() {
    const std::filesystem::path importRoot = nextTempRoot();
    std::filesystem::remove_all(importRoot);
    std::filesystem::create_directories(importRoot);

    fs::FS primary(g_tempRoot);
    fs::FS secondary(importRoot);
    V1DeviceStore source;
    TEST_ASSERT_TRUE(source.begin(&secondary));
    TEST_ASSERT_TRUE(source.setDeviceName("AA:BB:CC:DD:EE:FF", "Road V1"));
    TEST_ASSERT_TRUE(source.setDeviceDefaultProfile("AA:BB:CC:DD:EE:FF", 2));

    V1DeviceStore migrated;
    TEST_ASSERT_TRUE(migrated.begin(&primary, &secondary));
    TEST_ASSERT_TRUE(primary.exists("/v1devices.json"));
    const std::vector<V1DeviceRecord> records = migrated.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(records.size()));
    TEST_ASSERT_EQUAL_STRING("Road V1", records[0].name.c_str());
    TEST_ASSERT_EQUAL_UINT8(2u, records[0].defaultProfile);

    std::filesystem::remove_all(importRoot);
}

void test_short_device_store_write_fails_and_preserves_committed_catalog() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_TRUE(devices.setDeviceName("AA:BB:CC:DD:EE:FF", "Original"));
    const std::string committed = readFileToString(fs, "/v1devices.json");

    fs::mock_set_fs_write_budget(20);
    TEST_ASSERT_FALSE(devices.setDeviceName("AA:BB:CC:DD:EE:FF", "Changed"));
    TEST_ASSERT_TRUE(devices.hasPendingSave());
    TEST_ASSERT_EQUAL_STRING(committed.c_str(), readFileToString(fs, "/v1devices.json").c_str());

    fs::mock_reset_fs_write_budget();
    V1DeviceStore afterReboot;
    TEST_ASSERT_TRUE(afterReboot.begin(&fs));
    const std::vector<V1DeviceRecord> records = afterReboot.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(records.size()));
    TEST_ASSERT_EQUAL_STRING("Original", records[0].name.c_str());
}

void test_newer_fallback_device_edit_wins_when_stale_sd_returns() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd";
    const std::filesystem::path littleRoot = g_tempRoot / "little";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);

    V1DeviceStore initial;
    TEST_ASSERT_TRUE(initial.begin(&sd, &little));
    TEST_ASSERT_TRUE(initial.setDeviceName("AA:BB:CC:DD:EE:FF", "Before"));
    TEST_ASSERT_TRUE(initial.setDeviceDefaultProfile("AA:BB:CC:DD:EE:FF", 1));

    V1DeviceStore fallback;
    TEST_ASSERT_TRUE(fallback.begin(&little));
    TEST_ASSERT_TRUE(fallback.setDeviceName("AA:BB:CC:DD:EE:FF", "Offline edit"));
    TEST_ASSERT_TRUE(fallback.setDeviceDefaultProfile("AA:BB:CC:DD:EE:FF", 3));

    V1DeviceStore reconciled;
    TEST_ASSERT_TRUE(reconciled.begin(&sd, &little));
    const std::vector<V1DeviceRecord> records = reconciled.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(records.size()));
    TEST_ASSERT_EQUAL_STRING("Offline edit", records[0].name.c_str());
    TEST_ASSERT_EQUAL_UINT8(3u, records[0].defaultProfile);
    TEST_ASSERT_EQUAL_STRING(readFileToString(sd, "/v1devices.json").c_str(),
                             readFileToString(little, "/v1devices.json").c_str());
}

void test_valid_secondary_repairs_corrupt_primary_device_store() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd";
    const std::filesystem::path littleRoot = g_tempRoot / "little";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);

    V1DeviceStore fallback;
    TEST_ASSERT_TRUE(fallback.begin(&little));
    TEST_ASSERT_TRUE(fallback.setDeviceName("11:22:33:44:55:66", "Recovered"));
    writeFileFromString(sd, "/v1devices.json", "{not-json");

    V1DeviceStore repaired;
    TEST_ASSERT_TRUE(repaired.begin(&sd, &little));
    const std::vector<V1DeviceRecord> records = repaired.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(records.size()));
    TEST_ASSERT_EQUAL_STRING("Recovered", records[0].name.c_str());

    V1DeviceStore primaryOnlyAfterReboot;
    TEST_ASSERT_TRUE(primaryOnlyAfterReboot.begin(&sd));
    const std::vector<V1DeviceRecord> primaryRecords = primaryOnlyAfterReboot.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(primaryRecords.size()));
    TEST_ASSERT_EQUAL_STRING("Recovered", primaryRecords[0].name.c_str());
}

void test_equal_generation_device_conflict_converges_to_fallback_copy() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd";
    const std::filesystem::path littleRoot = g_tempRoot / "little";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);

    V1DeviceStore sdOnly;
    TEST_ASSERT_TRUE(sdOnly.begin(&sd));
    TEST_ASSERT_TRUE(sdOnly.setDeviceName("AA:BB:CC:DD:EE:FF", "SD copy"));

    V1DeviceStore fallbackOnly;
    TEST_ASSERT_TRUE(fallbackOnly.begin(&little));
    TEST_ASSERT_TRUE(fallbackOnly.setDeviceName("AA:BB:CC:DD:EE:FF", "Fallback copy"));

    V1DeviceStore reconciled;
    TEST_ASSERT_TRUE(reconciled.begin(&sd, &little));
    const std::vector<V1DeviceRecord> records = reconciled.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(records.size()));
    TEST_ASSERT_EQUAL_STRING("Fallback copy", records[0].name.c_str());
    TEST_ASSERT_EQUAL_STRING(readFileToString(sd, "/v1devices.json").c_str(),
                             readFileToString(little, "/v1devices.json").c_str());
}

void test_legacy_v1_device_store_is_loaded_and_upgraded_with_integrity_metadata() {
    fs::FS fs(g_tempRoot);
    writeFileFromString(
        fs, "/v1devices.json",
        "{\"version\":1,\"devices\":[{\"address\":\"AA:BB:CC:DD:EE:FF\",\"name\":\"Legacy\","
        "\"defaultProfile\":2,\"lastSeenMs\":1234}]}");

    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    const std::vector<V1DeviceRecord> records = devices.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(records.size()));
    TEST_ASSERT_EQUAL_STRING("Legacy", records[0].name.c_str());
    TEST_ASSERT_EQUAL_UINT8(2u, records[0].defaultProfile);

    JsonDocument upgraded;
    File file = fs.open("/v1devices.json", FILE_READ);
    TEST_ASSERT_TRUE(file);
    TEST_ASSERT_FALSE(deserializeJson(upgraded, file));
    file.close();
    TEST_ASSERT_EQUAL_UINT8(2u, upgraded["version"].as<uint8_t>());
    TEST_ASSERT_EQUAL_UINT32(1u, upgraded["generation"].as<uint32_t>());
    TEST_ASSERT_TRUE(upgraded["crc32"].is<uint32_t>());
}

void test_failed_primary_repair_remains_pending_after_secondary_succeeds() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd";
    const std::filesystem::path littleRoot = g_tempRoot / "little";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);

    V1DeviceStore sdOnly;
    TEST_ASSERT_TRUE(sdOnly.begin(&sd));
    TEST_ASSERT_TRUE(sdOnly.setDeviceName("AA:BB:CC:DD:EE:FF", "SD copy"));

    V1DeviceStore fallbackOnly;
    TEST_ASSERT_TRUE(fallbackOnly.begin(&little));
    TEST_ASSERT_TRUE(fallbackOnly.setDeviceName("AA:BB:CC:DD:EE:FF", "Fallback copy"));

    fs::mock_reset_fs_rename_state();
    fs::mock_fail_rename_on_call(1); // fail primary repair, then allow secondary repair
    V1DeviceStore reconciled;
    TEST_ASSERT_TRUE(reconciled.begin(&sd, &little));
    TEST_ASSERT_TRUE(reconciled.hasPendingSave());

    fs::mock_reset_fs_rename_state();
    TEST_ASSERT_TRUE(reconciled.flushPendingSave());
    TEST_ASSERT_FALSE(reconciled.hasPendingSave());
    TEST_ASSERT_EQUAL_STRING(readFileToString(sd, "/v1devices.json").c_str(),
                             readFileToString(little, "/v1devices.json").c_str());
}

void test_secondary_device_store_failure_is_reported_and_retried() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd";
    const std::filesystem::path littleRoot = g_tempRoot / "little";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);

    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&sd, &little));
    TEST_ASSERT_TRUE(devices.setDeviceName("AA:BB:CC:DD:EE:FF", "Before"));

    fs::mock_reset_fs_rename_state();
    fs::mock_fail_rename_on_call(3); // secondary live-store promotion
    TEST_ASSERT_FALSE(devices.setDeviceName("AA:BB:CC:DD:EE:FF", "After"));
    TEST_ASSERT_TRUE(devices.hasPendingSave());
    TEST_ASSERT_NOT_EQUAL(0, readFileToString(sd, "/v1devices.json").compare(
                                 readFileToString(little, "/v1devices.json")));

    fs::mock_reset_fs_rename_state();
    TEST_ASSERT_TRUE(devices.flushPendingSave());
    TEST_ASSERT_FALSE(devices.hasPendingSave());
    TEST_ASSERT_EQUAL_STRING(readFileToString(sd, "/v1devices.json").c_str(),
                             readFileToString(little, "/v1devices.json").c_str());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_touch_defers_device_write_until_flush_and_reloads_saved_record);
    RUN_TEST(test_failed_deferred_promotion_preserves_live_store_and_retry_succeeds);
    RUN_TEST(test_load_uses_valid_rollback_when_live_store_is_invalid);
    RUN_TEST(test_begin_migrates_device_store_from_secondary_filesystem);
    RUN_TEST(test_short_device_store_write_fails_and_preserves_committed_catalog);
    RUN_TEST(test_newer_fallback_device_edit_wins_when_stale_sd_returns);
    RUN_TEST(test_valid_secondary_repairs_corrupt_primary_device_store);
    RUN_TEST(test_equal_generation_device_conflict_converges_to_fallback_copy);
    RUN_TEST(test_legacy_v1_device_store_is_loaded_and_upgraded_with_integrity_metadata);
    RUN_TEST(test_failed_primary_repair_remains_pending_after_secondary_succeeds);
    RUN_TEST(test_secondary_device_store_failure_is_reported_and_retried);
    return UNITY_END();
}
