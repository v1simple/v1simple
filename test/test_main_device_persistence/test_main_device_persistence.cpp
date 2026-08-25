#include <unity.h>

#include <filesystem>
#include <string>

#include <ArduinoJson.h>

#include "../mocks/Arduino.h"
#include "../mocks/mock_heap_caps_state.h"
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

#define WIFI_MANAGER_H
class WiFiManager {
  public:
    static constexpr uint32_t WIFI_RUNTIME_MIN_FREE_AP_ONLY = 16384;
    static constexpr uint32_t WIFI_RUNTIME_MIN_BLOCK_AP_ONLY = 12288;
};

#include "../../src/storage_json_rollback.cpp"
#include "../../src/v1_devices.cpp"
#include "../../src/main_persist.cpp"

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
           ("main_device_persistence_" + std::to_string(++g_tempRootIndex));
}

}  // namespace

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    mock_reset_heap_caps();
    StorageManager::resetMockSdLockState();
    fs::mock_reset_fs_rename_state();
    fs::mock_reset_fs_write_budget();
    deviceStoreSaveState = DirtySaveState{};
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

void test_coordinator_waits_for_interval_then_flushes_injected_store() {
    fs::FS fs(g_tempRoot);
    StorageManager persistenceStorage;
    persistenceStorage.setFilesystem(&fs, false);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_TRUE(devices.touchDeviceInMemory("AA:BB:CC:DD:EE:FF"));

    processV1DeviceStoreSave(4999, persistenceStorage, devices);
    TEST_ASSERT_TRUE(devices.hasPendingSave());
    TEST_ASSERT_FALSE(fs.exists("/v1devices.json"));

    processV1DeviceStoreSave(5000, persistenceStorage, devices);
    TEST_ASSERT_FALSE(devices.hasPendingSave());
    TEST_ASSERT_TRUE(fs.exists("/v1devices.json"));
}

void test_coordinator_defers_busy_sd_without_blocking_then_retries() {
    fs::FS fs(g_tempRoot);
    StorageManager persistenceStorage;
    persistenceStorage.setFilesystem(&fs, true);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_TRUE(devices.touchDeviceInMemory("11:22:33:44:55:66"));
    StorageManager::mockSdLockState.failNextTryLockCount = 1;

    processV1DeviceStoreSave(5000, persistenceStorage, devices);
    TEST_ASSERT_TRUE(devices.hasPendingSave());
    TEST_ASSERT_EQUAL_UINT32(1u, StorageManager::mockSdLockState.tryAcquireCalls);

    processV1DeviceStoreSave(5999, persistenceStorage, devices);
    TEST_ASSERT_TRUE(devices.hasPendingSave());
    TEST_ASSERT_EQUAL_UINT32(1u, StorageManager::mockSdLockState.tryAcquireCalls);

    processV1DeviceStoreSave(6000, persistenceStorage, devices);
    TEST_ASSERT_FALSE(devices.hasPendingSave());
    TEST_ASSERT_EQUAL_UINT32(2u, StorageManager::mockSdLockState.tryAcquireCalls);
    TEST_ASSERT_TRUE(fs.exists("/v1devices.json"));
}

void test_coordinator_defers_low_dma_then_recovers_without_losing_dirty_state() {
    fs::FS fs(g_tempRoot);
    StorageManager persistenceStorage;
    persistenceStorage.setFilesystem(&fs, true);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_TRUE(devices.touchDeviceInMemory("AA:BB:CC:DD:EE:FF"));
    mock_set_heap_caps(16000, 12000);

    processV1DeviceStoreSave(5000, persistenceStorage, devices);
    TEST_ASSERT_TRUE(devices.hasPendingSave());
    TEST_ASSERT_EQUAL_UINT32(0u, StorageManager::mockSdLockState.tryAcquireCalls);

    mock_set_heap_caps(320000, 8u * 1024u * 1024u);
    processV1DeviceStoreSave(6000, persistenceStorage, devices);
    TEST_ASSERT_FALSE(devices.hasPendingSave());
    TEST_ASSERT_EQUAL_UINT32(1u, StorageManager::mockSdLockState.tryAcquireCalls);
    TEST_ASSERT_TRUE(fs.exists("/v1devices.json"));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_coordinator_waits_for_interval_then_flushes_injected_store);
    RUN_TEST(test_coordinator_defers_busy_sd_without_blocking_then_retries);
    RUN_TEST(test_coordinator_defers_low_dma_then_recovers_without_losing_dirty_state);
    return UNITY_END();
}
