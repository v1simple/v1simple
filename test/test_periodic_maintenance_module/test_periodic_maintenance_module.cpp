#include <unity.h>
#include <initializer_list>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/periodic_maintenance_module.cpp"

static PeriodicMaintenanceModule module;

enum CallId {
    CALL_PERF = 1,
    CALL_OBD_SETTINGS_SYNC = 3,
    CALL_DEFERRED_SETTINGS_PERSIST = 4,
    CALL_DEFERRED_SETTINGS_BACKUP = 5,
    CALL_DEFERRED_BLE_BOND_BACKUP = 6,
    CALL_STORE_SAVE = 7,
};

static int callLog[16];
static size_t callLogCount = 0;

static uint32_t timestampSequence[16];
static size_t timestampSequenceCount = 0;
static size_t timestampSequenceIndex = 0;

static uint32_t perfElapsedUs = 0;
static int perfRecordCalls = 0;
static uint32_t obdSettingsSyncNowMs = 0;
static uint32_t deferredSettingsPersistNowMs = 0;
static uint32_t deferredSettingsBackupNowMs = 0;
static uint32_t deferredBleBondBackupNowMs = 0;
static uint32_t storeSaveNowMs = 0;

static void noteCall(int id) {
    if (callLogCount < (sizeof(callLog) / sizeof(callLog[0]))) {
        callLog[callLogCount++] = id;
    }
}

static void setTimestampSequence(std::initializer_list<uint32_t> values) {
    timestampSequenceCount = values.size();
    timestampSequenceIndex = 0;
    size_t i = 0;
    for (uint32_t value : values) {
        timestampSequence[i++] = value;
    }
}

static uint32_t nextTimestampUs(void*) {
    if (timestampSequenceCount == 0) {
        return 0;
    }
    if (timestampSequenceIndex >= timestampSequenceCount) {
        return timestampSequence[timestampSequenceCount - 1];
    }
    return timestampSequence[timestampSequenceIndex++];
}

static void runPerfReport(void*) {
    noteCall(CALL_PERF);
}

static void recordPerfReportUs(void*, uint32_t elapsedUs) {
    perfRecordCalls++;
    perfElapsedUs = elapsedUs;
}

static void runObdSettingsSync(void*, uint32_t nowMs) {
    noteCall(CALL_OBD_SETTINGS_SYNC);
    obdSettingsSyncNowMs = nowMs;
}

static void runDeferredSettingsPersist(void*, uint32_t nowMs) {
    noteCall(CALL_DEFERRED_SETTINGS_PERSIST);
    deferredSettingsPersistNowMs = nowMs;
}

static void runDeferredSettingsBackup(void*, uint32_t nowMs) {
    noteCall(CALL_DEFERRED_SETTINGS_BACKUP);
    deferredSettingsBackupNowMs = nowMs;
}

static void runDeferredBleBondBackup(void*, uint32_t nowMs) {
    noteCall(CALL_DEFERRED_BLE_BOND_BACKUP);
    deferredBleBondBackupNowMs = nowMs;
}

static void runStoreSave(void*, uint32_t nowMs) {
    noteCall(CALL_STORE_SAVE);
    storeSaveNowMs = nowMs;
}

static PeriodicMaintenanceModule::Providers fullProviders() {
    PeriodicMaintenanceModule::Providers providers;
    providers.timestampUs = nextTimestampUs;
    providers.runPerfReport = runPerfReport;
    providers.recordPerfReportUs = recordPerfReportUs;
    providers.runObdSettingsSync = runObdSettingsSync;
    providers.runDeferredSettingsPersist = runDeferredSettingsPersist;
    providers.runDeferredSettingsBackup = runDeferredSettingsBackup;
    providers.runDeferredBleBondBackup = runDeferredBleBondBackup;
    providers.runStoreSave = runStoreSave;
    return providers;
}

static void resetState() {
    callLogCount = 0;
    timestampSequenceCount = 0;
    timestampSequenceIndex = 0;
    perfElapsedUs = 0;
    perfRecordCalls = 0;
    obdSettingsSyncNowMs = 0;
    deferredSettingsPersistNowMs = 0;
    deferredSettingsBackupNowMs = 0;
    deferredBleBondBackupNowMs = 0;
    storeSaveNowMs = 0;
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_process_runs_full_bundle_in_order_with_timing_records() {
    PeriodicMaintenanceModule::Providers providers = fullProviders();
    module.begin(providers);

    setTimestampSequence({100, 130});
    module.process(5000);

    TEST_ASSERT_EQUAL(6, callLogCount);
    TEST_ASSERT_EQUAL(CALL_PERF, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_OBD_SETTINGS_SYNC, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_DEFERRED_SETTINGS_PERSIST, callLog[2]);
    TEST_ASSERT_EQUAL(CALL_DEFERRED_SETTINGS_BACKUP, callLog[3]);
    TEST_ASSERT_EQUAL(CALL_DEFERRED_BLE_BOND_BACKUP, callLog[4]);
    TEST_ASSERT_EQUAL(CALL_STORE_SAVE, callLog[5]);

    TEST_ASSERT_EQUAL(1, perfRecordCalls);
    TEST_ASSERT_EQUAL(30u, perfElapsedUs);

    TEST_ASSERT_EQUAL(5000u, obdSettingsSyncNowMs);
    TEST_ASSERT_EQUAL(5000u, deferredSettingsPersistNowMs);
    TEST_ASSERT_EQUAL(5000u, deferredSettingsBackupNowMs);
    TEST_ASSERT_EQUAL(5000u, deferredBleBondBackupNowMs);
    TEST_ASSERT_EQUAL(5000u, storeSaveNowMs);
}

void test_process_defers_low_priority_persistence_when_pressured() {
    PeriodicMaintenanceModule::Providers providers = fullProviders();
    module.begin(providers);

    PeriodicMaintenanceModule::Context ctx;
    ctx.bleBackpressure = true;

    setTimestampSequence({100, 130});
    module.process(5000, ctx);

    TEST_ASSERT_EQUAL(2, callLogCount);
    TEST_ASSERT_EQUAL(CALL_PERF, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_OBD_SETTINGS_SYNC, callLog[1]);

    TEST_ASSERT_EQUAL(1, perfRecordCalls);
    TEST_ASSERT_EQUAL(30u, perfElapsedUs);

    TEST_ASSERT_EQUAL(5000u, obdSettingsSyncNowMs);
    TEST_ASSERT_EQUAL(0u, deferredSettingsPersistNowMs);
    TEST_ASSERT_EQUAL(0u, deferredSettingsBackupNowMs);
    TEST_ASSERT_EQUAL(0u, deferredBleBondBackupNowMs);
    TEST_ASSERT_EQUAL(0u, storeSaveNowMs);
}

void test_process_retries_settings_persist_when_pressure_clears() {
    PeriodicMaintenanceModule::Providers providers = fullProviders();
    module.begin(providers);

    PeriodicMaintenanceModule::Context pressuredCtx;
    pressuredCtx.loopOverloaded = true;
    module.process(5000, pressuredCtx);
    TEST_ASSERT_EQUAL(0u, deferredSettingsPersistNowMs);

    resetState();
    module.process(6000);

    TEST_ASSERT_EQUAL(6, callLogCount);
    TEST_ASSERT_EQUAL(CALL_DEFERRED_SETTINGS_PERSIST, callLog[2]);
    TEST_ASSERT_EQUAL(6000u, deferredSettingsPersistNowMs);
}

void test_process_skips_missing_providers_gracefully() {
    PeriodicMaintenanceModule::Providers providers;
    providers.runPerfReport = runPerfReport;
    providers.runObdSettingsSync = runObdSettingsSync;
    providers.runDeferredSettingsPersist = runDeferredSettingsPersist;
    providers.runDeferredSettingsBackup = runDeferredSettingsBackup;
    providers.runDeferredBleBondBackup = runDeferredBleBondBackup;
    providers.runStoreSave = runStoreSave;
    module.begin(providers);

    module.process(1200);

    TEST_ASSERT_EQUAL(6, callLogCount);
    TEST_ASSERT_EQUAL(CALL_PERF, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_OBD_SETTINGS_SYNC, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_DEFERRED_SETTINGS_PERSIST, callLog[2]);
    TEST_ASSERT_EQUAL(CALL_DEFERRED_SETTINGS_BACKUP, callLog[3]);
    TEST_ASSERT_EQUAL(CALL_DEFERRED_BLE_BOND_BACKUP, callLog[4]);
    TEST_ASSERT_EQUAL(CALL_STORE_SAVE, callLog[5]);

    TEST_ASSERT_EQUAL(0, perfRecordCalls);
}

void test_perf_elapsed_is_wrap_safe() {
    PeriodicMaintenanceModule::Providers providers;
    providers.timestampUs = nextTimestampUs;
    providers.runPerfReport = runPerfReport;
    providers.recordPerfReportUs = recordPerfReportUs;
    module.begin(providers);

    setTimestampSequence({0xFFFFFFF0u, 0x00000020u});
    module.process(10);

    TEST_ASSERT_EQUAL(1, perfRecordCalls);
    TEST_ASSERT_EQUAL(0x30u, perfElapsedUs);
    TEST_ASSERT_EQUAL(1, callLogCount);
    TEST_ASSERT_EQUAL(CALL_PERF, callLog[0]);
}

void test_empty_providers_is_safe_noop() {
    PeriodicMaintenanceModule::Providers providers;
    module.begin(providers);

    module.process(250);

    TEST_ASSERT_EQUAL(0, callLogCount);
    TEST_ASSERT_EQUAL(0, perfRecordCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_process_runs_full_bundle_in_order_with_timing_records);
    RUN_TEST(test_process_defers_low_priority_persistence_when_pressured);
    RUN_TEST(test_process_retries_settings_persist_when_pressure_clears);
    RUN_TEST(test_process_skips_missing_providers_gracefully);
    RUN_TEST(test_perf_elapsed_is_wrap_safe);
    RUN_TEST(test_empty_providers_is_safe_noop);
    return UNITY_END();
}
