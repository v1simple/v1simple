#include <unity.h>
#include <initializer_list>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_tail_module.cpp"

static LoopTailModule module;

enum CallId {
    CALL_DRAIN = 1,
    CALL_RECORD = 2,
    CALL_YIELD = 3,
    CALL_LOOP_JITTER = 4,
};

static int callLog[16];
static size_t callLogCount = 0;

static uint32_t perfTsSequence[8];
static size_t perfTsCount = 0;
static size_t perfTsIndex = 0;

static uint32_t loopMicrosNow = 0;
static int drainCalls = 0;
static int recordCalls = 0;
static int yieldCalls = 0;
static uint32_t recordedDrainElapsedUs = 0;
static int loopJitterCalls = 0;
static uint32_t recordedLoopJitterUs = 0;

static void resetState() {
    callLogCount = 0;
    perfTsCount = 0;
    perfTsIndex = 0;
    loopMicrosNow = 0;
    drainCalls = 0;
    recordCalls = 0;
    yieldCalls = 0;
    recordedDrainElapsedUs = 0;
    loopJitterCalls = 0;
    recordedLoopJitterUs = 0;
}

static void noteCall(int id) {
    if (callLogCount < (sizeof(callLog) / sizeof(callLog[0]))) {
        callLog[callLogCount++] = id;
    }
}

static void setPerfTsSequence(std::initializer_list<uint32_t> values) {
    perfTsCount = values.size();
    perfTsIndex = 0;
    size_t i = 0;
    for (uint32_t value : values) {
        perfTsSequence[i++] = value;
    }
}

static uint32_t nextPerfTs(void*) {
    if (perfTsCount == 0) {
        return 0;
    }
    if (perfTsIndex >= perfTsCount) {
        return perfTsSequence[perfTsCount - 1];
    }
    return perfTsSequence[perfTsIndex++];
}

static uint32_t currentLoopMicros(void*) {
    return loopMicrosNow;
}

static void runBleDrain(void*) {
    drainCalls++;
    noteCall(CALL_DRAIN);
}

static void recordBleDrainUs(void*, uint32_t elapsedUs) {
    recordCalls++;
    recordedDrainElapsedUs = elapsedUs;
    noteCall(CALL_RECORD);
}

static void yieldOneTick(void*) {
    yieldCalls++;
    noteCall(CALL_YIELD);
}

static void recordLoopJitterUs(void*, uint32_t elapsedUs) {
    loopJitterCalls++;
    recordedLoopJitterUs = elapsedUs;
    noteCall(CALL_LOOP_JITTER);
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_backpressure_runs_drain_records_and_yield_in_order() {
    LoopTailModule::Providers providers;
    providers.perfTimestampUs = nextPerfTs;
    providers.loopMicrosUs = currentLoopMicros;
    providers.runBleDrain = runBleDrain;
    providers.recordBleDrainUs = recordBleDrainUs;
    providers.recordLoopJitterUs = recordLoopJitterUs;
    providers.yieldOneTick = yieldOneTick;
    module.begin(providers);

    setPerfTsSequence({1000, 1125});
    loopMicrosNow = 4600;
    const uint32_t durationUs = module.process(true, 4000);

    TEST_ASSERT_EQUAL(1, drainCalls);
    TEST_ASSERT_EQUAL(1, recordCalls);
    TEST_ASSERT_EQUAL(1, yieldCalls);
    TEST_ASSERT_EQUAL(1, loopJitterCalls);
    TEST_ASSERT_EQUAL(125u, recordedDrainElapsedUs);
    TEST_ASSERT_EQUAL(600u, recordedLoopJitterUs);
    TEST_ASSERT_EQUAL(600u, durationUs);

    TEST_ASSERT_EQUAL(4, callLogCount);
    TEST_ASSERT_EQUAL(CALL_DRAIN, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_RECORD, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_YIELD, callLog[2]);
    TEST_ASSERT_EQUAL(CALL_LOOP_JITTER, callLog[3]);
}

void test_no_backpressure_skips_drain_and_still_yields() {
    LoopTailModule::Providers providers;
    providers.perfTimestampUs = nextPerfTs;
    providers.loopMicrosUs = currentLoopMicros;
    providers.runBleDrain = runBleDrain;
    providers.recordBleDrainUs = recordBleDrainUs;
    providers.recordLoopJitterUs = recordLoopJitterUs;
    providers.yieldOneTick = yieldOneTick;
    module.begin(providers);

    loopMicrosNow = 9050;
    const uint32_t durationUs = module.process(false, 9000);

    TEST_ASSERT_EQUAL(0, drainCalls);
    TEST_ASSERT_EQUAL(0, recordCalls);
    TEST_ASSERT_EQUAL(1, yieldCalls);
    TEST_ASSERT_EQUAL(1, loopJitterCalls);
    TEST_ASSERT_EQUAL(50u, recordedLoopJitterUs);
    TEST_ASSERT_EQUAL(50u, durationUs);
    TEST_ASSERT_EQUAL(2, callLogCount);
    TEST_ASSERT_EQUAL(CALL_YIELD, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_LOOP_JITTER, callLog[1]);
}

void test_recorded_elapsed_is_wrap_safe() {
    LoopTailModule::Providers providers;
    providers.perfTimestampUs = nextPerfTs;
    providers.runBleDrain = runBleDrain;
    providers.recordBleDrainUs = recordBleDrainUs;
    providers.recordLoopJitterUs = recordLoopJitterUs;
    providers.yieldOneTick = yieldOneTick;
    module.begin(providers);

    setPerfTsSequence({0xFFFFFFF0u, 0x00000010u});
    const uint32_t durationUs = module.process(true, 0);

    TEST_ASSERT_EQUAL(1, drainCalls);
    TEST_ASSERT_EQUAL(1, recordCalls);
    TEST_ASSERT_EQUAL(0x20u, recordedDrainElapsedUs);
    TEST_ASSERT_EQUAL(1, yieldCalls);
    TEST_ASSERT_EQUAL(0, loopJitterCalls);
    TEST_ASSERT_EQUAL(0u, durationUs);
}

void test_force_drain_runs_even_without_backpressure() {
    LoopTailModule::Providers providers;
    providers.perfTimestampUs = nextPerfTs;
    providers.loopMicrosUs = currentLoopMicros;
    providers.runBleDrain = runBleDrain;
    providers.recordBleDrainUs = recordBleDrainUs;
    providers.recordLoopJitterUs = recordLoopJitterUs;
    providers.yieldOneTick = yieldOneTick;
    module.begin(providers);

    setPerfTsSequence({200, 260});
    loopMicrosNow = 4200;
    const uint32_t durationUs = module.process(false, 4000, true);

    TEST_ASSERT_EQUAL(1, drainCalls);
    TEST_ASSERT_EQUAL(1, recordCalls);
    TEST_ASSERT_EQUAL(60u, recordedDrainElapsedUs);
    TEST_ASSERT_EQUAL(1, yieldCalls);
    TEST_ASSERT_EQUAL(1, loopJitterCalls);
    TEST_ASSERT_EQUAL(200u, recordedLoopJitterUs);
    TEST_ASSERT_EQUAL(200u, durationUs);
}

void test_empty_providers_is_safe_noop() {
    LoopTailModule::Providers providers;
    module.begin(providers);

    const uint32_t durationUs = module.process(true, 777);

    TEST_ASSERT_EQUAL(0u, durationUs);
    TEST_ASSERT_EQUAL(0, drainCalls);
    TEST_ASSERT_EQUAL(0, recordCalls);
    TEST_ASSERT_EQUAL(0, yieldCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_backpressure_runs_drain_records_and_yield_in_order);
    RUN_TEST(test_no_backpressure_skips_drain_and_still_yields);
    RUN_TEST(test_recorded_elapsed_is_wrap_safe);
    RUN_TEST(test_force_drain_runs_even_without_backpressure);
    RUN_TEST(test_empty_providers_is_safe_noop);
    return UNITY_END();
}
