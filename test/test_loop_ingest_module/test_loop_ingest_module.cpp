#include <unity.h>
#include <initializer_list>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_ingest_module.cpp"

static LoopIngestModule module;

enum CallId {
    CALL_PROVIDER_BLE_PROCESS = 1,
    CALL_RECORD_BLE_PROCESS = 2,
    CALL_PROVIDER_BLE_DRAIN = 3,
    CALL_RECORD_BLE_DRAIN = 4,
};

static int callLog[32];
static size_t callLogCount = 0;

static uint32_t timestampSequence[16];
static size_t timestampSequenceCount = 0;
static size_t timestampSequenceIndex = 0;

static int providerBleProcessCalls = 0;
static int providerBleDrainCalls = 0;
static bool scriptedBackpressure = false;

static int recordBleProcessCalls = 0;
static uint32_t bleProcessElapsedUs = 0;
static int recordBleDrainCalls = 0;
static uint32_t bleDrainElapsedUs = 0;

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

static void providerRunBleProcess(void*) {
    providerBleProcessCalls++;
    noteCall(CALL_PROVIDER_BLE_PROCESS);
}

static void recordBleProcessUs(void*, uint32_t elapsedUs) {
    recordBleProcessCalls++;
    bleProcessElapsedUs = elapsedUs;
    noteCall(CALL_RECORD_BLE_PROCESS);
}

static void providerRunBleDrain(void*) {
    providerBleDrainCalls++;
    noteCall(CALL_PROVIDER_BLE_DRAIN);
}

static void recordBleDrainUs(void*, uint32_t elapsedUs) {
    recordBleDrainCalls++;
    bleDrainElapsedUs = elapsedUs;
    noteCall(CALL_RECORD_BLE_DRAIN);
}

static bool readBleBackpressure(void*) {
    return scriptedBackpressure;
}

static LoopIngestModule::Providers makeDefaultProviders() {
    LoopIngestModule::Providers providers;
    providers.timestampUs = nextTimestampUs;
    providers.runBleProcess = providerRunBleProcess;
    providers.recordBleProcessUs = recordBleProcessUs;
    providers.runBleDrain = providerRunBleDrain;
    providers.recordBleDrainUs = recordBleDrainUs;
    providers.readBleBackpressure = readBleBackpressure;
    return providers;
}

static void resetState() {
    callLogCount = 0;
    timestampSequenceCount = 0;
    timestampSequenceIndex = 0;
    providerBleProcessCalls = 0;
    providerBleDrainCalls = 0;
    scriptedBackpressure = false;
    recordBleProcessCalls = 0;
    bleProcessElapsedUs = 0;
    recordBleDrainCalls = 0;
    bleDrainElapsedUs = 0;
}

void setUp() {
    resetState();
    module.begin(makeDefaultProviders());
}

void tearDown() {}

void test_process_runs_provider_pipeline_and_perf_records() {
    scriptedBackpressure = true;
    setTimestampSequence({100, 130, 200, 260});

    LoopIngestContext ctx;
    ctx.nowMs = 5000;
    ctx.bleProcessEnabled = true;

    const LoopIngestResult result = module.process(ctx);

    TEST_ASSERT_EQUAL(1, providerBleProcessCalls);
    TEST_ASSERT_EQUAL(1, providerBleDrainCalls);
    TEST_ASSERT_EQUAL(1, recordBleProcessCalls);
    TEST_ASSERT_EQUAL(30u, bleProcessElapsedUs);
    TEST_ASSERT_EQUAL(1, recordBleDrainCalls);
    TEST_ASSERT_EQUAL(60u, bleDrainElapsedUs);

    TEST_ASSERT_TRUE(result.bleBackpressure);
    TEST_ASSERT_TRUE(result.skipLateNonCoreThisLoop);
    TEST_ASSERT_TRUE(result.overloadLateThisLoop);

    TEST_ASSERT_EQUAL(4, callLogCount);
    TEST_ASSERT_EQUAL(CALL_PROVIDER_BLE_PROCESS, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_RECORD_BLE_PROCESS, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_PROVIDER_BLE_DRAIN, callLog[2]);
    TEST_ASSERT_EQUAL(CALL_RECORD_BLE_DRAIN, callLog[3]);
}

void test_ble_process_disabled_skips_ble_process_only() {
    setTimestampSequence({1000, 1010, 1100, 1110});

    LoopIngestContext ctx;
    ctx.nowMs = 99;
    ctx.bleProcessEnabled = false;

    module.process(ctx);

    TEST_ASSERT_EQUAL(0, providerBleProcessCalls);
    TEST_ASSERT_EQUAL(1, providerBleDrainCalls);
    TEST_ASSERT_EQUAL(0, recordBleProcessCalls);
    TEST_ASSERT_EQUAL(1, recordBleDrainCalls);
}

void test_missing_timing_hooks_still_runs_operations() {
    LoopIngestModule::Providers providers = makeDefaultProviders();
    providers.timestampUs = nullptr;
    module.begin(providers);

    LoopIngestContext ctx;
    ctx.nowMs = 88;
    ctx.bleProcessEnabled = true;
    module.process(ctx);

    TEST_ASSERT_EQUAL(1, providerBleProcessCalls);
    TEST_ASSERT_EQUAL(1, providerBleDrainCalls);
    TEST_ASSERT_EQUAL(0, recordBleProcessCalls);
    TEST_ASSERT_EQUAL(0, recordBleDrainCalls);
}

void test_empty_providers_is_safe_and_merges_flags() {
    LoopIngestModule::Providers providers;
    module.begin(providers);

    LoopIngestContext ctx;
    ctx.skipNonCoreThisLoop = true;
    ctx.overloadThisLoop = true;
    const LoopIngestResult result = module.process(ctx);

    TEST_ASSERT_FALSE(result.bleBackpressure);
    TEST_ASSERT_TRUE(result.skipLateNonCoreThisLoop);
    TEST_ASSERT_TRUE(result.overloadLateThisLoop);
    TEST_ASSERT_EQUAL(0, callLogCount);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_process_runs_provider_pipeline_and_perf_records);
    RUN_TEST(test_ble_process_disabled_skips_ble_process_only);
    RUN_TEST(test_missing_timing_hooks_still_runs_operations);
    RUN_TEST(test_empty_providers_is_safe_and_merges_flags);
    return UNITY_END();
}
