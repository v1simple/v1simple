#include <unity.h>

#include <filesystem>

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
#include "../../src/settings_restore.cpp"

namespace {

std::filesystem::path g_tempRoot;
int g_tempRootIndex = 0;

std::filesystem::path nextTempRoot() {
    return std::filesystem::temp_directory_path() /
           ("settings_deferred_persist_" + std::to_string(++g_tempRootIndex));
}

void resetRuntimeState() {
    mock_preferences::reset();
    mock_nvs::reset();
    mock_reset_heap_caps();
    mock_reset_queue_create_state();
    mock_reset_task_create_state();
    storageManager.reset();
    StorageManager::resetMockSdLockState();
    resetDeferredSettingsBackupStateForTest();
    v1ProfileManager = V1ProfileManager();
    settingsManager = SettingsManager();
    mockMillis = 1000;
    mockMicros = 1000000;
}

String activeNamespaceOrEmpty() {
    return mock_preferences::getString(SETTINGS_NS_META, "active", "");
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
    resetDeferredSettingsBackupStateForTest();
}

void test_deferred_batch_updates_coalesce_to_single_persist_and_request_backup() {
    SettingsManager manager;

    DeviceSettingsUpdate firstUpdate;
    firstUpdate.hasProxyName = true;
    firstUpdate.proxyName = "First";
    manager.applyDeviceSettingsUpdate(firstUpdate, SettingsPersistMode::Deferred);

    TEST_ASSERT_TRUE(manager.deferredPersistPending());
    TEST_ASSERT_FALSE(manager.deferredPersistRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(1750u, manager.deferredPersistNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());
    TEST_ASSERT_FALSE(manager.deferredBackupPending());

    mockMillis = 1300;

    DeviceSettingsUpdate secondUpdate;
    secondUpdate.hasProxyName = true;
    secondUpdate.proxyName = "Second";
    manager.applyDeviceSettingsUpdate(secondUpdate, SettingsPersistMode::Deferred);

    TEST_ASSERT_TRUE(manager.deferredPersistPending());
    TEST_ASSERT_FALSE(manager.deferredPersistRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(2050u, manager.deferredPersistNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    manager.serviceDeferredPersist(2049);
    TEST_ASSERT_TRUE(manager.deferredPersistPending());
    TEST_ASSERT_EQUAL_UINT32(2050u, manager.deferredPersistNextAttemptAtMs());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    manager.serviceDeferredPersist(2050);

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);
    TEST_ASSERT_EQUAL_STRING("Second",
                             mock_preferences::getString(activeNs.c_str(), "proxyName", "").c_str());
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
    TEST_ASSERT_FALSE(manager.deferredPersistRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(0u, manager.deferredPersistNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
    TEST_ASSERT_TRUE(manager.deferredBackupPending());

    manager.serviceDeferredPersist(3000);
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
}

void test_deferred_persist_retries_after_failed_nvs_write() {
    SettingsManager manager;
    manager.mutableSettings().apSSID = "RetryPath";
    manager.requestDeferredPersist();

    mock_preferences::set_fail_writes(true);
    manager.serviceDeferredPersist(1750);

    TEST_ASSERT_TRUE(manager.deferredPersistPending());
    TEST_ASSERT_TRUE(manager.deferredPersistRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(2750u, manager.deferredPersistNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_FALSE(manager.deferredBackupPending());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    mock_preferences::set_fail_writes(false);

    manager.serviceDeferredPersist(2749);
    TEST_ASSERT_TRUE(manager.deferredPersistPending());
    TEST_ASSERT_TRUE(manager.deferredPersistRetryScheduled());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    manager.serviceDeferredPersist(2750);

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);
    TEST_ASSERT_EQUAL_STRING("RetryPath",
                             mock_preferences::getString(activeNs.c_str(), "apSSID", "").c_str());
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
    TEST_ASSERT_FALSE(manager.deferredPersistRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(0u, manager.deferredPersistNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
    TEST_ASSERT_TRUE(manager.deferredBackupPending());
}

void test_save_flushes_immediately_and_clears_deferred_persist() {
    SettingsManager manager;
    manager.mutableSettings().proxyName = "Pending";
    manager.requestDeferredPersist();
    manager.mutableSettings().proxyName = "Immediate";

    manager.save();

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);
    TEST_ASSERT_EQUAL_STRING("Immediate",
                             mock_preferences::getString(activeNs.c_str(), "proxyName", "").c_str());
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
    TEST_ASSERT_FALSE(manager.deferredPersistRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(0u, manager.deferredPersistNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_deferred_batch_updates_coalesce_to_single_persist_and_request_backup);
    RUN_TEST(test_deferred_persist_retries_after_failed_nvs_write);
    RUN_TEST(test_save_flushes_immediately_and_clears_deferred_persist);
    return UNITY_END();
}
