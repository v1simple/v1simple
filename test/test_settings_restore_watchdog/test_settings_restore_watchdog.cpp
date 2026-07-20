// Regression coverage for the settings-restore task watchdog feed.
//
// A full /api/settings/restore rewrites the WiFi credential NVS namespace,
// re-saves every profile in the backup and then performs the A/B settings NVS
// rewrite.  None of that used to feed the task watchdog, so a large backup on a
// slow SD card could panic mid-restore.  esp_task_wdt_reset() is ESP-IDF only,
// so applyBackupDocument() takes an injectable SettingsRestoreWatchdog; these
// tests count the feeds and pin WHERE they land relative to the profile loop.

#include <unity.h>

#include <filesystem>
#include <string>
#include <vector>

#include <ArduinoJson.h>

#include "../mocks/Arduino.h"
#include "../mocks/Preferences.h"
#include "../mocks/nvs.h"
#include "../mocks/storage_manager.h"
#include "../../src/settings.h"
#include "../../src/v1_profiles.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

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
#include "../../src/settings_setters.cpp"
#include "../../src/settings_nvs.cpp"
#include "../../src/settings_backup.cpp"
#include "../../src/settings_backup_doc.cpp"
#include "../../src/settings_restore.cpp"

namespace {

std::filesystem::path g_tempRoot;
int g_tempRootIndex = 0;

std::filesystem::path nextTempRoot() {
    return std::filesystem::temp_directory_path() /
           ("settings_restore_watchdog_" + std::to_string(++g_tempRootIndex));
}

// Records one entry per watchdog feed holding the number of profiles already
// written to the profile store at that moment.  That makes feed PLACEMENT
// assertable without leaking a phase enum into the production API.
struct FeedRecorder {
    std::vector<size_t> profilesOnDiskAtFeed;

    size_t count() const { return profilesOnDiskAtFeed.size(); }
};

void recordFeed(void* ctx) {
    auto* recorder = static_cast<FeedRecorder*>(ctx);
    recorder->profilesOnDiskAtFeed.push_back(v1ProfileManager.listProfiles().size());
}

SettingsRestoreWatchdog makeWatchdog(FeedRecorder& recorder) {
    return SettingsRestoreWatchdog{&recordFeed, &recorder};
}

void addProfile(JsonDocument& doc, const char* name) {
    JsonObject profile = doc["profiles"].add<JsonObject>();
    profile["name"] = name;
    profile["description"] = "restored";
    profile["displayOn"] = true;
    JsonArray bytes = profile["bytes"].to<JsonArray>();
    for (int i = 0; i < 6; ++i) {
        bytes.add(static_cast<uint8_t>(i + 1));
    }
}

// A backup body with many scalar fields but no profiles.  Used to prove the
// feed is per PHASE, not per field.
void addManyScalarFields(JsonDocument& doc) {
    doc["apSSID"] = "RestoredSSID";
    doc["brightness"] = 77;
    doc["turnOffDisplay"] = true;
    doc["colorBogey"] = 0x1234;
    doc["colorFrequency"] = 0x2345;
    doc["colorArrowFront"] = 0x3456;
    doc["colorArrowSide"] = 0x4567;
    doc["colorArrowRear"] = 0x5678;
    doc["colorBandL"] = 0x6789;
    doc["colorBandKa"] = 0x789A;
    doc["colorBandK"] = 0x89AB;
    doc["colorBandX"] = 0x9ABC;
    doc["colorBandPhoto"] = 0xABCD;
    doc["colorWiFiIcon"] = 0xBCDE;
    doc["colorBar1"] = 0x1111;
    doc["colorBar2"] = 0x2222;
    doc["colorBar3"] = 0x3333;
    doc["colorBar4"] = 0x4444;
    doc["colorBar5"] = 0x5555;
    doc["colorBar6"] = 0x6666;
    doc["hideWifiIcon"] = true;
    doc["hideBatteryIcon"] = true;
    doc["hideBleIcon"] = true;
    doc["hideRssiIndicator"] = true;
    doc["autoPushEnabled"] = true;
    doc["activeSlot"] = 1;
    doc["slot0Name"] = "Zero";
    doc["slot1Name"] = "One";
    doc["slot2Name"] = "Two";
    doc["obdEnabled"] = true;
    doc["obdSavedName"] = "Adapter";
    doc["obdMinRssi"] = -61;
    doc["alpEnabled"] = true;
    doc["gpsEnabled"] = true;
    doc["gpsBaud"] = 9600;
}

void resetRuntimeState() {
    mock_preferences::reset();
    mock_nvs::reset();
    storageManager.reset();
    StorageManager::resetMockSdLockState();
    v1ProfileManager = V1ProfileManager();
    settingsManager = SettingsManager();
    resetDeferredSettingsBackupStateForTest();
    mockMillis = 1000;
    mockMicros = 1000000;
}

}  // namespace

void setUp() {
    g_tempRoot = nextTempRoot();
    std::filesystem::remove_all(g_tempRoot);
    std::filesystem::create_directories(g_tempRoot);
    resetRuntimeState();
}

void tearDown() {
    std::filesystem::remove_all(g_tempRoot);
}

// The bug: a restore that never feeds the watchdog.  A profile-free restore must
// still feed once per phase boundary, and the count must not scale with the
// number of restored FIELDS.
void test_field_only_restore_feeds_once_per_phase_and_not_per_field() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    FeedRecorder recorder;

    JsonDocument doc;
    doc["_type"] = "v1simple_http_backup";
    addManyScalarFields(doc);

    const SettingsBackupApplyResult result =
        manager.applyBackupDocument(doc, true, makeWatchdog(recorder));

    TEST_ASSERT_TRUE(result.success);
    // wifi-credentials, fields-applied, pre-persist, post-persist.
    TEST_ASSERT_EQUAL_UINT32(4u, static_cast<uint32_t>(recorder.count()));
    for (size_t i = 0; i < recorder.count(); ++i) {
        TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(recorder.profilesOnDiskAtFeed[i]));
    }
}

// The phase that actually outruns the watchdog: one filesystem write per
// profile.  Feeds must land INSIDE the loop, batched, with the last two feeds
// bracketing the NVS rewrite.
void test_profile_restore_feeds_inside_loop_every_batch() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    FeedRecorder recorder;

    JsonDocument doc;
    doc["_type"] = "v1simple_http_backup";
    doc["profiles"].to<JsonArray>();
    const char* names[] = {"P0", "P1", "P2", "P3", "P4", "P5", "P6", "P7", "P8"};
    for (const char* name : names) {
        addProfile(doc, name);
    }

    const SettingsBackupApplyResult result =
        manager.applyBackupDocument(doc, true, makeWatchdog(recorder));

    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_EQUAL_INT(9, result.profilesRestored);

    // 4 phase feeds + 2 in-loop batch feeds (entries 4 and 8 of 9).
    TEST_ASSERT_EQUAL_UINT32(6u, static_cast<uint32_t>(recorder.count()));

    // Placement: two feeds before any profile is written, two mid-loop with
    // partial progress on disk, two after the loop with everything written.
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(recorder.profilesOnDiskAtFeed[0]));
    TEST_ASSERT_EQUAL_UINT32(0u, static_cast<uint32_t>(recorder.profilesOnDiskAtFeed[1]));
    TEST_ASSERT_EQUAL_UINT32(3u, static_cast<uint32_t>(recorder.profilesOnDiskAtFeed[2]));
    TEST_ASSERT_EQUAL_UINT32(7u, static_cast<uint32_t>(recorder.profilesOnDiskAtFeed[3]));
    TEST_ASSERT_EQUAL_UINT32(9u, static_cast<uint32_t>(recorder.profilesOnDiskAtFeed[4]));
    TEST_ASSERT_EQUAL_UINT32(9u, static_cast<uint32_t>(recorder.profilesOnDiskAtFeed[5]));
}

// Feed cadence must track profile COUNT (batched), so a bigger backup gets more
// feeds rather than one fixed feed that a long loop can outrun.
void test_feed_count_grows_with_profile_count_in_batches() {
    struct Case {
        int profiles;
        uint32_t expectedFeeds;
    };
    const Case cases[] = {{0, 4u}, {3, 4u}, {4, 5u}, {8, 6u}, {12, 7u}};

    for (const Case& testCase : cases) {
        resetRuntimeState();
        fs::FS fs(g_tempRoot);
        storageManager.setFilesystem(&fs, true);
        TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

        SettingsManager manager;
        FeedRecorder recorder;

        JsonDocument doc;
        doc["_type"] = "v1simple_http_backup";
        doc["profiles"].to<JsonArray>();
        for (int i = 0; i < testCase.profiles; ++i) {
            addProfile(doc, ("P" + std::to_string(i)).c_str());
        }

        const SettingsBackupApplyResult result =
            manager.applyBackupDocument(doc, true, makeWatchdog(recorder));

        TEST_ASSERT_TRUE(result.success);
        TEST_ASSERT_EQUAL_INT(testCase.profiles, result.profilesRestored);
        TEST_ASSERT_EQUAL_UINT32(testCase.expectedFeeds, static_cast<uint32_t>(recorder.count()));
    }
}

// Skipped/invalid profile entries still cost a loop iteration, so they must
// still count toward the feed batch.
void test_invalid_profile_entries_still_count_toward_feed_batches() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    FeedRecorder recorder;

    JsonDocument doc;
    doc["_type"] = "v1simple_http_backup";
    JsonArray profiles = doc["profiles"].to<JsonArray>();
    for (int i = 0; i < 8; ++i) {
        JsonObject junk = profiles.add<JsonObject>();
        junk["name"] = "no-bytes";
    }

    const SettingsBackupApplyResult result =
        manager.applyBackupDocument(doc, true, makeWatchdog(recorder));

    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_EQUAL_INT(0, result.profilesRestored);
    TEST_ASSERT_EQUAL_UINT32(6u, static_cast<uint32_t>(recorder.count()));
}

// The boot-time SD restore path calls applyBackupDocument() without a watchdog.
// A null feed must be a no-op, never a crash.
void test_absent_watchdog_is_a_no_op_and_restore_still_succeeds() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;

    JsonDocument doc;
    doc["_type"] = "v1simple_http_backup";
    doc["brightness"] = 42;
    doc["profiles"].to<JsonArray>();
    addProfile(doc, "Solo");

    const SettingsBackupApplyResult defaulted = manager.applyBackupDocument(doc, true);
    TEST_ASSERT_TRUE(defaulted.success);
    TEST_ASSERT_EQUAL_INT(1, defaulted.profilesRestored);
    TEST_ASSERT_EQUAL_UINT8(42, manager.get().brightness);

    const SettingsBackupApplyResult explicitlyEmpty =
        manager.applyBackupDocument(doc, true, SettingsRestoreWatchdog{});
    TEST_ASSERT_TRUE(explicitlyEmpty.success);
}

// A failed persist returns early; the pre-persist feed must already have run so
// the failure path is not the one that panics.
void test_pre_persist_feed_runs_before_the_nvs_rewrite() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    FeedRecorder recorder;

    JsonDocument doc;
    doc["_type"] = "v1simple_http_backup";
    doc["profiles"].to<JsonArray>();
    addProfile(doc, "Solo");

    const SettingsBackupApplyResult result =
        manager.applyBackupDocument(doc, true, makeWatchdog(recorder));
    TEST_ASSERT_TRUE(result.success);

    // Final two feeds bracket the persist step: both see the full profile set.
    TEST_ASSERT_EQUAL_UINT32(4u, static_cast<uint32_t>(recorder.count()));
    TEST_ASSERT_EQUAL_UINT32(1u,
                             static_cast<uint32_t>(recorder.profilesOnDiskAtFeed[recorder.count() - 2]));
    TEST_ASSERT_EQUAL_UINT32(1u,
                             static_cast<uint32_t>(recorder.profilesOnDiskAtFeed[recorder.count() - 1]));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_field_only_restore_feeds_once_per_phase_and_not_per_field);
    RUN_TEST(test_profile_restore_feeds_inside_loop_every_batch);
    RUN_TEST(test_feed_count_grows_with_profile_count_in_batches);
    RUN_TEST(test_invalid_profile_entries_still_count_toward_feed_batches);
    RUN_TEST(test_absent_watchdog_is_a_no_op_and_restore_still_succeeds);
    RUN_TEST(test_pre_persist_feed_runs_before_the_nvs_rewrite);
    return UNITY_END();
}
