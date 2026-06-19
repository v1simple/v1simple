#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_telemetry_module.cpp"

static LoopTelemetryModule module;

enum CallId {
    CALL_JITTER = 1,
    CALL_REFRESH_DMA = 2,
    CALL_HEAP_STATS = 3,
};

static int callLog[16];
static size_t callLogCount = 0;

static uint32_t nowMicros = 0;
static int refreshDmaCalls = 0;
static int recordJitterCalls = 0;
static int recordHeapCalls = 0;
static uint32_t recordedJitterUs = 0;
static uint32_t recordedFreeHeap = 0;
static uint32_t recordedLargestHeapBlock = 0;
static uint32_t recordedCachedFreeDma = 0;
static uint32_t recordedCachedLargestDma = 0;

static uint32_t freeHeapValue = 0;
static uint32_t largestHeapBlockValue = 0;
static uint32_t cachedFreeDmaValue = 0;
static uint32_t cachedLargestDmaValue = 0;

static void resetState() {
    callLogCount = 0;
    nowMicros = 0;
    refreshDmaCalls = 0;
    recordJitterCalls = 0;
    recordHeapCalls = 0;
    recordedJitterUs = 0;
    recordedFreeHeap = 0;
    recordedLargestHeapBlock = 0;
    recordedCachedFreeDma = 0;
    recordedCachedLargestDma = 0;
    freeHeapValue = 0;
    largestHeapBlockValue = 0;
    cachedFreeDmaValue = 0;
    cachedLargestDmaValue = 0;
}

static void noteCall(int id) {
    if (callLogCount < (sizeof(callLog) / sizeof(callLog[0]))) {
        callLog[callLogCount++] = id;
    }
}

static uint32_t readMicrosNow(void*) {
    return nowMicros;
}

static void recordLoopJitter(void*, uint32_t jitterUs) {
    recordJitterCalls++;
    recordedJitterUs = jitterUs;
    noteCall(CALL_JITTER);
}

static void refreshDmaCache(void*) {
    refreshDmaCalls++;
    noteCall(CALL_REFRESH_DMA);
}

static uint32_t readFreeHeap(void*) {
    return freeHeapValue;
}

static uint32_t readLargestHeapBlock(void*) {
    return largestHeapBlockValue;
}

static uint32_t readCachedFreeDma(void*) {
    return cachedFreeDmaValue;
}

static uint32_t readCachedLargestDma(void*) {
    return cachedLargestDmaValue;
}

static void recordHeapStats(
    void*,
    uint32_t freeHeap,
    uint32_t largestHeapBlock,
    uint32_t cachedFreeDma,
    uint32_t cachedLargestDma) {
    recordHeapCalls++;
    recordedFreeHeap = freeHeap;
    recordedLargestHeapBlock = largestHeapBlock;
    recordedCachedFreeDma = cachedFreeDma;
    recordedCachedLargestDma = cachedLargestDma;
    noteCall(CALL_HEAP_STATS);
}

void setUp() {
    resetState();
    module = LoopTelemetryModule();  // reset cadence counter
}

void tearDown() {}

void test_records_jitter_dma_cache_and_heap_stats() {
    LoopTelemetryModule::Providers providers;
    providers.microsNow = readMicrosNow;
    providers.recordLoopJitterUs = recordLoopJitter;
    providers.refreshDmaCache = refreshDmaCache;
    providers.readFreeHeap = readFreeHeap;
    providers.readLargestHeapBlock = readLargestHeapBlock;
    providers.readCachedFreeDma = readCachedFreeDma;
    providers.readCachedLargestDma = readCachedLargestDma;
    providers.recordHeapStats = recordHeapStats;
    module.begin(providers);

    nowMicros = 1200;
    freeHeapValue = 101;
    largestHeapBlockValue = 202;
    cachedFreeDmaValue = 303;
    cachedLargestDmaValue = 404;
    module.process(1000);

    TEST_ASSERT_EQUAL(1, recordJitterCalls);
    TEST_ASSERT_EQUAL(200u, recordedJitterUs);
    TEST_ASSERT_EQUAL(1, refreshDmaCalls);
    TEST_ASSERT_EQUAL(1, recordHeapCalls);
    TEST_ASSERT_EQUAL(101u, recordedFreeHeap);
    TEST_ASSERT_EQUAL(202u, recordedLargestHeapBlock);
    TEST_ASSERT_EQUAL(303u, recordedCachedFreeDma);
    TEST_ASSERT_EQUAL(404u, recordedCachedLargestDma);
    TEST_ASSERT_EQUAL(3, callLogCount);
    TEST_ASSERT_EQUAL(CALL_JITTER, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_REFRESH_DMA, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_HEAP_STATS, callLog[2]);
}

void test_jitter_elapsed_is_wrap_safe() {
    LoopTelemetryModule::Providers providers;
    providers.microsNow = readMicrosNow;
    providers.recordLoopJitterUs = recordLoopJitter;
    module.begin(providers);

    nowMicros = 0x00000020u;
    module.process(0xFFFFFFF0u);

    TEST_ASSERT_EQUAL(1, recordJitterCalls);
    TEST_ASSERT_EQUAL(0x30u, recordedJitterUs);
}

void test_missing_heap_input_skips_heap_recording() {
    LoopTelemetryModule::Providers providers;
    providers.microsNow = readMicrosNow;
    providers.recordLoopJitterUs = recordLoopJitter;
    providers.refreshDmaCache = refreshDmaCache;
    providers.readFreeHeap = readFreeHeap;
    providers.readLargestHeapBlock = readLargestHeapBlock;
    providers.readCachedFreeDma = readCachedFreeDma;
    providers.recordHeapStats = recordHeapStats;
    module.begin(providers);

    nowMicros = 1500;
    module.process(1000);

    TEST_ASSERT_EQUAL(1, recordJitterCalls);
    TEST_ASSERT_EQUAL(1, refreshDmaCalls);
    TEST_ASSERT_EQUAL(0, recordHeapCalls);
    TEST_ASSERT_EQUAL(2, callLogCount);
    TEST_ASSERT_EQUAL(CALL_JITTER, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_REFRESH_DMA, callLog[1]);
}

void test_heap_sampling_skips_between_cadence_hits() {
    LoopTelemetryModule mod;
    LoopTelemetryModule::Providers providers;
    providers.microsNow = readMicrosNow;
    providers.recordLoopJitterUs = recordLoopJitter;
    providers.refreshDmaCache = refreshDmaCache;
    providers.readFreeHeap = readFreeHeap;
    providers.readLargestHeapBlock = readLargestHeapBlock;
    providers.readCachedFreeDma = readCachedFreeDma;
    providers.readCachedLargestDma = readCachedLargestDma;
    providers.recordHeapStats = recordHeapStats;
    mod.begin(providers);

    nowMicros = 100;

    // First call samples (counter initialised to DIVISOR-1)
    mod.process(0);
    TEST_ASSERT_EQUAL(1, recordHeapCalls);
    TEST_ASSERT_EQUAL(1, refreshDmaCalls);

    // Next DIVISOR-1 calls skip heap sampling
    for (uint8_t i = 1; i < LoopTelemetryModule::HEAP_SAMPLE_DIVISOR; i++) {
        mod.process(0);
    }
    TEST_ASSERT_EQUAL(1, recordHeapCalls);
    TEST_ASSERT_EQUAL(1, refreshDmaCalls);

    // The DIVISOR-th call after the first triggers another sample
    mod.process(0);
    TEST_ASSERT_EQUAL(2, recordHeapCalls);
    TEST_ASSERT_EQUAL(2, refreshDmaCalls);

    // Jitter is still recorded every call
    TEST_ASSERT_EQUAL(
        static_cast<int>(LoopTelemetryModule::HEAP_SAMPLE_DIVISOR) + 1,
        recordJitterCalls);
}

void test_empty_providers_is_safe_noop() {
    LoopTelemetryModule::Providers providers;
    module.begin(providers);

    module.process(777);

    TEST_ASSERT_EQUAL(0, recordJitterCalls);
    TEST_ASSERT_EQUAL(0, refreshDmaCalls);
    TEST_ASSERT_EQUAL(0, recordHeapCalls);
    TEST_ASSERT_EQUAL(0, callLogCount);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_records_jitter_dma_cache_and_heap_stats);
    RUN_TEST(test_jitter_elapsed_is_wrap_safe);
    RUN_TEST(test_missing_heap_input_skips_heap_recording);
    RUN_TEST(test_heap_sampling_skips_between_cadence_hits);
    RUN_TEST(test_empty_providers_is_safe_noop);
    return UNITY_END();
}
