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

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_touch_defers_device_write_until_flush_and_reloads_saved_record);
    RUN_TEST(test_failed_deferred_promotion_preserves_live_store_and_retry_succeeds);
    RUN_TEST(test_load_uses_valid_rollback_when_live_store_is_invalid);
    RUN_TEST(test_begin_migrates_device_store_from_secondary_filesystem);
    return UNITY_END();
}
