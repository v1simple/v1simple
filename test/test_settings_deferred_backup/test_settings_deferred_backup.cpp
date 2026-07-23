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
#include "../../src/settings_nvs.cpp"
#include "../../src/settings_backup.cpp"
#include "../../src/settings_backup_doc.cpp"
#include "../../src/settings_restore.cpp"

namespace {

std::filesystem::path g_tempRoot;
int g_tempRootIndex = 0;

std::filesystem::path nextTempRoot() {
    return std::filesystem::temp_directory_path() /
           ("settings_deferred_backup_" + std::to_string(++g_tempRootIndex));
}

String activeNamespaceOrEmpty() {
    return mock_preferences::getString(SETTINGS_NS_META, "active", "");
}

bool loadJsonFile(fs::FS& fs, const char* path, JsonDocument& doc) {
    File file = fs.open(path, FILE_READ);
    if (!file) {
        return false;
    }
    const DeserializationError err = deserializeJson(doc, file);
    file.close();
    return !err;
}

void writeSeedBackup(fs::FS& fs, const char* apSsid, uint8_t brightness) {
    SettingsManager source;
    source.mutableSettings().apSSID = apSsid;
    source.mutableSettings().brightness = brightness;

    SerializedSettingsBackupPayload payload;
    TEST_ASSERT_TRUE(buildSerializedSdBackupPayload(payload, source.get(), v1ProfileManager, 5000));
    TEST_ASSERT_TRUE(writeBackupAtomically(&fs, payload));
    releaseSerializedSettingsBackupPayload(payload);
}

void createRestorePendingNvsWithoutSd() {
    storageManager.reset();
    v1ProfileManager = V1ProfileManager();

    SettingsManager noSdBoot;
    noSdBoot.begin();
    noSdBoot.save();

    const String provisionalNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(provisionalNs.length() > 0);
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(provisionalNs.c_str(), kNvsRestorePending));
    TEST_ASSERT_TRUE(mock_preferences::getUnsigned(provisionalNs.c_str(), kNvsRestorePending, 0) != 0);
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

void test_save_deferred_backup_persists_nvs_and_writes_snapshot_via_writer() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().apSSID = "DeferredSave";
    manager.mutableSettings().brightness = 88;

    manager.saveDeferredBackup();

    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
    TEST_ASSERT_TRUE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_TRUE(manager.deferredBackupPending());
    TEST_ASSERT_FALSE(manager.deferredBackupRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(0u, manager.deferredBackupNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT(0u, deferredSettingsBackupQueueDepthForTest());

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(activeNs.c_str(), "apSSID"));

    manager.serviceDeferredBackup(1000);

    TEST_ASSERT_FALSE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_queue_create_state.staticCalls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_task_create_state.capsCalls);

    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT(0u, deferredSettingsBackupQueueDepthForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_STRING("DeferredSave", backupDoc["apSSID"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(88, backupDoc["brightness"].as<int>());
}

void test_service_deferred_backup_retries_after_sd_trylock_busy() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().apSSID = "RetryPath";
    manager.requestDeferredBackupFromCurrentState();

    StorageManager::mockSdLockState.failNextTryLockCount = 1;
    manager.serviceDeferredBackup(1000);

    TEST_ASSERT_TRUE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_TRUE(manager.deferredBackupPending());
    TEST_ASSERT_TRUE(manager.deferredBackupRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(1250u, manager.deferredBackupNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT(0u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_EQUAL_UINT32(1u, StorageManager::mockSdLockState.tryAcquireCalls);

    manager.serviceDeferredBackup(1200);
    TEST_ASSERT_EQUAL_UINT32(1u, StorageManager::mockSdLockState.tryAcquireCalls);
    TEST_ASSERT_TRUE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_TRUE(manager.deferredBackupPending());
    TEST_ASSERT_TRUE(manager.deferredBackupRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(1250u, manager.deferredBackupNextAttemptAtMs());

    manager.serviceDeferredBackup(1250);
    TEST_ASSERT_FALSE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_FALSE(manager.deferredBackupPending());
    TEST_ASSERT_FALSE(manager.deferredBackupRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(0u, manager.deferredBackupNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_EQUAL_UINT32(2u, StorageManager::mockSdLockState.tryAcquireCalls);
}

void test_repeated_requests_coalesce_to_latest_snapshot() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().apSSID = "OldValue";
    manager.requestDeferredBackupFromCurrentState();
    manager.mutableSettings().apSSID = "NewValue";
    manager.requestDeferredBackupFromCurrentState();

    manager.serviceDeferredBackup(1000);
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_STRING("NewValue", backupDoc["apSSID"].as<const char*>());
}

void test_repeated_save_deferred_backup_calls_coalesce_to_latest_snapshot() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().apSSID = "FirstValue";
    manager.saveDeferredBackup();
    manager.mutableSettings().apSSID = "FinalValue";
    manager.saveDeferredBackup();

    TEST_ASSERT_TRUE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_EQUAL_UINT32(3u, manager.backupRevision());

    manager.serviceDeferredBackup(1000);
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_STRING("FinalValue", backupDoc["apSSID"].as<const char*>());
}

void test_immediate_reset_recovers_one_latest_deferred_backup_from_persisted_intent() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager beforeReset;
    beforeReset.mutableSettings().brightness = 91;
    TEST_ASSERT_TRUE(beforeReset.saveDeferredBackup());
    beforeReset.mutableSettings().stealthEnabled = true;
    TEST_ASSERT_TRUE(beforeReset.saveDeferredBackup());
    beforeReset.mutableSettings().activeSlot = 2;
    TEST_ASSERT_TRUE(beforeReset.saveDeferredBackup());
    const uint32_t latestRevision = beforeReset.backupDueRevision();
    const String committedNamespace = activeNamespaceOrEmpty();

    TEST_ASSERT_TRUE(latestRevision != 0u);
    TEST_ASSERT_EQUAL_UINT64(
        latestRevision,
        mock_preferences::getUnsigned(committedNamespace.c_str(), kNvsBackupDueRevision, 0));
    TEST_ASSERT_FALSE(fs.exists(SETTINGS_BACKUP_PATH));
    TEST_ASSERT_EQUAL_UINT(0u, deferredSettingsBackupQueueDepthForTest());

    // Model an immediate hard reset: NVS and media survive, all task/queue and
    // SettingsManager RAM state does not.
    resetDeferredSettingsBackupStateForTest();
    SettingsManager afterReset;
    afterReset.load();

    TEST_ASSERT_EQUAL_UINT8(91, afterReset.get().brightness);
    TEST_ASSERT_TRUE(afterReset.get().stealthEnabled);
    TEST_ASSERT_EQUAL_INT(2, afterReset.get().activeSlot);
    TEST_ASSERT_EQUAL_UINT32(latestRevision, afterReset.backupDueRevision());
    TEST_ASSERT_TRUE(afterReset.deferredBackupPending());

    afterReset.serviceDeferredBackup(2000);
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT(0u, deferredSettingsBackupQueueDepthForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_INT(91, backupDoc["brightness"].as<int>());
    TEST_ASSERT_TRUE(backupDoc["stealthEnabled"].as<bool>());
    TEST_ASSERT_EQUAL_INT(2, backupDoc["activeSlot"].as<int>());
    TEST_ASSERT_EQUAL_UINT64(
        latestRevision,
        mock_preferences::getUnsigned(SETTINGS_NS_META, kNvsBackupCompletedRevision, 0));

    // A second reboot sees completion bound to the same revision and must not
    // enqueue or rewrite the already-current file.
    resetDeferredSettingsBackupStateForTest();
    SettingsManager secondBoot;
    secondBoot.load();
    TEST_ASSERT_FALSE(secondBoot.deferredBackupPending());
    secondBoot.serviceDeferredBackup(3000);
    TEST_ASSERT_EQUAL_UINT(0u, deferredSettingsBackupQueueDepthForTest());
}

void test_older_writer_completion_does_not_suppress_newer_persisted_due_revision() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().brightness = 81;
    TEST_ASSERT_TRUE(manager.saveDeferredBackup());
    const uint32_t olderRevision = manager.backupDueRevision();
    manager.serviceDeferredBackup(1000);
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());

    manager.mutableSettings().brightness = 93;
    TEST_ASSERT_TRUE(manager.saveDeferredBackup());
    const uint32_t newerRevision = manager.backupDueRevision();
    TEST_ASSERT_TRUE(newerRevision != olderRevision);

    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT64(
        olderRevision,
        mock_preferences::getUnsigned(SETTINGS_NS_META, kNvsBackupCompletedRevision, 0));

    resetDeferredSettingsBackupStateForTest();
    SettingsManager rebooted;
    rebooted.load();
    TEST_ASSERT_EQUAL_UINT32(newerRevision, rebooted.backupDueRevision());
    TEST_ASSERT_TRUE(rebooted.deferredBackupPending());
    rebooted.serviceDeferredBackup(2000);
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_INT(93, backupDoc["brightness"].as<int>());
    TEST_ASSERT_EQUAL_UINT64(
        newerRevision,
        mock_preferences::getUnsigned(SETTINGS_NS_META, kNvsBackupCompletedRevision, 0));
}

void test_completion_revision_write_failure_preserves_deferred_retry() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().brightness = 84;
    TEST_ASSERT_TRUE(manager.saveDeferredBackup());
    const uint32_t dueRevision = manager.backupDueRevision();
    manager.serviceDeferredBackup(1000);
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());

    mock_preferences::set_fail_writes(true);
    TEST_ASSERT_FALSE(runDeferredSettingsBackupWriterOnceForTest());
    mock_preferences::set_fail_writes(false);
    TEST_ASSERT_TRUE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_EQUAL_UINT64(
        0u,
        mock_preferences::getUnsigned(SETTINGS_NS_META, kNvsBackupCompletedRevision, 0));

    manager.serviceDeferredBackup(2000);
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());
    TEST_ASSERT_EQUAL_UINT64(
        dueRevision,
        mock_preferences::getUnsigned(SETTINGS_NS_META, kNvsBackupCompletedRevision, 0));
    TEST_ASSERT_FALSE(deferredSettingsBackupPendingForTest());
}

void test_ab_rollback_completed_revision_collision_requeues_latest_after_reset() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    for (uint8_t brightness : {81u, 82u, 83u}) {
        manager.mutableSettings().brightness = brightness;
        TEST_ASSERT_TRUE(manager.saveDeferredBackup());
        manager.serviceDeferredBackup(1000u + brightness);
        TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());
    }
    const uint32_t completedRevision = manager.backupDueRevision();
    const String newestNamespace = activeNamespaceOrEmpty();
    const String rollbackNamespace = newestNamespace == SETTINGS_NS_A ? SETTINGS_NS_B : SETTINGS_NS_A;
    const uint32_t rollbackDue = static_cast<uint32_t>(
        mock_preferences::getUnsigned(rollbackNamespace.c_str(), kNvsBackupDueRevision, 0));
    TEST_ASSERT_TRUE(rollbackDue != 0u);
    TEST_ASSERT_TRUE(rollbackDue != completedRevision);

    Preferences meta;
    TEST_ASSERT_TRUE(meta.begin(SETTINGS_NS_META, false));
    TEST_ASSERT_GREATER_THAN(0u, meta.putString(kNvsMetaActive, rollbackNamespace));
    meta.end();

    resetDeferredSettingsBackupStateForTest();
    SettingsManager recovered;
    recovered.load();
    TEST_ASSERT_EQUAL_UINT32(rollbackDue, recovered.backupDueRevision());
    TEST_ASSERT_TRUE(recovered.deferredBackupPending());

    recovered.mutableSettings().brightness = 99;
    TEST_ASSERT_TRUE(recovered.saveDeferredBackup());
    const uint32_t latestRevision = recovered.backupDueRevision();
    TEST_ASSERT_TRUE(latestRevision != rollbackDue);
    TEST_ASSERT_TRUE(latestRevision != completedRevision);

    resetDeferredSettingsBackupStateForTest();
    SettingsManager afterImmediateReset;
    afterImmediateReset.load();
    TEST_ASSERT_EQUAL_UINT32(latestRevision, afterImmediateReset.backupDueRevision());
    TEST_ASSERT_TRUE(afterImmediateReset.deferredBackupPending());
    afterImmediateReset.serviceDeferredBackup(3000);
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_INT(99, backupDoc["brightness"].as<int>());
    TEST_ASSERT_EQUAL_UINT64(
        latestRevision,
        mock_preferences::getUnsigned(SETTINGS_NS_META, kNvsBackupCompletedRevision, 0));

    resetDeferredSettingsBackupStateForTest();
    SettingsManager settled;
    settled.load();
    TEST_ASSERT_FALSE(settled.deferredBackupPending());
}

void test_revision_wrap_repeated_writes_never_collide_and_reset_requeues_latest() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager seed;
    seed.mutableSettings().brightness = 70;
    TEST_ASSERT_TRUE(seed.saveDeferredBackup());
    const String activeNamespace = activeNamespaceOrEmpty();

    Preferences settingsPrefs;
    TEST_ASSERT_TRUE(settingsPrefs.begin(activeNamespace.c_str(), false));
    TEST_ASSERT_GREATER_THAN(0u, settingsPrefs.putUInt(kNvsBackupDueRevision, UINT32_MAX));
    settingsPrefs.end();
    Preferences meta;
    TEST_ASSERT_TRUE(meta.begin(SETTINGS_NS_META, false));
    TEST_ASSERT_GREATER_THAN(0u, meta.putUInt(kNvsBackupCompletedRevision, UINT32_MAX));
    meta.end();

    resetDeferredSettingsBackupStateForTest();
    SettingsManager wrapped;
    wrapped.load();
    TEST_ASSERT_FALSE(wrapped.deferredBackupPending());
    wrapped.mutableSettings().brightness = 71;
    TEST_ASSERT_TRUE(wrapped.saveDeferredBackup());
    const uint32_t firstFresh = wrapped.backupDueRevision();
    TEST_ASSERT_EQUAL_UINT32(1u, firstFresh);
    wrapped.mutableSettings().brightness = 72;
    TEST_ASSERT_TRUE(wrapped.saveDeferredBackup());
    const uint32_t latestRevision = wrapped.backupDueRevision();
    TEST_ASSERT_EQUAL_UINT32(2u, latestRevision);
    TEST_ASSERT_TRUE(latestRevision != firstFresh);
    TEST_ASSERT_TRUE(latestRevision != UINT32_MAX);

    TEST_ASSERT_EQUAL_UINT32(
        3u, settings_backup_revision::nextFresh(UINT32_MAX, 1u, 2u));
    TEST_ASSERT_EQUAL_UINT32(
        2u, settings_backup_revision::nextFresh(UINT32_MAX, UINT32_MAX, 1u));

    resetDeferredSettingsBackupStateForTest();
    SettingsManager afterImmediateReset;
    afterImmediateReset.load();
    TEST_ASSERT_EQUAL_UINT32(latestRevision, afterImmediateReset.backupDueRevision());
    TEST_ASSERT_TRUE(afterImmediateReset.deferredBackupPending());
    afterImmediateReset.serviceDeferredBackup(4000);
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_INT(72, backupDoc["brightness"].as<int>());
}

void test_service_deferred_backup_keeps_pending_when_writer_setup_fails() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().apSSID = "QueueFail";
    manager.requestDeferredBackupFromCurrentState();

    g_mock_heap_caps_fail_malloc = true;
    g_mock_queue_create_state.failDynamic = true;

    manager.serviceDeferredBackup(1000);

    TEST_ASSERT_TRUE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_EQUAL_UINT(0u, deferredSettingsBackupQueueDepthForTest());
}

void test_aborted_shutdown_reopens_deferred_backup_writer_admission() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().apSSID = "AbortRecovery";
    shutdownDeferredSettingsBackupWriter(0);
    manager.requestDeferredBackupFromCurrentState();

    manager.serviceDeferredBackup(1000);
    TEST_ASSERT_TRUE(manager.deferredBackupPending());
    TEST_ASSERT_TRUE(manager.deferredBackupRetryScheduled());
    TEST_ASSERT_EQUAL_UINT32(1250u, manager.deferredBackupNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT(0u, deferredSettingsBackupQueueDepthForTest());

    resumeDeferredSettingsBackupWriterAfterAbortedShutdown();
    manager.serviceDeferredBackup(1250);

    TEST_ASSERT_FALSE(manager.deferredBackupPending());
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_STRING("AbortRecovery", backupDoc["apSSID"].as<const char*>());
}

void test_resume_handoffs_work_queued_while_old_deferred_writer_exits() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().apSSID = "BeforeAbort";
    manager.requestDeferredBackupFromCurrentState();
    manager.serviceDeferredBackup(1000);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_task_create_state.capsCalls);
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());

    // Model the bounded-timeout boundary: the old task has committed to exit,
    // abort recovery reopens admission, and service queues work before the old
    // task publishes itself inactive.
    gDeferredSettingsBackupState.shutdownRequested.store(true, std::memory_order_release);
    resumeDeferredSettingsBackupWriterAfterAbortedShutdown();
    manager.mutableSettings().apSSID = "DuringExit";
    manager.requestDeferredBackupFromCurrentState();
    manager.serviceDeferredBackup(2000);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_task_create_state.capsCalls);
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());

    TEST_ASSERT_TRUE(completeDeferredBackupWriterExit());

    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_task_create_state.capsCalls);
    TEST_ASSERT_TRUE(gDeferredSettingsBackupState.writerActive.load(std::memory_order_acquire));
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_STRING("DuringExit", backupDoc["apSSID"].as<const char*>());
}

void test_shutdown_exit_discards_late_deferred_payload_and_waits_for_admissions() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.requestDeferredBackupFromCurrentState();
    manager.serviceDeferredBackup(1000);
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());

    gDeferredSettingsBackupState.writerActive.store(false, std::memory_order_release);
    TEST_ASSERT_FALSE(deferredBackupWriterQuiesced());
    gDeferredSettingsBackupState.writerActive.store(true, std::memory_order_release);

    gDeferredSettingsBackupState.shutdownRequested.store(true, std::memory_order_release);
    TEST_ASSERT_FALSE(completeDeferredBackupWriterExit());
    TEST_ASSERT_EQUAL_UINT(0u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(deferredBackupWriterQuiesced());

    gDeferredSettingsBackupState.writerAdmissionsInFlight.store(1, std::memory_order_release);
    TEST_ASSERT_FALSE(deferredBackupWriterQuiesced());
    gDeferredSettingsBackupState.writerAdmissionsInFlight.store(0, std::memory_order_release);
    TEST_ASSERT_TRUE(deferredBackupWriterQuiesced());
}

void test_writer_failure_requeues_backup_request_for_rebuild() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().apSSID = "Initial";
    manager.requestDeferredBackupFromCurrentState();
    manager.serviceDeferredBackup(1000);
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());

    storageManager.reset();
    StorageManager::resetMockSdLockState();
    TEST_ASSERT_FALSE(runDeferredSettingsBackupWriterOnceForTest());
    TEST_ASSERT_TRUE(deferredSettingsBackupPendingForTest());

    storageManager.setFilesystem(&fs, true);
    manager.mutableSettings().apSSID = "Recovered";
    manager.serviceDeferredBackup(2000);

    TEST_ASSERT_FALSE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_STRING("Recovered", backupDoc["apSSID"].as<const char*>());
}

void test_restore_pending_deferred_backup_preserves_existing_sd_backup() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));
    writeSeedBackup(fs, "SD-Rig", 88);

    mock_preferences::reset();
    createRestorePendingNvsWithoutSd();

    storageManager.setFilesystem(&fs, true);
    v1ProfileManager = V1ProfileManager();
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager pendingWriter;
    pendingWriter.load();
    pendingWriter.mutableSettings().apSSID = "Factory-Default";
    pendingWriter.mutableSettings().brightness = 200;
    pendingWriter.requestDeferredBackupFromCurrentState();
    pendingWriter.serviceDeferredBackup(1000);

    TEST_ASSERT_FALSE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());
    TEST_ASSERT_FALSE(deferredSettingsBackupPendingForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_STRING("SD-Rig", backupDoc["apSSID"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(88, backupDoc["brightness"].as<int>());
}

void test_restore_pending_deferred_backup_creates_baseline_when_no_valid_sd_backup_exists() {
    fs::FS fs(g_tempRoot);

    createRestorePendingNvsWithoutSd();

    storageManager.setFilesystem(&fs, true);
    v1ProfileManager = V1ProfileManager();
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager pendingWriter;
    pendingWriter.load();
    pendingWriter.mutableSettings().apSSID = "Factory-Baseline";
    pendingWriter.mutableSettings().brightness = 200;
    pendingWriter.requestDeferredBackupFromCurrentState();
    pendingWriter.serviceDeferredBackup(1000);

    TEST_ASSERT_FALSE(deferredSettingsBackupPendingForTest());
    TEST_ASSERT_EQUAL_UINT(1u, deferredSettingsBackupQueueDepthForTest());
    TEST_ASSERT_TRUE(runDeferredSettingsBackupWriterOnceForTest());
    TEST_ASSERT_FALSE(deferredSettingsBackupPendingForTest());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_STRING("Factory-Baseline", backupDoc["apSSID"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(200, backupDoc["brightness"].as<int>());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_save_deferred_backup_persists_nvs_and_writes_snapshot_via_writer);
    RUN_TEST(test_service_deferred_backup_retries_after_sd_trylock_busy);
    RUN_TEST(test_repeated_requests_coalesce_to_latest_snapshot);
    RUN_TEST(test_repeated_save_deferred_backup_calls_coalesce_to_latest_snapshot);
    RUN_TEST(test_immediate_reset_recovers_one_latest_deferred_backup_from_persisted_intent);
    RUN_TEST(test_older_writer_completion_does_not_suppress_newer_persisted_due_revision);
    RUN_TEST(test_completion_revision_write_failure_preserves_deferred_retry);
    RUN_TEST(test_ab_rollback_completed_revision_collision_requeues_latest_after_reset);
    RUN_TEST(test_revision_wrap_repeated_writes_never_collide_and_reset_requeues_latest);
    RUN_TEST(test_service_deferred_backup_keeps_pending_when_writer_setup_fails);
    RUN_TEST(test_aborted_shutdown_reopens_deferred_backup_writer_admission);
    RUN_TEST(test_resume_handoffs_work_queued_while_old_deferred_writer_exits);
    RUN_TEST(test_shutdown_exit_discards_late_deferred_payload_and_waits_for_admissions);
    RUN_TEST(test_writer_failure_requeues_backup_request_for_rebuild);
    RUN_TEST(test_restore_pending_deferred_backup_preserves_existing_sd_backup);
    RUN_TEST(test_restore_pending_deferred_backup_creates_baseline_when_no_valid_sd_backup_exists);
    return UNITY_END();
}
