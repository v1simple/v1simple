#include <unity.h>

#include <filesystem>
#include <fstream>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/storage_json_rollback.cpp"
#define private public
#include "../../src/v1_devices.h"
#undef private
#include "../../src/v1_devices.cpp"

namespace {

std::filesystem::path makeFsRoot(const char* testName) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / ("v1simple_v1_device_store_" + std::string(testName));
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    return root;
}

std::filesystem::path resolveFsPath(const std::filesystem::path& root, const char* logicalPath) {
    std::filesystem::path relative = logicalPath ? std::filesystem::path(logicalPath) : std::filesystem::path();
    if (relative.is_absolute()) {
        relative = relative.relative_path();
    }
    return root / relative;
}

void writeText(const std::filesystem::path& path, const char* text) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    TEST_ASSERT_TRUE(stream.is_open());
    stream << (text ? text : "");
    TEST_ASSERT_TRUE(stream.good());
}

}  // namespace

void setUp() {
    mockMillis = 0;
    mockMicros = 0;
}
void tearDown() {}

void test_device_store_prefers_valid_live_over_prev() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS fs(root);
    writeText(resolveFsPath(root, "/v1devices.json"),
              "{\"version\":1,\"devices\":[{\"address\":\"AA:BB:CC:DD:EE:FF\",\"name\":\"Live\",\"defaultProfile\":2,\"lastSeenMs\":10}]}");
    writeText(resolveFsPath(root, "/v1devices.json.prev"),
              "{\"version\":1,\"devices\":[{\"address\":\"11:22:33:44:55:66\",\"name\":\"Prev\",\"defaultProfile\":1,\"lastSeenMs\":20}]}");

    V1DeviceStore store;
    TEST_ASSERT_TRUE(store.begin(&fs));

    const auto devices = store.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1, devices.size());
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", devices[0].address.c_str());
    TEST_ASSERT_EQUAL_STRING("Live", devices[0].name.c_str());
    TEST_ASSERT_EQUAL_UINT8(2, devices[0].defaultProfile);
}

void test_device_store_falls_back_to_prev_when_live_missing() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS fs(root);
    writeText(resolveFsPath(root, "/v1devices.json.prev"),
              "{\"version\":1,\"devices\":[{\"address\":\"11:22:33:44:55:66\",\"name\":\"Prev\",\"defaultProfile\":1,\"lastSeenMs\":20}]}");

    V1DeviceStore store;
    TEST_ASSERT_TRUE(store.begin(&fs));

    const auto devices = store.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1, devices.size());
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", devices[0].address.c_str());
    TEST_ASSERT_EQUAL_STRING("Prev", devices[0].name.c_str());
}

void test_device_store_falls_back_to_prev_when_live_is_corrupt() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS fs(root);
    writeText(resolveFsPath(root, "/v1devices.json"), "{bad json");
    writeText(resolveFsPath(root, "/v1devices.json.prev"),
              "{\"version\":1,\"devices\":[{\"address\":\"11:22:33:44:55:66\",\"name\":\"Prev\",\"defaultProfile\":1,\"lastSeenMs\":20}]}");

    V1DeviceStore store;
    TEST_ASSERT_TRUE(store.begin(&fs));

    const auto devices = store.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1, devices.size());
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", devices[0].address.c_str());
    TEST_ASSERT_EQUAL_STRING("Prev", devices[0].name.c_str());
}

void test_touch_in_memory_defers_write_until_flush() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS fs(root);

    V1DeviceStore store;
    TEST_ASSERT_TRUE(store.begin(&fs));

    mockMillis = 1234;
    TEST_ASSERT_TRUE(store.touchDeviceInMemory("aa-bb-cc-dd-ee-ff"));
    TEST_ASSERT_TRUE(store.hasPendingSave());
    TEST_ASSERT_FALSE(fs.exists("/v1devices.json"));

    const auto pendingDevices = store.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1, pendingDevices.size());
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", pendingDevices[0].address.c_str());
    TEST_ASSERT_EQUAL_UINT32(1234, pendingDevices[0].lastSeenMs);

    TEST_ASSERT_TRUE(store.flushPendingSave());
    TEST_ASSERT_FALSE(store.hasPendingSave());
    TEST_ASSERT_TRUE(fs.exists("/v1devices.json"));

    V1DeviceStore reloaded;
    TEST_ASSERT_TRUE(reloaded.begin(&fs));
    const auto persistedDevices = reloaded.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1, persistedDevices.size());
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", persistedDevices[0].address.c_str());
    TEST_ASSERT_EQUAL_UINT32(1234, persistedDevices[0].lastSeenMs);
}

void test_set_name_and_profile_create_and_persist_record() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS fs(root);

    V1DeviceStore store;
    TEST_ASSERT_TRUE(store.begin(&fs));

    mockMillis = 200;
    TEST_ASSERT_TRUE(store.setDeviceName("aa:bb:cc:dd:ee:ff", "  Truck  "));
    TEST_ASSERT_TRUE(store.setDeviceDefaultProfile("AA:BB:CC:DD:EE:FF", 9));
    TEST_ASSERT_FALSE(store.hasPendingSave());

    const auto devices = store.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1, devices.size());
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", devices[0].address.c_str());
    TEST_ASSERT_EQUAL_STRING("Truck", devices[0].name.c_str());
    TEST_ASSERT_EQUAL_UINT8(3, devices[0].defaultProfile);
    TEST_ASSERT_EQUAL_UINT32(200, devices[0].lastSeenMs);
    TEST_ASSERT_EQUAL_UINT8(3, store.getDeviceDefaultProfile("aa-bb-cc-dd-ee-ff"));

    V1DeviceStore reloaded;
    TEST_ASSERT_TRUE(reloaded.begin(&fs));
    const auto persistedDevices = reloaded.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1, persistedDevices.size());
    TEST_ASSERT_EQUAL_STRING("Truck", persistedDevices[0].name.c_str());
    TEST_ASSERT_EQUAL_UINT8(3, persistedDevices[0].defaultProfile);
}

void test_upsert_persists_and_remove_deletes_record() {
    const std::filesystem::path root = makeFsRoot(__func__);
    fs::FS fs(root);

    V1DeviceStore store;
    TEST_ASSERT_TRUE(store.begin(&fs));

    mockMillis = 4321;
    TEST_ASSERT_TRUE(store.upsertDevice("11-22-33-44-55-66"));
    TEST_ASSERT_FALSE(store.hasPendingSave());

    V1DeviceStore reloaded;
    TEST_ASSERT_TRUE(reloaded.begin(&fs));
    const auto loadedDevices = reloaded.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1, loadedDevices.size());
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", loadedDevices[0].address.c_str());
    TEST_ASSERT_EQUAL_UINT32(4321, loadedDevices[0].lastSeenMs);

    TEST_ASSERT_TRUE(reloaded.removeDevice("11:22:33:44:55:66"));
    TEST_ASSERT_FALSE(reloaded.hasPendingSave());
    TEST_ASSERT_EQUAL_UINT32(0, reloaded.listDevices().size());

    V1DeviceStore emptyReload;
    TEST_ASSERT_TRUE(emptyReload.begin(&fs));
    TEST_ASSERT_EQUAL_UINT32(0, emptyReload.listDevices().size());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_device_store_prefers_valid_live_over_prev);
    RUN_TEST(test_device_store_falls_back_to_prev_when_live_missing);
    RUN_TEST(test_device_store_falls_back_to_prev_when_live_is_corrupt);
    RUN_TEST(test_touch_in_memory_defers_write_until_flush);
    RUN_TEST(test_set_name_and_profile_create_and_persist_record);
    RUN_TEST(test_upsert_persists_and_remove_deletes_record);
    return UNITY_END();
}
