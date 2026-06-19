#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_power_touch_module.cpp"

static LoopPowerTouchModule module;

enum CallId {
    CALL_POWER = 1,
    CALL_TOUCH = 2,
    CALL_TOUCH_PERF = 3,
    CALL_JITTER = 4,
    CALL_DMA = 5,
    CALL_HEAP = 6,
};

static int callLog[24];
static size_t callLogCount = 0;

static uint32_t tsSequence[8];
static size_t tsCount = 0;
static size_t tsIndex = 0;
static uint32_t microsNowValue = 0;

static bool touchReturnInSettings = false;

static uint32_t powerNowMs = 0;
static uint32_t touchNowMs = 0;
static bool touchBootPressed = false;
static uint32_t touchElapsedUs = 0;
static uint32_t loopJitterUs = 0;
static uint32_t heapFree = 0;
static uint32_t heapLargest = 0;
static uint32_t dmaFree = 0;
static uint32_t dmaLargest = 0;

static int providerPowerCalls = 0;
static int providerTouchCalls = 0;
static int jitterRecordCalls = 0;
static int dmaRefreshCalls = 0;
static int heapRecordCalls = 0;

static uint32_t providerFreeHeap = 0;
static uint32_t providerLargestHeap = 0;
static uint32_t providerCachedFreeDma = 0;
static uint32_t providerCachedLargestDma = 0;

static void noteCall(int id) {
    if (callLogCount < (sizeof(callLog) / sizeof(callLog[0]))) {
        callLog[callLogCount++] = id;
    }
}

static void resetState() {
    callLogCount = 0;
    tsCount = 0;
    tsIndex = 0;
    microsNowValue = 0;
    touchReturnInSettings = false;

    powerNowMs = 0;
    touchNowMs = 0;
    touchBootPressed = false;
    touchElapsedUs = 0;
    loopJitterUs = 0;
    heapFree = 0;
    heapLargest = 0;
    dmaFree = 0;
    dmaLargest = 0;

    providerPowerCalls = 0;
    providerTouchCalls = 0;
    jitterRecordCalls = 0;
    dmaRefreshCalls = 0;
    heapRecordCalls = 0;

    providerFreeHeap = 0;
    providerLargestHeap = 0;
    providerCachedFreeDma = 0;
    providerCachedLargestDma = 0;
}

static void setTimestampSequence(uint32_t first, uint32_t second) {
    tsSequence[0] = first;
    tsSequence[1] = second;
    tsCount = 2;
    tsIndex = 0;
}

static uint32_t nextTimestamp(void*) {
    if (tsCount == 0) {
        return 0;
    }
    if (tsIndex >= tsCount) {
        return tsSequence[tsCount - 1];
    }
    return tsSequence[tsIndex++];
}

static uint32_t microsNow(void*) {
    return microsNowValue;
}

static void runProviderPower(void*, uint32_t nowMs) {
    providerPowerCalls++;
    powerNowMs = nowMs;
    noteCall(CALL_POWER);
}

static bool runProviderTouch(void*, uint32_t nowMs, bool bootButtonPressed) {
    providerTouchCalls++;
    touchNowMs = nowMs;
    touchBootPressed = bootButtonPressed;
    noteCall(CALL_TOUCH);
    return touchReturnInSettings;
}

static void recordTouchUs(void*, uint32_t elapsedUs) {
    touchElapsedUs = elapsedUs;
    noteCall(CALL_TOUCH_PERF);
}

static void recordLoopJitterUs(void*, uint32_t jitterUs) {
    jitterRecordCalls++;
    loopJitterUs = jitterUs;
    noteCall(CALL_JITTER);
}

static void refreshDmaCache(void*) {
    dmaRefreshCalls++;
    noteCall(CALL_DMA);
}

static uint32_t readFreeHeap(void*) {
    return providerFreeHeap;
}

static uint32_t readLargestHeapBlock(void*) {
    return providerLargestHeap;
}

static uint32_t readCachedFreeDma(void*) {
    return providerCachedFreeDma;
}

static uint32_t readCachedLargestDma(void*) {
    return providerCachedLargestDma;
}

static void recordHeapStats(
    void*,
    uint32_t freeHeapValue,
    uint32_t largestHeapBlock,
    uint32_t cachedFreeDma,
    uint32_t cachedLargestDma) {
    heapRecordCalls++;
    heapFree = freeHeapValue;
    heapLargest = largestHeapBlock;
    dmaFree = cachedFreeDma;
    dmaLargest = cachedLargestDma;
    noteCall(CALL_HEAP);
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_provider_path_not_in_settings_runs_power_touch_and_perf_only() {
    LoopPowerTouchModule::Providers providers;
    providers.timestampUs = nextTimestamp;
    providers.runPowerProcess = runProviderPower;
    providers.runTouchUiProcess = runProviderTouch;
    providers.recordTouchUs = recordTouchUs;
    providers.microsNow = microsNow;
    providers.recordLoopJitterUs = recordLoopJitterUs;
    providers.refreshDmaCache = refreshDmaCache;
    providers.recordHeapStats = recordHeapStats;
    providers.readFreeHeap = readFreeHeap;
    providers.readLargestHeapBlock = readLargestHeapBlock;
    providers.readCachedFreeDma = readCachedFreeDma;
    providers.readCachedLargestDma = readCachedLargestDma;
    module.begin(providers);

    touchReturnInSettings = false;
    setTimestampSequence(100, 140);
    microsNowValue = 9999;

    LoopPowerTouchContext ctx;
    ctx.nowMs = 1234;
    ctx.loopStartUs = 8000;
    ctx.bootButtonPressed = true;

    const LoopPowerTouchResult result = module.process(ctx);

    TEST_ASSERT_FALSE(result.inSettings);
    TEST_ASSERT_FALSE(result.shouldReturnEarly);
    TEST_ASSERT_EQUAL(1, providerPowerCalls);
    TEST_ASSERT_EQUAL(1, providerTouchCalls);
    TEST_ASSERT_EQUAL(1234u, powerNowMs);
    TEST_ASSERT_EQUAL(1234u, touchNowMs);
    TEST_ASSERT_TRUE(touchBootPressed);
    TEST_ASSERT_EQUAL(40u, touchElapsedUs);
    TEST_ASSERT_EQUAL(0u, loopJitterUs);
    TEST_ASSERT_EQUAL(0, dmaRefreshCalls);
    TEST_ASSERT_EQUAL(0, heapRecordCalls);

    TEST_ASSERT_EQUAL(3, callLogCount);
    TEST_ASSERT_EQUAL(CALL_POWER, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_TOUCH, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_TOUCH_PERF, callLog[2]);
}

void test_in_settings_triggers_early_return_and_records_jitter_dma_heap() {
    LoopPowerTouchModule::Providers providers;
    providers.timestampUs = nextTimestamp;
    providers.runPowerProcess = runProviderPower;
    providers.runTouchUiProcess = runProviderTouch;
    providers.microsNow = microsNow;
    providers.recordTouchUs = recordTouchUs;
    providers.recordLoopJitterUs = recordLoopJitterUs;
    providers.refreshDmaCache = refreshDmaCache;
    providers.readFreeHeap = readFreeHeap;
    providers.readLargestHeapBlock = readLargestHeapBlock;
    providers.readCachedFreeDma = readCachedFreeDma;
    providers.readCachedLargestDma = readCachedLargestDma;
    providers.recordHeapStats = recordHeapStats;
    module.begin(providers);

    touchReturnInSettings = true;
    setTimestampSequence(500, 555);
    microsNowValue = 10300;
    providerFreeHeap = 111;
    providerLargestHeap = 222;
    providerCachedFreeDma = 333;
    providerCachedLargestDma = 444;

    LoopPowerTouchContext ctx;
    ctx.nowMs = 2048;
    ctx.loopStartUs = 10000;
    ctx.bootButtonPressed = false;

    const LoopPowerTouchResult result = module.process(ctx);

    TEST_ASSERT_TRUE(result.inSettings);
    TEST_ASSERT_TRUE(result.shouldReturnEarly);
    TEST_ASSERT_EQUAL(55u, touchElapsedUs);
    TEST_ASSERT_EQUAL(1, jitterRecordCalls);
    TEST_ASSERT_EQUAL(300u, loopJitterUs);
    TEST_ASSERT_EQUAL(1, dmaRefreshCalls);
    TEST_ASSERT_EQUAL(1, heapRecordCalls);
    TEST_ASSERT_EQUAL(111u, heapFree);
    TEST_ASSERT_EQUAL(222u, heapLargest);
    TEST_ASSERT_EQUAL(333u, dmaFree);
    TEST_ASSERT_EQUAL(444u, dmaLargest);

    TEST_ASSERT_EQUAL(6, callLogCount);
    TEST_ASSERT_EQUAL(CALL_POWER, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_TOUCH, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_TOUCH_PERF, callLog[2]);
    TEST_ASSERT_EQUAL(CALL_JITTER, callLog[3]);
    TEST_ASSERT_EQUAL(CALL_DMA, callLog[4]);
    TEST_ASSERT_EQUAL(CALL_HEAP, callLog[5]);
}

void test_provider_path_keeps_wrap_safe_timing() {
    LoopPowerTouchModule::Providers providers;
    providers.timestampUs = nextTimestamp;
    providers.microsNow = microsNow;
    providers.runPowerProcess = runProviderPower;
    providers.runTouchUiProcess = runProviderTouch;
    providers.recordTouchUs = recordTouchUs;
    providers.recordLoopJitterUs = recordLoopJitterUs;
    module.begin(providers);

    touchReturnInSettings = true;
    setTimestampSequence(0xFFFFFFF0u, 0x00000010u);
    microsNowValue = 0x00000020u;

    LoopPowerTouchContext ctx;
    ctx.loopStartUs = 0xFFFFFFF0u;

    const LoopPowerTouchResult result = module.process(ctx);

    TEST_ASSERT_TRUE(result.inSettings);
    TEST_ASSERT_TRUE(result.shouldReturnEarly);
    TEST_ASSERT_EQUAL(1, providerPowerCalls);
    TEST_ASSERT_EQUAL(1, providerTouchCalls);
    TEST_ASSERT_EQUAL(0x20u, touchElapsedUs);
    TEST_ASSERT_EQUAL(0x30u, loopJitterUs);
}

void test_settings_heap_sampling_uses_telemetry_cadence() {
    LoopPowerTouchModule::Providers providers;
    providers.timestampUs = nextTimestamp;
    providers.runPowerProcess = runProviderPower;
    providers.runTouchUiProcess = runProviderTouch;
    providers.microsNow = microsNow;
    providers.recordTouchUs = recordTouchUs;
    providers.recordLoopJitterUs = recordLoopJitterUs;
    providers.refreshDmaCache = refreshDmaCache;
    providers.readFreeHeap = readFreeHeap;
    providers.readLargestHeapBlock = readLargestHeapBlock;
    providers.readCachedFreeDma = readCachedFreeDma;
    providers.readCachedLargestDma = readCachedLargestDma;
    providers.recordHeapStats = recordHeapStats;
    module.begin(providers);

    touchReturnInSettings = true;
    setTimestampSequence(100, 125);
    microsNowValue = 200;
    providerFreeHeap = 111;
    providerLargestHeap = 222;
    providerCachedFreeDma = 333;
    providerCachedLargestDma = 444;

    LoopPowerTouchContext ctx;
    ctx.nowMs = 2048;
    ctx.loopStartUs = 100;

    // Settings mode exits before LoopTelemetryModule, so this path must
    // keep jitter every loop but rate-limit expensive heap/DMA sampling.
    module.process(ctx);
    TEST_ASSERT_EQUAL(1, heapRecordCalls);
    TEST_ASSERT_EQUAL(1, dmaRefreshCalls);

    for (uint8_t i = 1; i < LoopPowerTouchModule::HEAP_SAMPLE_DIVISOR; i++) {
        module.process(ctx);
    }
    TEST_ASSERT_EQUAL(1, heapRecordCalls);
    TEST_ASSERT_EQUAL(1, dmaRefreshCalls);

    module.process(ctx);
    TEST_ASSERT_EQUAL(2, heapRecordCalls);
    TEST_ASSERT_EQUAL(2, dmaRefreshCalls);

    TEST_ASSERT_EQUAL(
        static_cast<int>(LoopPowerTouchModule::HEAP_SAMPLE_DIVISOR) + 1,
        jitterRecordCalls);
    TEST_ASSERT_EQUAL(
        static_cast<int>(LoopPowerTouchModule::HEAP_SAMPLE_DIVISOR) + 1,
        providerPowerCalls);
    TEST_ASSERT_EQUAL(
        static_cast<int>(LoopPowerTouchModule::HEAP_SAMPLE_DIVISOR) + 1,
        providerTouchCalls);
}

void test_empty_providers_and_context_is_safe() {
    LoopPowerTouchModule::Providers providers;
    module.begin(providers);

    const LoopPowerTouchResult result = module.process(LoopPowerTouchContext{});

    TEST_ASSERT_FALSE(result.inSettings);
    TEST_ASSERT_FALSE(result.shouldReturnEarly);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_provider_path_not_in_settings_runs_power_touch_and_perf_only);
    RUN_TEST(test_in_settings_triggers_early_return_and_records_jitter_dma_heap);
    RUN_TEST(test_provider_path_keeps_wrap_safe_timing);
    RUN_TEST(test_settings_heap_sampling_uses_telemetry_cadence);
    RUN_TEST(test_empty_providers_and_context_is_safe);
    return UNITY_END();
}
