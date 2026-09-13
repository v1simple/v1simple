#include <unity.h>

#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
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

void writeFileFromBytes(fs::FS& fs, const char* path, const std::string& contents) {
    File file = fs.open(path, FILE_WRITE);
    TEST_ASSERT_TRUE(file);
    TEST_ASSERT_EQUAL_UINT(contents.size(),
                           file.write(reinterpret_cast<const uint8_t*>(contents.data()), contents.size()));
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

void writeDeviceStoreDocument(fs::FS& fs, const char* path, JsonDocument& doc) {
    doc["crc32"] = deviceStoreContentCrc(doc);
    String serialized;
    serializeJson(doc, serialized);
    writeFileFromString(fs, path, serialized.c_str());
}

JsonObject addValidV4Device(JsonDocument& doc, const char* name = "Captured") {
    doc["version"] = 4;
    doc["generation"] = 7;
    JsonObject device = doc["devices"].to<JsonArray>().add<JsonObject>();
    device["address"] = "AA:BB:CC:DD:EE:FF";
    device["name"] = name;
    device["defaultProfile"] = 2;
    device["lastSeenMs"] = 7000;
    JsonObject snapshot = device["snapshot"].to<JsonObject>();
    snapshot["capturedBootId"] = 10;
    snapshot["capturedUptimeMs"] = 2000;
    snapshot["sessionGeneration"] = 3;
    snapshot["captureTimedOut"] = false;
    snapshot["firmwareVersion"] = 41039;
    snapshot["mode"] = static_cast<uint8_t>('A');
    return device;
}

}  // namespace

void setUp() {
    mockMillis = 1000;
    mockMicros = 1000000;
    mock_reset_heap_caps();
    fs::mock_reset_fs_rename_state();
    fs::mock_reset_fs_write_budget();
    g_tempRoot = nextTempRoot();
    std::filesystem::remove_all(g_tempRoot);
    std::filesystem::create_directories(g_tempRoot);
}

void tearDown() {
    mock_reset_heap_caps();
    fs::mock_reset_fs_rename_state();
    fs::mock_reset_fs_write_budget();
    if (!g_tempRoot.empty()) {
        std::filesystem::remove_all(g_tempRoot);
    }
}

String catalogAddress(unsigned int index) {
    char address[18];
    snprintf(address, sizeof(address), "00:00:00:00:00:%02X", index);
    return String(address);
}

void seedDeviceCatalog(V1DeviceStore& devices, unsigned int count) {
    for (unsigned int index = 0; index < count; ++index) {
        mockMillis = 100000 + index;
        TEST_ASSERT_TRUE(devices.upsertDevice(catalogAddress(index)));
    }
}

bool catalogContains(const V1DeviceStore& devices, const String& address) {
    for (const auto& record : devices.listDevices()) {
        if (record.address == address) return true;
    }
    return false;
}

void test_new_device_survives_cross_boot_capacity_for_every_insertion_path() {
    for (unsigned int count : {16u, 15u}) {
        for (int action = 0; action < 4; ++action) {
            const auto root = g_tempRoot / (std::to_string(count) + "_" + std::to_string(action));
            std::filesystem::create_directories(root);
            fs::FS fs(root);
            V1DeviceStore priorBoot;
            TEST_ASSERT_TRUE(priorBoot.begin(&fs));
            seedDeviceCatalog(priorBoot, count);
            mockMillis = 1000;
            V1DeviceStore currentBoot;
            TEST_ASSERT_TRUE(currentBoot.begin(&fs));
            const String added = "AA:BB:CC:DD:EE:FF";
            if (action == 0) TEST_ASSERT_TRUE(currentBoot.setDeviceName(added, "Newest detector"));
            if (action == 1) TEST_ASSERT_TRUE(currentBoot.setDeviceDefaultProfile(added, 2));
            if (action == 2) TEST_ASSERT_TRUE(currentBoot.upsertDevice(added));
            if (action == 3) {
                TEST_ASSERT_TRUE(currentBoot.touchDeviceInMemory(added));
                TEST_ASSERT_TRUE(currentBoot.hasPendingSave());
                TEST_ASSERT_TRUE(currentBoot.flushPendingSave());
            }
            TEST_ASSERT_TRUE(catalogContains(currentBoot, added));
            V1DeviceStore reloaded;
            TEST_ASSERT_TRUE(reloaded.begin(&fs));
            const auto records = reloaded.listDevices();
            TEST_ASSERT_EQUAL_UINT(16, records.size());
            TEST_ASSERT_EQUAL_STRING(added.c_str(), records.front().address.c_str());
            TEST_ASSERT_EQUAL_UINT32(1000, records.front().lastSeenMs);
            if (action == 0) TEST_ASSERT_EQUAL_STRING("Newest detector", records.front().name.c_str());
            if (action == 1) TEST_ASSERT_EQUAL_UINT8(2, records.front().defaultProfile);
            TEST_ASSERT_EQUAL(count == 15, catalogContains(reloaded, catalogAddress(0)));
            TEST_ASSERT_TRUE(catalogContains(reloaded, catalogAddress(count - 1)));
        }
    }
}

void test_recency_survives_successive_boots_and_existing_device_metadata_edits() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore original;
    TEST_ASSERT_TRUE(original.begin(&fs));
    seedDeviceCatalog(original, 16);
    mockMillis = 1000;
    V1DeviceStore secondBoot;
    TEST_ASSERT_TRUE(secondBoot.begin(&fs));
    TEST_ASSERT_TRUE(secondBoot.touchDeviceInMemory(catalogAddress(0)));
    TEST_ASSERT_TRUE(secondBoot.flushPendingSave());

    mockMillis = 5;
    V1DeviceStore thirdBoot;
    TEST_ASSERT_TRUE(thirdBoot.begin(&fs));
    TEST_ASSERT_EQUAL_STRING(catalogAddress(0).c_str(), thirdBoot.listDevices().front().address.c_str());
    // Editing metadata is not another sighting and must not alter recency.
    const auto beforeEdit = thirdBoot.listDevices();
    TEST_ASSERT_TRUE(thirdBoot.setDeviceName(catalogAddress(1), "Oldest edited"));
    TEST_ASSERT_TRUE(thirdBoot.setDeviceDefaultProfile(catalogAddress(1), 3));
    const auto afterEdit = thirdBoot.listDevices();
    for (size_t index = 0; index < beforeEdit.size(); ++index) {
        TEST_ASSERT_EQUAL_STRING(beforeEdit[index].address.c_str(), afterEdit[index].address.c_str());
        TEST_ASSERT_EQUAL_UINT32(beforeEdit[index].lastSeenMs, afterEdit[index].lastSeenMs);
    }
    TEST_ASSERT_TRUE(thirdBoot.upsertDevice("AA:BB:CC:DD:EE:FF"));
    TEST_ASSERT_FALSE(catalogContains(thirdBoot, catalogAddress(1)));
    TEST_ASSERT_TRUE(catalogContains(thirdBoot, catalogAddress(0)));

    mockMillis = 0;
    V1DeviceStore fourthBoot;
    TEST_ASSERT_TRUE(fourthBoot.begin(&fs));
    TEST_ASSERT_TRUE(fourthBoot.upsertDevice("11:22:33:44:55:66"));
    V1DeviceStore reloaded;
    TEST_ASSERT_TRUE(reloaded.begin(&fs));
    const auto records = reloaded.listDevices();
    TEST_ASSERT_EQUAL_UINT(16, records.size());
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", records[0].address.c_str());
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", records[1].address.c_str());
    TEST_ASSERT_EQUAL_STRING(catalogAddress(0).c_str(), records[2].address.c_str());
    TEST_ASSERT_FALSE(catalogContains(reloaded, catalogAddress(2)));
}

void test_device_name_rejects_non_roundtrippable_bytes_without_mutation() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_TRUE(devices.setDeviceName("AA:BB:CC:DD:EE:FF", "Original"));
    const std::string before = readFileToString(fs, "/v1devices.json");

    for (const uint8_t byte : {uint8_t{0x01}, uint8_t{0xff}}) {
        String invalid("BAD");
        invalid += static_cast<char>(byte);
        TEST_ASSERT_FALSE(devices.setDeviceName("AA:BB:CC:DD:EE:FF", invalid));
        TEST_ASSERT_EQUAL_STRING("Original", devices.listDevices().front().name.c_str());
        TEST_ASSERT_EQUAL_STRING(before.c_str(), readFileToString(fs, "/v1devices.json").c_str());
    }
}

void test_device_recency_survives_mirror_recovery_and_millis_wrap() {
    std::filesystem::create_directories(g_tempRoot / "sd");
    std::filesystem::create_directories(g_tempRoot / "little");
    fs::FS sd(g_tempRoot / "sd"), little(g_tempRoot / "little");
    V1DeviceStore initial;
    TEST_ASSERT_TRUE(initial.begin(&sd, &little));
    seedDeviceCatalog(initial, 16);
    V1DeviceStore offline;
    TEST_ASSERT_TRUE(offline.begin(&little));
    mockMillis = UINT32_MAX - 1;
    TEST_ASSERT_TRUE(offline.upsertDevice(catalogAddress(0)));
    mockMillis = 0;
    TEST_ASSERT_TRUE(offline.upsertDevice(catalogAddress(1)));
    TEST_ASSERT_TRUE(offline.upsertDevice("AA:BB:CC:DD:EE:FF"));
    V1DeviceStore returned;
    TEST_ASSERT_TRUE(returned.begin(&sd, &little));
    TEST_ASSERT_EQUAL_STRING(readFileToString(little, "/v1devices.json").c_str(),
                             readFileToString(sd, "/v1devices.json").c_str());
    TEST_ASSERT_TRUE(sd.rename("/v1devices.json", "/v1devices.json.prev"));
    writeFileFromString(sd, "/v1devices.json", "invalid");
    V1DeviceStore recovered;
    TEST_ASSERT_TRUE(recovered.begin(&sd));
    const auto records = recovered.listDevices();
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", records[0].address.c_str());
    TEST_ASSERT_EQUAL_STRING(catalogAddress(1).c_str(), records[1].address.c_str());
    TEST_ASSERT_EQUAL_STRING(catalogAddress(0).c_str(), records[2].address.c_str());
    TEST_ASSERT_FALSE(catalogContains(recovered, catalogAddress(2)));
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
    const V1DeviceMutationResult failed =
        devices.setDeviceName("AA:BB:CC:DD:EE:FF", "Changed");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(V1DeviceMutationStatus::NotCommitted),
                          static_cast<int>(failed.status));
    TEST_ASSERT_FALSE(devices.hasPendingSave());
    TEST_ASSERT_EQUAL_STRING("Original", devices.listDevices().front().name.c_str());
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
    TEST_ASSERT_EQUAL_UINT8(4u, upgraded["version"].as<uint8_t>());
    TEST_ASSERT_EQUAL_UINT32(1u, upgraded["generation"].as<uint32_t>());
    TEST_ASSERT_TRUE(upgraded["crc32"].is<uint32_t>());
}

void test_detector_snapshot_round_trips_with_partial_field_validity() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));

    V1DetectorSnapshot snapshot;
    snapshot.available = true;
    snapshot.capturedBootId = 12;
    snapshot.capturedUptimeMs = 3456;
    snapshot.sessionGeneration = 9;
    snapshot.captureTimedOut = true;
    snapshot.hasFirmwareVersion = true;
    snapshot.firmwareVersion = 41039;
    snapshot.hasUserBytes = true;
    snapshot.userBytes = {{0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xA5}};
    snapshot.hasMode = true;
    snapshot.mode = 'L';
    snapshot.hasDisplayOn = true;
    snapshot.displayOn = false;
    snapshot.hasCurrentVolume = true;
    snapshot.currentMainVolume = 8;
    snapshot.currentMutedVolume = 3;
    // Saved volume deliberately unavailable.

    TEST_ASSERT_TRUE(devices.recordSnapshotInMemory("aa-bb-cc-dd-ee-ff", snapshot));
    TEST_ASSERT_TRUE(devices.hasPendingSave());
    TEST_ASSERT_TRUE(devices.flushPendingSave());

    V1DeviceStore reloaded;
    TEST_ASSERT_TRUE(reloaded.begin(&fs));
    V1DeviceRecord record;
    TEST_ASSERT_TRUE(reloaded.getLatestSnapshot(record));
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", record.address.c_str());
    TEST_ASSERT_TRUE(record.snapshot.available);
    TEST_ASSERT_EQUAL_UINT32(12, record.snapshot.capturedBootId);
    TEST_ASSERT_EQUAL_UINT32(3456, record.snapshot.capturedUptimeMs);
    TEST_ASSERT_EQUAL_UINT32(9, record.snapshot.sessionGeneration);
    TEST_ASSERT_TRUE(record.snapshot.captureTimedOut);
    TEST_ASSERT_EQUAL_UINT32(41039, record.snapshot.firmwareVersion);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(snapshot.userBytes.data(), record.snapshot.userBytes.data(), 6);
    TEST_ASSERT_EQUAL_CHAR('L', record.snapshot.mode);
    TEST_ASSERT_FALSE(record.snapshot.displayOn);
    TEST_ASSERT_EQUAL_UINT8(8, record.snapshot.currentMainVolume);
    TEST_ASSERT_FALSE(record.snapshot.hasSavedVolume);
}

void test_worst_case_complete_sweep_catalog_round_trips_within_bounded_store() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    mockMillis = UINT32_MAX;
    const String escapedName(std::string(32, '\"').c_str());
    for (unsigned index = 0; index < 16; ++index) {
        const String address = catalogAddress(index);
        TEST_ASSERT_TRUE(devices.setDeviceName(address, escapedName));
    }

    V1DetectorSnapshot snapshot;
    snapshot.available = true;
    snapshot.capturedBootId = UINT32_MAX;
    snapshot.capturedUptimeMs = UINT32_MAX;
    snapshot.sessionGeneration = UINT32_MAX;
    snapshot.captureTimedOut = false;
    snapshot.hasFirmwareVersion = true;
    snapshot.firmwareVersion = UINT32_MAX;
    snapshot.hasUserBytes = true;
    snapshot.userBytes.fill(255);
    snapshot.hasMode = true;
    snapshot.mode = 'u';
    snapshot.hasDisplayOn = true;
    snapshot.displayOn = false;
    snapshot.hasBluetoothIndicator = true;
    snapshot.bluetoothIndicator = V1BluetoothIndicatorState::On;
    snapshot.hasCurrentVolume = true;
    snapshot.currentMainVolume = 9;
    snapshot.currentMutedVolume = 9;
    snapshot.hasSavedVolume = true;
    snapshot.savedMainVolume = 9;
    snapshot.savedMutedVolume = 9;
    snapshot.hasSweepSections = true;
    snapshot.sweepSectionCount = 15;
    for (uint8_t index = 0; index < snapshot.sweepSectionCount; ++index) {
        snapshot.sweepSections[index] = {index, snapshot.sweepSectionCount, 65534, 65535};
    }
    snapshot.hasMaxSweepIndex = true;
    snapshot.maxSweepIndex = 63;
    snapshot.hasSweepDefinitions = true;
    for (uint8_t index = 0; index <= snapshot.maxSweepIndex; ++index) {
        snapshot.sweepDefinitions[index] = {index, 65534, 65535};
    }
    for (unsigned index = 0; index < 16; ++index) {
        TEST_ASSERT_TRUE(devices.recordSnapshotInMemory(catalogAddress(index), snapshot));
    }
    for (const auto& record : devices.listDevices()) {
        TEST_ASSERT_EQUAL_UINT32(32, static_cast<uint32_t>(record.name.length()));
    }
    TEST_ASSERT_TRUE(devices.flushPendingSave());
    JsonDocument maximumStore;
    TEST_ASSERT_FALSE(deserializeJson(maximumStore, readFileToString(fs, "/v1devices.json")));
    maximumStore["generation"] = UINT32_MAX;
    maximumStore["crc32"] = deviceStoreContentCrc(maximumStore);
    String maximumStoreText;
    TEST_ASSERT_EQUAL_UINT(measureJson(maximumStore), serializeJson(maximumStore, maximumStoreText));
    writeFileFromString(fs, "/v1devices.json", maximumStoreText.c_str());
    const size_t storeSize = maximumStoreText.length();
    TEST_ASSERT_EQUAL_UINT32(70180, static_cast<uint32_t>(storeSize));
    TEST_ASSERT_LESS_THAN(72u * 1024u, storeSize);
    TEST_ASSERT_EQUAL_UINT32(3548, static_cast<uint32_t>(72u * 1024u - storeSize));

    V1DeviceStore reloaded;
    TEST_ASSERT_TRUE(reloaded.begin(&fs));
    const auto records = reloaded.listDevices();
    TEST_ASSERT_EQUAL_UINT32(16, static_cast<uint32_t>(records.size()));
    for (const auto& record : records) {
        TEST_ASSERT_TRUE(record.snapshot.hasSweepSections);
        TEST_ASSERT_EQUAL_UINT8(15, record.snapshot.sweepSectionCount);
        TEST_ASSERT_TRUE(record.snapshot.hasMaxSweepIndex);
        TEST_ASSERT_EQUAL_UINT8(63, record.snapshot.maxSweepIndex);
        TEST_ASSERT_TRUE(record.snapshot.hasSweepDefinitions);
        TEST_ASSERT_EQUAL_UINT16(65534, record.snapshot.sweepDefinitions[63].lowerMHz);
        TEST_ASSERT_EQUAL_UINT16(65535, record.snapshot.sweepDefinitions[63].upperMHz);
    }

    // Seed a genuine previously committed document. The production promote
    // helper removes .prev after a successful rename, so the fallback fixture
    // must model an interrupted/invalid live write explicitly.
    const std::string validStore = readFileToString(fs, "/v1devices.json");
    writeFileFromString(fs, "/v1devices.json.prev", validStore.c_str());
    const std::string oversized(72u * 1024u + 1u, ' ');
    writeFileFromString(fs, "/v1devices.json", oversized.c_str());
    V1DeviceStore recovered;
    TEST_ASSERT_TRUE(recovered.begin(&fs));
    TEST_ASSERT_EQUAL_UINT32(16, static_cast<uint32_t>(recovered.listDevices().size()));
    TEST_ASSERT_TRUE(recovered.listDevices()[0].snapshot.hasSweepDefinitions);
}

void test_null_sweep_section_slots_persist_without_losing_declared_selector_topology() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_TRUE(devices.upsertDevice("AA:BB:CC:DD:EE:FF"));

    V1DetectorSnapshot snapshot;
    snapshot.available = true;
    snapshot.capturedBootId = 7;
    snapshot.capturedUptimeMs = 100;
    snapshot.sessionGeneration = 2;
    snapshot.hasSweepSections = true;
    snapshot.sweepSectionCount = 3;
    snapshot.sweepSections = {{{0, 3, 23900, 25000}, {1, 3, 0, 0}, {2, 3, 33000, 37000}}};
    TEST_ASSERT_TRUE(devices.recordSnapshotInMemory("AA:BB:CC:DD:EE:FF", snapshot));
    TEST_ASSERT_TRUE(devices.flushPendingSave());

    V1DeviceStore reloaded;
    TEST_ASSERT_TRUE(reloaded.begin(&fs));
    const auto records = reloaded.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1, static_cast<uint32_t>(records.size()));
    TEST_ASSERT_TRUE(records[0].snapshot.hasSweepSections);
    TEST_ASSERT_EQUAL_UINT8(3, records[0].snapshot.sweepSectionCount);
    TEST_ASSERT_EQUAL_UINT8(1, records[0].snapshot.sweepSections[1].index);
    TEST_ASSERT_EQUAL_UINT8(3, records[0].snapshot.sweepSections[1].count);
    TEST_ASSERT_EQUAL_UINT16(0, records[0].snapshot.sweepSections[1].lowerMHz);
    TEST_ASSERT_EQUAL_UINT16(0, records[0].snapshot.sweepSections[1].upperMHz);
}

void test_malformed_claimed_complete_sweep_snapshot_is_not_serialized() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_TRUE(devices.upsertDevice("AA:BB:CC:DD:EE:FF"));
    const std::string before = readFileToString(fs, "/v1devices.json");
    V1DetectorSnapshot snapshot;
    snapshot.available = true;
    snapshot.hasSweepDefinitions = true;
    snapshot.hasMaxSweepIndex = false;
    TEST_ASSERT_FALSE(devices.recordSnapshotInMemory("AA:BB:CC:DD:EE:FF", snapshot));
    TEST_ASSERT_FALSE(devices.hasPendingSave());
    TEST_ASSERT_EQUAL_STRING(before.c_str(), readFileToString(fs, "/v1devices.json").c_str());
}

void test_invalid_claimed_scalar_snapshot_fields_are_rejected_before_staging() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_TRUE(devices.upsertDevice("AA:BB:CC:DD:EE:FF"));
    const std::string before = readFileToString(fs, "/v1devices.json");

    V1DetectorSnapshot snapshot;
    snapshot.available = true;
    snapshot.hasFirmwareVersion = true;
    snapshot.firmwareVersion = 0;
    TEST_ASSERT_FALSE(devices.recordSnapshotInMemory("AA:BB:CC:DD:EE:FF", snapshot));
    TEST_ASSERT_FALSE(devices.hasPendingSave());

    snapshot.hasFirmwareVersion = false;
    snapshot.hasMode = true;
    snapshot.mode = 'X';
    TEST_ASSERT_FALSE(devices.recordSnapshotInMemory("AA:BB:CC:DD:EE:FF", snapshot));
    TEST_ASSERT_FALSE(devices.hasPendingSave());
    TEST_ASSERT_EQUAL_STRING(before.c_str(), readFileToString(fs, "/v1devices.json").c_str());
}

void test_checksum_valid_semantic_snapshot_corruption_is_not_downgraded_to_absent() {
    for (int variant = 0; variant < 7; ++variant) {
        const std::filesystem::path root = g_tempRoot / ("semantic_" + std::to_string(variant));
        std::filesystem::create_directories(root);
        fs::FS fs(root);
        JsonDocument doc;
        JsonObject device = addValidV4Device(doc);
        JsonObject snapshot = device["snapshot"].as<JsonObject>();
        switch (variant) {
        case 0:
            device["snapshot"] = "not-an-object";
            break;
        case 1:
            snapshot["firmwareVersion"] = "41039";
            break;
        case 2:
            snapshot["currentVolume"]["main"] = 10;
            snapshot["currentVolume"]["muted"] = 1;
            break;
        case 3:
            snapshot["maxSweepIndex"] = 0;
            snapshot["sweepDefinitions"][0]["index"] = 0;
            snapshot["sweepDefinitions"][0]["lowerMHz"] = 0;
            snapshot["sweepDefinitions"][0]["upperMHz"] = 24000;
            break;
        case 4:
            device["defaultProfile"] = 4;
            break;
        case 5:
            device["lastSeenMs"] = "7000";
            break;
        case 6:
            snapshot["unexpectedClaim"] = true;
            break;
        }
        writeDeviceStoreDocument(fs, "/v1devices.json", doc);

        V1DeviceStore devices;
        TEST_ASSERT_TRUE(devices.begin(&fs));
        TEST_ASSERT_FALSE(devices.catalogReadable());
        std::vector<V1DeviceRecord> records;
        TEST_ASSERT_FALSE(devices.listDevicesChecked(records));
        TEST_ASSERT_TRUE(records.empty());
        V1DeviceRecord captured;
        TEST_ASSERT_FALSE(devices.getLatestSnapshot(captured));
    }
}

void test_semantically_invalid_primary_snapshot_recovers_from_valid_mirror() {
    const std::filesystem::path primaryRoot = g_tempRoot / "semantic_primary";
    const std::filesystem::path secondaryRoot = g_tempRoot / "semantic_secondary";
    std::filesystem::create_directories(primaryRoot);
    std::filesystem::create_directories(secondaryRoot);
    fs::FS primary(primaryRoot);
    fs::FS secondary(secondaryRoot);

    JsonDocument invalid;
    JsonObject invalidDevice = addValidV4Device(invalid, "Invalid primary");
    invalidDevice["snapshot"] = "not-an-object";
    writeDeviceStoreDocument(primary, "/v1devices.json", invalid);

    JsonDocument valid;
    addValidV4Device(valid, "Valid mirror");
    writeDeviceStoreDocument(secondary, "/v1devices.json", valid);

    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&primary, &secondary));
    TEST_ASSERT_TRUE(devices.catalogReadable());
    const auto records = devices.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(records.size()));
    TEST_ASSERT_EQUAL_STRING("Valid mirror", records[0].name.c_str());
    TEST_ASSERT_TRUE(records[0].snapshot.available);
    TEST_ASSERT_TRUE(records[0].snapshot.hasFirmwareVersion);
    TEST_ASSERT_EQUAL_STRING(readFileToString(primary, "/v1devices.json").c_str(),
                             readFileToString(secondary, "/v1devices.json").c_str());
}

void test_unreadable_catalog_blocks_every_read_derived_mutation_and_preserves_bytes() {
    fs::FS fs(g_tempRoot);
    JsonDocument invalid;
    JsonObject item = addValidV4Device(invalid, "Unreadable authority");
    item["snapshot"] = "not-an-object";
    writeDeviceStoreDocument(fs, "/v1devices.json", invalid);
    const std::string before = readFileToString(fs, "/v1devices.json");

    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_FALSE(devices.catalogReadable());
    V1DetectorSnapshot snapshot;
    snapshot.available = true;
    snapshot.hasFirmwareVersion = true;
    snapshot.firmwareVersion = 41039;
    TEST_ASSERT_FALSE(devices.bootstrapDevice("11:22:33:44:55:66", false));
    TEST_ASSERT_FALSE(devices.upsertDevice("11:22:33:44:55:66"));
    TEST_ASSERT_FALSE(devices.touchDeviceInMemory("11:22:33:44:55:66"));
    TEST_ASSERT_FALSE(devices.recordSnapshotInMemory("11:22:33:44:55:66", snapshot));
    TEST_ASSERT_FALSE(devices.setDeviceName("11:22:33:44:55:66", "New name"));
    TEST_ASSERT_FALSE(devices.setDeviceDefaultProfile("11:22:33:44:55:66", 2));
    TEST_ASSERT_FALSE(devices.removeDevice("AA:BB:CC:DD:EE:FF"));
    TEST_ASSERT_FALSE(devices.flushPendingSave());
    TEST_ASSERT_EQUAL_UINT8(0u, devices.getDeviceDefaultProfile("AA:BB:CC:DD:EE:FF"));
    TEST_ASSERT_FALSE(devices.catalogReadable());
    TEST_ASSERT_EQUAL_STRING(before.c_str(), readFileToString(fs, "/v1devices.json").c_str());
    TEST_ASSERT_FALSE(fs.exists("/v1devices.json.prev"));
}

void test_present_store_markers_are_exact_and_semantic_rollback_preserves_capture() {
    enum class Mutation { QuotedVersion, NullGeneration, FractionGeneration, NegativeGeneration, LegacyCrc };
    const Mutation mutations[] = {Mutation::QuotedVersion, Mutation::NullGeneration,
                                  Mutation::FractionGeneration, Mutation::NegativeGeneration,
                                  Mutation::LegacyCrc};
    for (size_t index = 0; index < sizeof(mutations) / sizeof(mutations[0]); ++index) {
        const std::filesystem::path caseRoot = g_tempRoot / ("marker_" + std::to_string(index));
        std::filesystem::create_directories(caseRoot);
        fs::FS fs(caseRoot);
        JsonDocument rollback;
        addValidV4Device(rollback, "Rollback capture");
        rollback["generation"] = 6;
        writeDeviceStoreDocument(fs, "/v1devices.json.prev", rollback);

        JsonDocument invalid;
        addValidV4Device(invalid, "Invalid live");
        invalid["crc32"] = deviceStoreContentCrc(invalid);
        switch (mutations[index]) {
        case Mutation::QuotedVersion: invalid["version"] = "4"; break;
        case Mutation::NullGeneration: invalid["generation"] = nullptr; break;
        case Mutation::FractionGeneration: invalid["generation"] = 1.5; break;
        case Mutation::NegativeGeneration: invalid["generation"] = -1; break;
        case Mutation::LegacyCrc:
            invalid["version"] = 1;
            invalid.remove("generation");
            break;
        }
        String invalidBytes;
        serializeJson(invalid, invalidBytes);
        writeFileFromString(fs, "/v1devices.json", invalidBytes.c_str());

        V1DeviceStore devices;
        TEST_ASSERT_TRUE(devices.begin(&fs));
        std::vector<V1DeviceRecord> records;
        TEST_ASSERT_TRUE(devices.listDevicesChecked(records));
        TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(records.size()));
        TEST_ASSERT_EQUAL_STRING("Rollback capture", records[0].name.c_str());
        TEST_ASSERT_TRUE(records[0].snapshot.available);
    }
}

void test_current_store_rejects_duplicate_addresses_and_uses_valid_mirror() {
    const std::filesystem::path primaryRoot = g_tempRoot / "duplicate_primary";
    const std::filesystem::path secondaryRoot = g_tempRoot / "duplicate_secondary";
    std::filesystem::create_directories(primaryRoot);
    std::filesystem::create_directories(secondaryRoot);
    fs::FS primary(primaryRoot);
    fs::FS secondary(secondaryRoot);

    JsonDocument duplicate;
    JsonObject first = addValidV4Device(duplicate, "First");
    JsonObject second = duplicate["devices"].as<JsonArray>().add<JsonObject>();
    second.set(first);
    second["name"] = "Composite must not be invented";
    writeDeviceStoreDocument(primary, "/v1devices.json", duplicate);

    JsonDocument valid;
    addValidV4Device(valid, "Valid mirror");
    valid["generation"] = 6;
    writeDeviceStoreDocument(secondary, "/v1devices.json", valid);

    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&primary, &secondary));
    const auto records = devices.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(records.size()));
    TEST_ASSERT_EQUAL_STRING("Valid mirror", records[0].name.c_str());
}

void test_current_store_rejects_unknown_root_or_device_keys_and_uses_valid_mirror() {
    for (bool unknownRoot : {true, false}) {
        const std::filesystem::path caseRoot = g_tempRoot / (unknownRoot ? "root" : "item");
        const std::filesystem::path sdRoot = caseRoot / "sd";
        const std::filesystem::path littleRoot = caseRoot / "little";
        std::filesystem::create_directories(sdRoot);
        std::filesystem::create_directories(littleRoot);
        fs::FS sd(sdRoot);
        fs::FS little(littleRoot);

        JsonDocument invalid;
        JsonObject invalidDevice = addValidV4Device(invalid, "Typo copy");
        invalid["generation"] = 9;
        if (unknownRoot) invalid["generatoin"] = 9;
        else invalidDevice["snapshpt"] = true;
        writeDeviceStoreDocument(sd, "/v1devices.json", invalid);

        JsonDocument valid;
        addValidV4Device(valid, "Preserved capture");
        valid["generation"] = 4;
        writeDeviceStoreDocument(little, "/v1devices.json", valid);

        V1DeviceStore recovered;
        TEST_ASSERT_TRUE(recovered.begin(&sd, &little));
        TEST_ASSERT_TRUE(recovered.catalogReadable());
        V1DeviceRecord latest;
        TEST_ASSERT_TRUE(recovered.getLatestSnapshot(latest));
        TEST_ASSERT_EQUAL_STRING("Preserved capture", latest.name.c_str());
        TEST_ASSERT_TRUE(latest.snapshot.available);
        TEST_ASSERT_EQUAL_UINT32(10u, latest.snapshot.capturedBootId);
    }
}

void test_current_store_rejects_more_than_supported_device_count_before_copying() {
    fs::FS fs(g_tempRoot);
    JsonDocument overCap;
    overCap["version"] = 4;
    overCap["generation"] = 7;
    JsonArray devices = overCap["devices"].to<JsonArray>();
    for (unsigned index = 0; index < 17; ++index) {
        JsonObject device = devices.add<JsonObject>();
        device["address"] = catalogAddress(index);
        device["name"] = "Over-cap";
        device["defaultProfile"] = 0;
        device["lastSeenMs"] = index;
    }
    writeDeviceStoreDocument(fs, "/v1devices.json", overCap);
    V1DeviceStore loaded;
    TEST_ASSERT_TRUE(loaded.begin(&fs));
    TEST_ASSERT_FALSE(loaded.catalogReadable());
    std::vector<V1DeviceRecord> records;
    TEST_ASSERT_FALSE(loaded.listDevicesChecked(records));
    TEST_ASSERT_TRUE(records.empty());
}

void test_generation_exhaustion_never_promotes_zero_or_overwrites_valid_copies() {
    fs::FS fs(g_tempRoot);
    JsonDocument maximum;
    addValidV4Device(maximum, "Maximum generation");
    maximum["generation"] = 0xFFFFFFFFu;
    writeDeviceStoreDocument(fs, "/v1devices.json", maximum);
    const std::string before = readFileToString(fs, "/v1devices.json");

    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    V1DetectorSnapshot changed;
    changed.available = true;
    changed.capturedBootId = 2;
    changed.capturedUptimeMs = 3;
    changed.sessionGeneration = 4;
    changed.captureTimedOut = false;
    changed.hasMode = true;
    changed.mode = 'C';
    TEST_ASSERT_TRUE(devices.recordSnapshotInMemory("AA:BB:CC:DD:EE:FF", changed));
    TEST_ASSERT_FALSE(devices.flushPendingSave());
    TEST_ASSERT_EQUAL_STRING(before.c_str(), readFileToString(fs, "/v1devices.json").c_str());
    TEST_ASSERT_FALSE(fs.exists("/v1devices.tmp"));

    const std::filesystem::path secondaryRoot = g_tempRoot / "max_secondary";
    std::filesystem::create_directories(secondaryRoot);
    fs::FS secondary(secondaryRoot);
    JsonDocument divergent;
    addValidV4Device(divergent, "Divergent maximum");
    divergent["generation"] = 0xFFFFFFFFu;
    writeDeviceStoreDocument(secondary, "/v1devices.json", divergent);
    const std::string primaryBefore = readFileToString(fs, "/v1devices.json");
    const std::string secondaryBefore = readFileToString(secondary, "/v1devices.json");
    V1DeviceStore conflicted;
    TEST_ASSERT_TRUE(conflicted.begin(&fs, &secondary));
    TEST_ASSERT_FALSE(conflicted.catalogReadable());
    TEST_ASSERT_EQUAL_STRING(primaryBefore.c_str(), readFileToString(fs, "/v1devices.json").c_str());
    TEST_ASSERT_EQUAL_STRING(secondaryBefore.c_str(), readFileToString(secondary, "/v1devices.json").c_str());
}

void test_all_canonical_mode_glyphs_round_trip_in_persisted_snapshot() {
    const char modes[] = {'A', 'C', 'U', 'l', 'c', 'u', 'L'};
    for (char mode : modes) {
        const std::filesystem::path caseRoot = g_tempRoot / ("mode_" + std::to_string(static_cast<int>(mode)));
        std::filesystem::create_directories(caseRoot);
        fs::FS fs(caseRoot);
        V1DeviceStore devices;
        TEST_ASSERT_TRUE(devices.begin(&fs));
        V1DetectorSnapshot captured;
        captured.available = true;
        captured.capturedBootId = 10;
        captured.capturedUptimeMs = 20;
        captured.sessionGeneration = 3;
        captured.captureTimedOut = false;
        captured.hasMode = true;
        captured.mode = mode;
        TEST_ASSERT_TRUE(devices.recordSnapshotInMemory("AA:BB:CC:DD:EE:FF", captured));
        TEST_ASSERT_TRUE(devices.flushPendingSave());
        V1DeviceStore rebooted;
        TEST_ASSERT_TRUE(rebooted.begin(&fs));
        V1DeviceRecord restored;
        TEST_ASSERT_TRUE(rebooted.getLatestSnapshot(restored));
        TEST_ASSERT_EQUAL_CHAR(mode, restored.snapshot.mode);
    }
}

void test_device_store_crc_streams_without_bulk_allocations() {
    JsonDocument doc;
    doc["version"] = 4;
    doc["generation"] = 19;
    JsonObject device = doc["devices"].to<JsonArray>().add<JsonObject>();
    device["address"] = "AA:BB:CC:DD:EE:FF";
    device["name"] = "Road";

    mock_reset_heap_caps_tracking();
    g_mock_heap_caps_fail_all_allocations = true;
    uint32_t crc = 0;
    TEST_ASSERT_TRUE(deviceStoreContentCrc(doc, crc));
    g_mock_heap_caps_fail_all_allocations = false;
    TEST_ASSERT_NOT_EQUAL(0u, crc);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_heap_caps_realloc_calls);
}

void test_device_store_psram_exhaustion_preserves_live_catalog_and_retries() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_TRUE(devices.setDeviceName("AA:BB:CC:DD:EE:FF", "Preserved"));
    const std::string committed = readFileToString(fs, "/v1devices.json");

    V1DetectorSnapshot snapshot;
    snapshot.available = true;
    snapshot.hasMode = true;
    snapshot.mode = 'A';
    TEST_ASSERT_TRUE(devices.recordSnapshotInMemory("AA:BB:CC:DD:EE:FF", snapshot));

    mock_reset_heap_caps_tracking();
    g_mock_heap_caps_fail_all_allocations = true;
    TEST_ASSERT_FALSE(devices.flushPendingSave());
    g_mock_heap_caps_fail_all_allocations = false;
    TEST_ASSERT_GREATER_THAN(0u, g_mock_heap_caps_realloc_calls + g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_EQUAL_STRING(committed.c_str(), readFileToString(fs, "/v1devices.json").c_str());
    TEST_ASSERT_FALSE(fs.exists("/v1devices.tmp"));

    TEST_ASSERT_TRUE(devices.flushPendingSave());
    TEST_ASSERT_NOT_EQUAL(0, committed.compare(readFileToString(fs, "/v1devices.json")));
}

void test_device_store_boot_oom_does_not_import_legacy_or_clobber_catalog() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore source;
    TEST_ASSERT_TRUE(source.begin(&fs));
    TEST_ASSERT_TRUE(source.setDeviceName("AA:BB:CC:DD:EE:FF", "PSRAM authority"));
    const std::string committed = readFileToString(fs, "/v1devices.json");
    writeFileFromString(fs, "/known_v1.txt", "11:22:33:44:55:66\n");

    mock_reset_heap_caps_tracking();
    g_mock_heap_caps_fail_all_allocations = true;
    V1DeviceStore starved;
    TEST_ASSERT_TRUE(starved.begin(&fs));
    g_mock_heap_caps_fail_all_allocations = false;
    TEST_ASSERT_TRUE(starved.listDevices().empty());
    TEST_ASSERT_FALSE(starved.catalogReadable());
    std::vector<V1DeviceRecord> checked;
    TEST_ASSERT_FALSE(starved.listDevicesChecked(checked));
    V1DeviceRecord unavailableSnapshot;
    TEST_ASSERT_FALSE(starved.getLatestSnapshot(unavailableSnapshot));
    TEST_ASSERT_EQUAL_STRING(committed.c_str(), readFileToString(fs, "/v1devices.json").c_str());

    V1DeviceStore recovered;
    TEST_ASSERT_TRUE(recovered.begin(&fs));
    TEST_ASSERT_TRUE(recovered.catalogReadable());
    TEST_ASSERT_EQUAL_UINT(1, recovered.listDevices().size());
    TEST_ASSERT_EQUAL_STRING("PSRAM authority", recovered.listDevices()[0].name.c_str());
}

void test_unavailable_live_address_cannot_overwrite_prior_detector_snapshot() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));

    V1DetectorSnapshot prior;
    prior.available = true;
    prior.capturedBootId = 10;
    TEST_ASSERT_TRUE(devices.recordSnapshotInMemory("AA:BB:CC:DD:EE:FF", prior));
    TEST_ASSERT_TRUE(devices.flushPendingSave());

    V1DetectorSnapshot unrelatedSession;
    unrelatedSession.available = true;
    unrelatedSession.capturedBootId = 11;
    TEST_ASSERT_FALSE(devices.recordSnapshotInMemory("", unrelatedSession));

    V1DeviceRecord latest;
    TEST_ASSERT_TRUE(devices.getLatestSnapshot(latest));
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", latest.address.c_str());
    TEST_ASSERT_EQUAL_UINT32(10u, latest.snapshot.capturedBootId);

    // A non-null live identity must fail before the stale fallback branch if
    // either std::string or Arduino String materialization is unavailable.
    std::ifstream driveSource(std::string(PROJECT_DIR) + "/src/drive_runtime.cpp");
    const std::string source((std::istreambuf_iterator<char>(driveSource)), std::istreambuf_iterator<char>());
    const size_t connectedBranch = source.find("if (!connected.isNull())",
                                               source.find("void DriveRuntime::onV1Connected()"));
    const size_t exactFailure = source.find("reason=device_identity_unavailable", connectedBranch);
    const size_t fallbackBranch = source.find("if (connected.isNull())", connectedBranch);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, connectedBranch);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, exactFailure);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, fallbackBranch);
    TEST_ASSERT_LESS_THAN(fallbackBranch, exactFailure);
    TEST_ASSERT_NOT_EQUAL(std::string::npos, source.find("catch (const std::bad_alloc&)", connectedBranch));
    const size_t snapshotBlock = source.find("if (linkAddress.length() > 0 && self.devices_.isReady())",
                                             source.find("bounded connect-followup read"));
    TEST_ASSERT_NOT_EQUAL(std::string::npos, snapshotBlock);
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("recordSnapshotInMemory(linkAddress, snapshot)", snapshotBlock));
    TEST_ASSERT_NOT_EQUAL(std::string::npos,
                          source.find("reason=device_snapshot_unavailable", snapshotBlock));
}

void test_default_profile_lookup_distinguishes_no_override_from_unavailable() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));

    V1DeviceDefaultProfileResult lookup =
        devices.getDeviceDefaultProfileChecked("AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(V1DeviceDefaultProfileStatus::NoOverride),
                          static_cast<int>(lookup.status));
    TEST_ASSERT_EQUAL_UINT8(0u, lookup.profile);
    TEST_ASSERT_TRUE(devices.setDeviceDefaultProfile("AA:BB:CC:DD:EE:FF", 0));
    lookup = devices.getDeviceDefaultProfileChecked("AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(V1DeviceDefaultProfileStatus::NoOverride),
                          static_cast<int>(lookup.status));

    TEST_ASSERT_TRUE(devices.setDeviceDefaultProfile("AA:BB:CC:DD:EE:FF", 3));
    lookup = devices.getDeviceDefaultProfileChecked("AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(V1DeviceDefaultProfileStatus::Found),
                          static_cast<int>(lookup.status));
    TEST_ASSERT_EQUAL_UINT8(3u, lookup.profile);
    lookup = devices.getDeviceDefaultProfileChecked("not-an-address");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(V1DeviceDefaultProfileStatus::Unavailable),
                          static_cast<int>(lookup.status));

    writeFileFromString(fs, "/v1devices.json", "invalid");
    V1DeviceStore unavailable;
    TEST_ASSERT_TRUE(unavailable.begin(&fs));
    lookup = unavailable.getDeviceDefaultProfileChecked("AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(V1DeviceDefaultProfileStatus::Unavailable),
                          static_cast<int>(lookup.status));
}

void test_v2_device_catalog_upgrades_without_inventing_a_snapshot() {
    fs::FS fs(g_tempRoot);
    JsonDocument v2;
    v2["version"] = 2;
    v2["generation"] = 4;
    JsonObject device = v2["devices"].to<JsonArray>().add<JsonObject>();
    device["address"] = "AA:BB:CC:DD:EE:FF";
    device["name"] = "Existing";
    device["defaultProfile"] = 2;
    device["lastSeenMs"] = 9000;
    v2["crc32"] = deviceStoreContentCrc(v2);
    String serialized;
    serializeJson(v2, serialized);
    writeFileFromString(fs, "/v1devices.json", serialized.c_str());

    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    V1DeviceRecord snapshot;
    TEST_ASSERT_FALSE(devices.getLatestSnapshot(snapshot));
    TEST_ASSERT_EQUAL_STRING("Existing", devices.listDevices()[0].name.c_str());

    JsonDocument upgraded;
    TEST_ASSERT_FALSE(deserializeJson(upgraded, readFileToString(fs, "/v1devices.json")));
    TEST_ASSERT_EQUAL_UINT8(4, upgraded["version"].as<uint8_t>());
    TEST_ASSERT_FALSE(upgraded["devices"][0]["snapshot"].is<JsonObject>());
}

void test_corrupt_newer_v2_catalog_is_rejected_and_recovered_from_valid_mirror() {
    const std::filesystem::path sdRoot = g_tempRoot / "sd";
    const std::filesystem::path littleRoot = g_tempRoot / "little";
    std::filesystem::create_directories(sdRoot);
    std::filesystem::create_directories(littleRoot);
    fs::FS sd(sdRoot);
    fs::FS little(littleRoot);

    JsonDocument primaryV2;
    primaryV2["version"] = 2;
    primaryV2["generation"] = 9;
    JsonObject primaryDevice = primaryV2["devices"].to<JsonArray>().add<JsonObject>();
    primaryDevice["address"] = "AA:BB:CC:DD:EE:FF";
    primaryDevice["name"] = "Checksum source";
    primaryDevice["defaultProfile"] = 3;
    primaryDevice["lastSeenMs"] = 9000;
    primaryV2["crc32"] = deviceStoreContentCrc(primaryV2);
    // Corrupt checksum-covered content after computing the real v2 checksum.
    primaryDevice["name"] = "Tampered newer copy";
    String primarySerialized;
    serializeJson(primaryV2, primarySerialized);
    writeFileFromString(sd, "/v1devices.json", primarySerialized.c_str());

    JsonDocument secondaryV2;
    secondaryV2["version"] = 2;
    secondaryV2["generation"] = 4;
    JsonObject secondaryDevice = secondaryV2["devices"].to<JsonArray>().add<JsonObject>();
    secondaryDevice["address"] = "AA:BB:CC:DD:EE:FF";
    secondaryDevice["name"] = "Recovered valid copy";
    secondaryDevice["defaultProfile"] = 1;
    secondaryDevice["lastSeenMs"] = 4000;
    secondaryV2["crc32"] = deviceStoreContentCrc(secondaryV2);
    String secondarySerialized;
    serializeJson(secondaryV2, secondarySerialized);
    writeFileFromString(little, "/v1devices.json", secondarySerialized.c_str());

    V1DeviceStore recovered;
    TEST_ASSERT_TRUE(recovered.begin(&sd, &little));
    const std::vector<V1DeviceRecord> records = recovered.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(records.size()));
    TEST_ASSERT_EQUAL_STRING("Recovered valid copy", records[0].name.c_str());
    TEST_ASSERT_EQUAL_UINT8(1u, records[0].defaultProfile);
    TEST_ASSERT_EQUAL_STRING(readFileToString(sd, "/v1devices.json").c_str(),
                             readFileToString(little, "/v1devices.json").c_str());

    JsonDocument repaired;
    TEST_ASSERT_FALSE(deserializeJson(repaired, readFileToString(sd, "/v1devices.json")));
    TEST_ASSERT_EQUAL_UINT8(4u, repaired["version"].as<uint8_t>());
    TEST_ASSERT_EQUAL_UINT32(10u, repaired["generation"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(deviceStoreContentCrc(repaired), repaired["crc32"].as<uint32_t>());
}

void test_semantic_crc_failure_recovers_same_filesystem_rollback() {
    fs::FS fs(g_tempRoot);

    JsonDocument committed;
    committed["version"] = 3;
    committed["generation"] = 7;
    JsonObject committedDevice = committed["devices"].to<JsonArray>().add<JsonObject>();
    committedDevice["address"] = "AA:BB:CC:DD:EE:FF";
    committedDevice["name"] = "Committed rollback";
    committedDevice["defaultProfile"] = 2;
    committedDevice["lastSeenMs"] = 7000;
    committed["crc32"] = deviceStoreContentCrc(committed);
    String committedSerialized;
    serializeJson(committed, committedSerialized);
    writeFileFromString(fs, "/v1devices.json.prev", committedSerialized.c_str());

    // Keep the live file syntactically valid while changing checksum-covered
    // content without updating its committed checksum.
    committedDevice["name"] = "Tampered live copy";
    String tamperedSerialized;
    serializeJson(committed, tamperedSerialized);
    writeFileFromString(fs, "/v1devices.json", tamperedSerialized.c_str());

    V1DeviceStore recovered;
    TEST_ASSERT_TRUE(recovered.begin(&fs));
    const std::vector<V1DeviceRecord> records = recovered.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(records.size()));
    TEST_ASSERT_EQUAL_STRING("Committed rollback", records[0].name.c_str());
    TEST_ASSERT_EQUAL_UINT8(2u, records[0].defaultProfile);

    JsonDocument repaired;
    TEST_ASSERT_FALSE(deserializeJson(repaired, readFileToString(fs, "/v1devices.json")));
    TEST_ASSERT_EQUAL_UINT8(4u, repaired["version"].as<uint8_t>());
    TEST_ASSERT_EQUAL_UINT32(7u, repaired["generation"].as<uint32_t>());
    TEST_ASSERT_EQUAL_UINT32(deviceStoreContentCrc(repaired), repaired["crc32"].as<uint32_t>());
    TEST_ASSERT_EQUAL_STRING("Committed rollback", repaired["devices"][0]["name"].as<const char*>());
}

void test_failed_recovery_promotion_preserves_only_valid_rollback() {
    fs::FS fs(g_tempRoot);

    JsonDocument committed;
    committed["version"] = 3;
    committed["generation"] = 8;
    JsonObject committedDevice = committed["devices"].to<JsonArray>().add<JsonObject>();
    committedDevice["address"] = "AA:BB:CC:DD:EE:FF";
    committedDevice["name"] = "Only valid copy";
    committedDevice["defaultProfile"] = 2;
    committedDevice["lastSeenMs"] = 8000;
    committed["crc32"] = deviceStoreContentCrc(committed);
    String committedSerialized;
    serializeJson(committed, committedSerialized);
    writeFileFromString(fs, "/v1devices.json.prev", committedSerialized.c_str());
    writeFileFromString(fs, "/v1devices.json", "{known-bad-live");

    // In the recovery path the next rename is the verified temp candidate's
    // promotion. A failed promotion must leave the proven rollback untouched.
    fs::mock_fail_next_rename();
    V1DeviceStore recovered;
    TEST_ASSERT_TRUE(recovered.begin(&fs));
    const std::vector<V1DeviceRecord> records = recovered.listDevices();
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(records.size()));
    TEST_ASSERT_EQUAL_STRING("Only valid copy", records[0].name.c_str());
    TEST_ASSERT_TRUE(recovered.hasPendingSave());
    TEST_ASSERT_TRUE(fs.exists("/v1devices.json.prev"));
    TEST_ASSERT_EQUAL_STRING(committedSerialized.c_str(),
                             readFileToString(fs, "/v1devices.json.prev").c_str());
    TEST_ASSERT_FALSE(fs.exists("/v1devices.json"));

    fs::mock_reset_fs_rename_state();
    V1DeviceStore afterReboot;
    TEST_ASSERT_TRUE(afterReboot.begin(&fs));
    TEST_ASSERT_EQUAL_UINT32(1u, static_cast<uint32_t>(afterReboot.listDevices().size()));
    TEST_ASSERT_EQUAL_STRING("Only valid copy", afterReboot.listDevices()[0].name.c_str());
    TEST_ASSERT_TRUE(fs.exists("/v1devices.json"));
    TEST_ASSERT_FALSE(fs.exists("/v1devices.json.prev"));

    JsonDocument repaired;
    TEST_ASSERT_FALSE(deserializeJson(repaired, readFileToString(fs, "/v1devices.json")));
    TEST_ASSERT_EQUAL_UINT32(deviceStoreContentCrc(repaired), repaired["crc32"].as<uint32_t>());
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
    const V1DeviceMutationResult committed =
        devices.setDeviceName("AA:BB:CC:DD:EE:FF", "After");
    TEST_ASSERT_EQUAL_INT(static_cast<int>(V1DeviceMutationStatus::PrimaryCommittedMirrorPending),
                          static_cast<int>(committed.status));
    TEST_ASSERT_TRUE(devices.hasPendingSave());
    TEST_ASSERT_NOT_EQUAL(0, readFileToString(sd, "/v1devices.json").compare(
                                 readFileToString(little, "/v1devices.json")));

    fs::mock_reset_fs_rename_state();
    TEST_ASSERT_TRUE(devices.flushPendingSave());
    TEST_ASSERT_FALSE(devices.hasPendingSave());
    TEST_ASSERT_EQUAL_STRING(readFileToString(sd, "/v1devices.json").c_str(),
                             readFileToString(little, "/v1devices.json").c_str());
}

void test_deleted_last_legacy_device_stays_deleted_after_reload() {
    fs::FS fs(g_tempRoot);
    writeFileFromString(fs, "/known_v1.txt", "AA:BB:CC:DD:EE:FF\n");
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_EQUAL_UINT(1, devices.listDevices().size());
    TEST_ASSERT_TRUE(devices.removeDevice("AA:BB:CC:DD:EE:FF"));
    V1DeviceStore rebooted;
    TEST_ASSERT_TRUE(rebooted.begin(&fs));
    TEST_ASSERT_TRUE(rebooted.listDevices().empty());
    TEST_ASSERT_TRUE(fs.exists("/known_v1.txt"));
}

void test_legacy_name_truncates_only_at_utf8_boundary_and_reboots_readably() {
    fs::FS fs(g_tempRoot);
    writeFileFromString(fs, "/known_v1.txt", "AA:BB:CC:DD:EE:FF\n");
    const std::string expected(31u, 'A');
    writeFileFromBytes(fs, "/known_v1_names.txt",
                       std::string("AA:BB:CC:DD:EE:FF|") + expected + "\xC3\xA9\n");

    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_TRUE(devices.catalogReadable());
    TEST_ASSERT_EQUAL_UINT(1u, devices.listDevices().size());
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), devices.listDevices()[0].name.c_str());

    V1DeviceStore rebooted;
    TEST_ASSERT_TRUE(rebooted.begin(&fs));
    TEST_ASSERT_TRUE(rebooted.catalogReadable());
    TEST_ASSERT_EQUAL_UINT(1u, rebooted.listDevices().size());
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), rebooted.listDevices()[0].name.c_str());
}

void test_invalid_legacy_name_aborts_whole_migration_without_partial_catalog() {
    for (const std::string& badName : {std::string("Bad\x01Name", 8u),
                                      std::string("Bad\xFFName", 8u)}) {
        const std::filesystem::path root = nextTempRoot();
        std::filesystem::remove_all(root);
        std::filesystem::create_directories(root);
        fs::FS fs(root);
        writeFileFromString(fs, "/known_v1.txt",
                            "AA:BB:CC:DD:EE:FF\n11:22:33:44:55:66\n");
        writeFileFromBytes(fs, "/known_v1_names.txt",
                           std::string("AA:BB:CC:DD:EE:FF|Good\n11:22:33:44:55:66|") +
                               badName + "\n");

        V1DeviceStore devices;
        TEST_ASSERT_TRUE(devices.begin(&fs));
        TEST_ASSERT_TRUE(devices.listDevices().empty());
        TEST_ASSERT_FALSE(fs.exists("/v1devices.json"));
        std::filesystem::remove_all(root);
    }
}

void test_failed_legacy_catalog_persist_is_unavailable_until_clean_retry() {
    fs::FS fs(g_tempRoot);
    writeFileFromString(fs, "/known_v1.txt", "AA:BB:CC:DD:EE:FF\n");
    writeFileFromString(fs, "/known_v1_names.txt", "AA:BB:CC:DD:EE:FF|Legacy\n");
    fs::mock_set_fs_write_budget(0);
    V1DeviceStore failed;
    TEST_ASSERT_TRUE(failed.begin(&fs));
    TEST_ASSERT_FALSE(failed.catalogReadable());
    std::vector<V1DeviceRecord> unavailable;
    TEST_ASSERT_FALSE(failed.listDevicesChecked(unavailable));
    TEST_ASSERT_TRUE(failed.listDevices().empty());
    fs::mock_reset_fs_write_budget();

    V1DeviceStore retry;
    TEST_ASSERT_TRUE(retry.begin(&fs));
    TEST_ASSERT_TRUE(retry.catalogReadable());
    TEST_ASSERT_EQUAL_UINT(1u, retry.listDevices().size());
    TEST_ASSERT_EQUAL_STRING("Legacy", retry.listDevices()[0].name.c_str());
}

void test_empty_legacy_v1_catalog_overrides_legacy_text_import() {
    fs::FS fs(g_tempRoot);
    writeFileFromString(fs, "/v1devices.json", "{\"version\":1,\"devices\":[]}");
    writeFileFromString(fs, "/known_v1.txt", "AA:BB:CC:DD:EE:FF\n");
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_TRUE(devices.listDevices().empty());
    JsonDocument upgraded;
    TEST_ASSERT_FALSE(deserializeJson(upgraded, readFileToString(fs, "/v1devices.json")));
    TEST_ASSERT_EQUAL_INT(4, upgraded["version"].as<int>());
    TEST_ASSERT_TRUE(upgraded["crc32"].is<uint32_t>());
}

void test_newer_empty_littlefs_catalog_overrides_stale_sd_and_legacy_text() {
    std::filesystem::create_directories(g_tempRoot / "sd");
    std::filesystem::create_directories(g_tempRoot / "little");
    fs::FS sd(g_tempRoot / "sd"), little(g_tempRoot / "little");
    V1DeviceStore initial;
    TEST_ASSERT_TRUE(initial.begin(&sd, &little));
    TEST_ASSERT_TRUE(initial.upsertDevice("AA:BB:CC:DD:EE:FF"));
    V1DeviceStore offline;
    TEST_ASSERT_TRUE(offline.begin(&little));
    TEST_ASSERT_TRUE(offline.removeDevice("AA:BB:CC:DD:EE:FF"));
    writeFileFromString(sd, "/known_v1.txt", "AA:BB:CC:DD:EE:FF\n");
    V1DeviceStore returned;
    TEST_ASSERT_TRUE(returned.begin(&sd, &little));
    TEST_ASSERT_TRUE(returned.listDevices().empty());
    TEST_ASSERT_EQUAL_STRING(readFileToString(little, "/v1devices.json").c_str(),
                             readFileToString(sd, "/v1devices.json").c_str());
}

void test_missing_and_invalid_catalogs_still_import_legacy_text() {
    fs::FS fs(g_tempRoot);
    writeFileFromString(fs, "/known_v1.txt", "AA:BB:CC:DD:EE:FF\n");
    for (bool invalid : {false, true}) {
        if (invalid) writeFileFromString(fs, "/v1devices.json", "invalid");
        V1DeviceStore devices;
        TEST_ASSERT_TRUE(devices.begin(&fs));
        TEST_ASSERT_EQUAL_UINT(1, devices.listDevices().size());
    }
}

void test_empty_rollback_catalog_remains_authoritative() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_TRUE(devices.upsertDevice("AA:BB:CC:DD:EE:FF"));
    TEST_ASSERT_TRUE(devices.removeDevice("AA:BB:CC:DD:EE:FF"));
    TEST_ASSERT_TRUE(fs.rename("/v1devices.json", "/v1devices.json.prev"));
    writeFileFromString(fs, "/v1devices.json", "invalid");
    writeFileFromString(fs, "/known_v1.txt", "AA:BB:CC:DD:EE:FF\n");
    V1DeviceStore rebooted;
    TEST_ASSERT_TRUE(rebooted.begin(&fs));
    TEST_ASSERT_TRUE(rebooted.listDevices().empty());
    TEST_ASSERT_TRUE(rebooted.bootstrapDevice("AA:BB:CC:DD:EE:FF", false));
    TEST_ASSERT_TRUE(rebooted.listDevices().empty());
}

void test_bootstrap_distinguishes_history_from_actual_degraded_connection() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_FALSE(devices.bootstrapDevice("AA:BB:CC:DD:EE:FF", true));
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_FALSE(devices.bootstrapDevice("invalid", false));
    TEST_ASSERT_TRUE(devices.bootstrapDevice("AA:BB:CC:DD:EE:FF", false));
    TEST_ASSERT_EQUAL_UINT(1, devices.listDevices().size());
    TEST_ASSERT_TRUE(devices.removeDevice("AA:BB:CC:DD:EE:FF"));
    const std::string emptyCatalog = readFileToString(fs, "/v1devices.json");
    TEST_ASSERT_TRUE(devices.bootstrapDevice("AA:BB:CC:DD:EE:FF", false));
    TEST_ASSERT_EQUAL_STRING(emptyCatalog.c_str(), readFileToString(fs, "/v1devices.json").c_str());
    // A later actual connection recorded while this valid catalog was offline
    // is recovery data, even when the old catalog was deliberately empty.
    TEST_ASSERT_TRUE(devices.bootstrapDevice("11:22:33:44:55:66", true));
    V1DeviceStore rebooted;
    TEST_ASSERT_TRUE(rebooted.begin(&fs));
    TEST_ASSERT_EQUAL_UINT(1, rebooted.listDevices().size());
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", rebooted.listDevices()[0].address.c_str());
}

void test_delete_preserves_other_devices_and_actual_rediscovery_uses_defaults() {
    fs::FS fs(g_tempRoot);
    V1DeviceStore devices;
    TEST_ASSERT_TRUE(devices.begin(&fs));
    TEST_ASSERT_TRUE(devices.setDeviceName("AA:BB:CC:DD:EE:FF", "Old metadata"));
    TEST_ASSERT_TRUE(devices.setDeviceDefaultProfile("AA:BB:CC:DD:EE:FF", 3));
    TEST_ASSERT_TRUE(devices.setDeviceName("11:22:33:44:55:66", "Survivor"));
    writeFileFromString(fs, "/known_v1.txt", "AA:BB:CC:DD:EE:FF\n");
    TEST_ASSERT_TRUE(devices.removeDevice("AA:BB:CC:DD:EE:FF"));
    V1DeviceStore rebooted;
    TEST_ASSERT_TRUE(rebooted.begin(&fs));
    TEST_ASSERT_TRUE(rebooted.bootstrapDevice("AA:BB:CC:DD:EE:FF", false));
    TEST_ASSERT_EQUAL_UINT(1, rebooted.listDevices().size());
    TEST_ASSERT_EQUAL_STRING("Survivor", rebooted.listDevices()[0].name.c_str());
    TEST_ASSERT_TRUE(rebooted.touchDeviceInMemory("AA:BB:CC:DD:EE:FF"));
    TEST_ASSERT_TRUE(rebooted.flushPendingSave());
    for (const auto& device : rebooted.listDevices()) {
        if (device.address == "AA:BB:CC:DD:EE:FF") {
            TEST_ASSERT_EQUAL_STRING("", device.name.c_str());
            TEST_ASSERT_EQUAL_UINT8(0, device.defaultProfile);
        }
    }
    TEST_ASSERT_EQUAL_UINT(2, rebooted.listDevices().size());
}

void test_both_boot_paths_use_catalog_bootstrap_and_connection_still_discovers() {
    for (const char* path : {"/src/drive_runtime.cpp", "/src/maintenance_runtime.cpp"}) {
        std::ifstream input(std::string(PROJECT_DIR) + path);
        const std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        TEST_ASSERT_TRUE(source.find("bootstrapDevice(restoredLastKnownV1, degradedFallback.length() > 0)") != std::string::npos);
        if (std::string(path).find("drive_runtime") != std::string::npos) {
            TEST_ASSERT_TRUE(source.find("self.devices_.touchDeviceInMemory(*profileAddress)",
                                        source.find("void DriveRuntime::onV1Connected()")) != std::string::npos);
        }
    }
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_new_device_survives_cross_boot_capacity_for_every_insertion_path);
    RUN_TEST(test_recency_survives_successive_boots_and_existing_device_metadata_edits);
    RUN_TEST(test_device_name_rejects_non_roundtrippable_bytes_without_mutation);
    RUN_TEST(test_device_recency_survives_mirror_recovery_and_millis_wrap);
    RUN_TEST(test_deleted_last_legacy_device_stays_deleted_after_reload);
    RUN_TEST(test_legacy_name_truncates_only_at_utf8_boundary_and_reboots_readably);
    RUN_TEST(test_invalid_legacy_name_aborts_whole_migration_without_partial_catalog);
    RUN_TEST(test_failed_legacy_catalog_persist_is_unavailable_until_clean_retry);
    RUN_TEST(test_empty_legacy_v1_catalog_overrides_legacy_text_import);
    RUN_TEST(test_newer_empty_littlefs_catalog_overrides_stale_sd_and_legacy_text);
    RUN_TEST(test_missing_and_invalid_catalogs_still_import_legacy_text);
    RUN_TEST(test_empty_rollback_catalog_remains_authoritative);
    RUN_TEST(test_bootstrap_distinguishes_history_from_actual_degraded_connection);
    RUN_TEST(test_delete_preserves_other_devices_and_actual_rediscovery_uses_defaults);
    RUN_TEST(test_both_boot_paths_use_catalog_bootstrap_and_connection_still_discovers);
    RUN_TEST(test_touch_defers_device_write_until_flush_and_reloads_saved_record);
    RUN_TEST(test_failed_deferred_promotion_preserves_live_store_and_retry_succeeds);
    RUN_TEST(test_load_uses_valid_rollback_when_live_store_is_invalid);
    RUN_TEST(test_begin_migrates_device_store_from_secondary_filesystem);
    RUN_TEST(test_short_device_store_write_fails_and_preserves_committed_catalog);
    RUN_TEST(test_newer_fallback_device_edit_wins_when_stale_sd_returns);
    RUN_TEST(test_valid_secondary_repairs_corrupt_primary_device_store);
    RUN_TEST(test_equal_generation_device_conflict_converges_to_fallback_copy);
    RUN_TEST(test_legacy_v1_device_store_is_loaded_and_upgraded_with_integrity_metadata);
    RUN_TEST(test_detector_snapshot_round_trips_with_partial_field_validity);
    RUN_TEST(test_worst_case_complete_sweep_catalog_round_trips_within_bounded_store);
    RUN_TEST(test_null_sweep_section_slots_persist_without_losing_declared_selector_topology);
    RUN_TEST(test_malformed_claimed_complete_sweep_snapshot_is_not_serialized);
    RUN_TEST(test_invalid_claimed_scalar_snapshot_fields_are_rejected_before_staging);
    RUN_TEST(test_checksum_valid_semantic_snapshot_corruption_is_not_downgraded_to_absent);
    RUN_TEST(test_semantically_invalid_primary_snapshot_recovers_from_valid_mirror);
    RUN_TEST(test_unreadable_catalog_blocks_every_read_derived_mutation_and_preserves_bytes);
    RUN_TEST(test_present_store_markers_are_exact_and_semantic_rollback_preserves_capture);
    RUN_TEST(test_current_store_rejects_duplicate_addresses_and_uses_valid_mirror);
    RUN_TEST(test_current_store_rejects_unknown_root_or_device_keys_and_uses_valid_mirror);
    RUN_TEST(test_current_store_rejects_more_than_supported_device_count_before_copying);
    RUN_TEST(test_generation_exhaustion_never_promotes_zero_or_overwrites_valid_copies);
    RUN_TEST(test_all_canonical_mode_glyphs_round_trip_in_persisted_snapshot);
    RUN_TEST(test_device_store_crc_streams_without_bulk_allocations);
    RUN_TEST(test_device_store_psram_exhaustion_preserves_live_catalog_and_retries);
    RUN_TEST(test_device_store_boot_oom_does_not_import_legacy_or_clobber_catalog);
    RUN_TEST(test_unavailable_live_address_cannot_overwrite_prior_detector_snapshot);
    RUN_TEST(test_default_profile_lookup_distinguishes_no_override_from_unavailable);
    RUN_TEST(test_v2_device_catalog_upgrades_without_inventing_a_snapshot);
    RUN_TEST(test_corrupt_newer_v2_catalog_is_rejected_and_recovered_from_valid_mirror);
    RUN_TEST(test_semantic_crc_failure_recovers_same_filesystem_rollback);
    RUN_TEST(test_failed_recovery_promotion_preserves_only_valid_rollback);
    RUN_TEST(test_failed_primary_repair_remains_pending_after_secondary_succeeds);
    RUN_TEST(test_secondary_device_store_failure_is_reported_and_retried);
    return UNITY_END();
}
