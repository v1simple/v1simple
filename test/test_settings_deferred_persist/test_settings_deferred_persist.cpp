/**
 * Regression boundary: deferred settings updates coalesce, retry failed NVS
 * writes, and flush immediately when an explicit save is requested.
 */
#include <unity.h>

#include <filesystem>

#include <ArduinoJson.h>

#include "../mocks/Arduino.h"
#include "../mocks/Preferences.h"
#include "../mocks/nvs.h"
#include "../mocks/storage_manager.h"
#include "../../src/settings.h"
#include "../../src/settings_keys.h"
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

V1ProfileManager profiles;
SettingsManager settings(storage, profiles);

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
           ("settings_deferred_persist_" + std::to_string(++g_tempRootIndex));
}

void resetRuntimeState() {
    mock_preferences::reset();
    mock_nvs::reset();
    mock_reset_heap_caps();
    mock_reset_queue_create_state();
    mock_reset_task_create_state();
    storage.reset();
    StorageManager::resetMockSdLockState();
    resetDeferredSettingsBackupStateForTest();
    profiles = V1ProfileManager();
    settings = SettingsManager(storage, profiles);
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
    SettingsManager manager(storage, profiles);

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
    SettingsManager manager(storage, profiles);
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
    SettingsManager manager(storage, profiles);
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

void test_last_v1_address_does_not_schedule_full_settings_persist() {
    SettingsManager manager(storage, profiles);

    manager.setLastV1Address("  aa:bb:cc:dd:ee:ff  ");

    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", manager.get().lastV1Address.c_str());
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
    TEST_ASSERT_FALSE(manager.deferredBackupPending());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    // The compatibility value is still included when an explicit settings
    // save is requested, including the graceful-shutdown path.
    manager.save();

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);
    TEST_ASSERT_EQUAL_STRING(
        "AA:BB:CC:DD:EE:FF",
        mock_preferences::getString(activeNs.c_str(), "lastV1Addr", "").c_str());

    const bool persistPendingBeforeCaseOnlyUpdate = manager.deferredPersistPending();
    const bool backupPendingBeforeCaseOnlyUpdate = manager.deferredBackupPending();
    manager.setLastV1Address("AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", manager.get().lastV1Address.c_str());
    TEST_ASSERT_EQUAL(persistPendingBeforeCaseOnlyUpdate, manager.deferredPersistPending());
    TEST_ASSERT_EQUAL(backupPendingBeforeCaseOnlyUpdate, manager.deferredBackupPending());
}

void test_last_v1_address_degraded_fallback_uses_one_idempotent_nvs_key() {
    SettingsManager manager(storage, profiles);

    manager.requestLastV1AddressFallbackPersist("AA:BB:CC:DD:EE:FF");
    TEST_ASSERT_EQUAL_STRING(
        "",
        mock_preferences::getString(kSettingsV1RuntimeNamespace, kNvsLastConnectedV1Address, "").c_str());
    manager.serviceDeferredPersist(1749u);
    TEST_ASSERT_EQUAL_STRING(
        "",
        mock_preferences::getString(kSettingsV1RuntimeNamespace, kNvsLastConnectedV1Address, "").c_str());
    manager.serviceDeferredPersist(1750u);
    TEST_ASSERT_EQUAL_STRING(
        "AA:BB:CC:DD:EE:FF",
        mock_preferences::getString(kSettingsV1RuntimeNamespace, kNvsLastConnectedV1Address, "").c_str());
    TEST_ASSERT_EQUAL_UINT(0u, mock_preferences::missingStringReadCount(kNvsLastConnectedV1Address));
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
    TEST_ASSERT_FALSE(manager.deferredBackupPending());

    // Re-recording the same successful connection is a no-op, even when new
    // NVS writes are unavailable. A failed update leaves the last verified
    // fallback intact and retries outside the connection callback.
    mock_preferences::set_fail_writes(true);
    manager.requestLastV1AddressFallbackPersist("AA:BB:CC:DD:EE:FF");
    manager.serviceDeferredPersist(3000u);
    manager.requestLastV1AddressFallbackPersist("11:22:33:44:55:66");
    manager.serviceDeferredPersist(1750u);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", manager.loadLastV1AddressFallback().c_str());

    mock_preferences::set_fail_writes(false);
    manager.serviceDeferredPersist(2750u);
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", manager.loadLastV1AddressFallback().c_str());
    TEST_ASSERT_TRUE(manager.clearLastV1AddressFallback());
    TEST_ASSERT_EQUAL_STRING("", manager.loadLastV1AddressFallback().c_str());
}

void test_full_settings_save_supersedes_pending_degraded_fallback() {
    SettingsManager manager(storage, profiles);

    manager.setLastV1Address("11:22:33:44:55:66");
    manager.requestLastV1AddressFallbackPersist("11:22:33:44:55:66");
    manager.save();

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);
    TEST_ASSERT_EQUAL_STRING(
        "11:22:33:44:55:66",
        mock_preferences::getString(activeNs.c_str(), kNvsLastV1Address, "").c_str());
    TEST_ASSERT_EQUAL_STRING("", manager.loadLastV1AddressFallback().c_str());

    // Servicing later must not resurrect the fallback cleared by the newer
    // atomic settings snapshot.
    manager.serviceDeferredPersist(5000u);
    TEST_ASSERT_EQUAL_STRING("", manager.loadLastV1AddressFallback().c_str());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_deferred_batch_updates_coalesce_to_single_persist_and_request_backup);
    RUN_TEST(test_deferred_persist_retries_after_failed_nvs_write);
    RUN_TEST(test_save_flushes_immediately_and_clears_deferred_persist);
    RUN_TEST(test_last_v1_address_does_not_schedule_full_settings_persist);
    RUN_TEST(test_last_v1_address_degraded_fallback_uses_one_idempotent_nvs_key);
    RUN_TEST(test_full_settings_save_supersedes_pending_degraded_fallback);
    return UNITY_END();
}
