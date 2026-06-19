#include <unity.h>
#include <initializer_list>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_display_module.cpp"

static LoopDisplayModule module;

enum CallId {
    CALL_COLLECT = 1,
    CALL_PARSED = 2,
    CALL_NOTIFY_PERF = 3,
    CALL_PIPELINE = 4,
    CALL_DISP_PIPE_PERF = 5,
    CALL_REFRESH = 6,
};

static int callLog[32];
static size_t callLogCount = 0;

static uint32_t perfTsSequence[8];
static size_t perfTsCount = 0;
static size_t perfTsIndex = 0;

static uint32_t displayNowMs = 0;
static ParsedFrameSignal parsedSignal;
static DisplayOrchestrationParsedResult parsedResult;
static DisplayOrchestrationRefreshResult refreshResult;

static DisplayOrchestrationParsedContext lastParsedCtx;
static DisplayOrchestrationRefreshContext lastRefreshCtx;
static uint32_t lastPipelineNowMs = 0;

static uint32_t dispPipeElapsedUs = 0;
static uint32_t notifyElapsedMs = 0;

static int collectCalls = 0;
static int parsedCalls = 0;
static int refreshCalls = 0;
static int providerPipelineCalls = 0;

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

static void resetState() {
    callLogCount = 0;
    perfTsCount = 0;
    perfTsIndex = 0;

    displayNowMs = 0;
    parsedSignal = ParsedFrameSignal{};
    parsedResult = DisplayOrchestrationParsedResult{};
    refreshResult = DisplayOrchestrationRefreshResult{};
    lastParsedCtx = DisplayOrchestrationParsedContext{};
    lastRefreshCtx = DisplayOrchestrationRefreshContext{};
    lastPipelineNowMs = 0;
    dispPipeElapsedUs = 0;
    notifyElapsedMs = 0;
    collectCalls = 0;
    parsedCalls = 0;
    refreshCalls = 0;
    providerPipelineCalls = 0;
}

static uint32_t readDisplayNowMs(void*) {
    return displayNowMs;
}

static ParsedFrameSignal collectParsedSignal(void*) {
    collectCalls++;
    noteCall(CALL_COLLECT);
    return parsedSignal;
}

static DisplayOrchestrationParsedResult runParsedFrame(
    void*,
    const DisplayOrchestrationParsedContext& ctx) {
    parsedCalls++;
    lastParsedCtx = ctx;
    noteCall(CALL_PARSED);
    return parsedResult;
}

static DisplayOrchestrationRefreshResult runRefresh(
    void*,
    const DisplayOrchestrationRefreshContext& ctx) {
    refreshCalls++;
    lastRefreshCtx = ctx;
    noteCall(CALL_REFRESH);
    return refreshResult;
}

static void runProviderPipeline(void*, uint32_t nowMs) {
    providerPipelineCalls++;
    lastPipelineNowMs = nowMs;
    noteCall(CALL_PIPELINE);
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

static void recordDispPipeUs(void*, uint32_t elapsedUs) {
    dispPipeElapsedUs = elapsedUs;
    noteCall(CALL_DISP_PIPE_PERF);
}

static void recordNotifyToDisplayMs(void*, uint32_t elapsedMs) {
    notifyElapsedMs = elapsedMs;
    noteCall(CALL_NOTIFY_PERF);
}

static LoopDisplayModule::Providers makeDefaultProviders() {
    LoopDisplayModule::Providers providers;
    providers.readDisplayNowMs = readDisplayNowMs;
    providers.collectParsedSignal = collectParsedSignal;
    providers.runParsedFrame = runParsedFrame;
    providers.runLightweightRefresh = runRefresh;
    providers.runDisplayPipeline = runProviderPipeline;
    providers.timestampUs = nextPerfTs;
    providers.recordDispPipeUs = recordDispPipeUs;
    providers.recordNotifyToDisplayMs = recordNotifyToDisplayMs;
    return providers;
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_process_full_pipeline_with_provider_pipeline_and_perf_records() {
    module.begin(makeDefaultProviders());

    displayNowMs = 1300;
    parsedSignal = ParsedFrameSignal{true, 1200};
    parsedResult = DisplayOrchestrationParsedResult{true};
    refreshResult.signalPriorityActive = true;
    setPerfTsSequence({200, 260});

    LoopDisplayContext ctx;
    ctx.nowMs = 900;
    ctx.bootSplashHoldActive = false;
    ctx.overloadLateThisLoop = true;

    module.process(ctx);

    TEST_ASSERT_EQUAL(1, collectCalls);
    TEST_ASSERT_EQUAL(1, parsedCalls);
    TEST_ASSERT_EQUAL(1, refreshCalls);
    TEST_ASSERT_EQUAL(1, providerPipelineCalls);
    TEST_ASSERT_EQUAL(60u, dispPipeElapsedUs);
    TEST_ASSERT_EQUAL(100u, notifyElapsedMs);
    TEST_ASSERT_EQUAL(1300u, lastPipelineNowMs);

    TEST_ASSERT_TRUE(lastParsedCtx.parsedReady);
    TEST_ASSERT_EQUAL(1300u, lastParsedCtx.nowMs);
    TEST_ASSERT_FALSE(lastParsedCtx.bootSplashHoldActive);

    TEST_ASSERT_EQUAL(1300u, lastRefreshCtx.nowMs);
    TEST_ASSERT_FALSE(lastRefreshCtx.bootSplashHoldActive);
    TEST_ASSERT_TRUE(lastRefreshCtx.overloadLateThisLoop);
    TEST_ASSERT_TRUE(lastRefreshCtx.pipelineRanThisLoop);

    TEST_ASSERT_EQUAL(6, callLogCount);
    TEST_ASSERT_EQUAL(CALL_COLLECT, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_PARSED, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_NOTIFY_PERF, callLog[2]);
    TEST_ASSERT_EQUAL(CALL_PIPELINE, callLog[3]);
    TEST_ASSERT_EQUAL(CALL_DISP_PIPE_PERF, callLog[4]);
    TEST_ASSERT_EQUAL(CALL_REFRESH, callLog[5]);
}

void test_process_skips_pipeline_when_parsed_result_disables_pipeline() {
    module.begin(makeDefaultProviders());

    displayNowMs = 5000;
    parsedSignal = ParsedFrameSignal{true, 4900};
    parsedResult = DisplayOrchestrationParsedResult{false};
    refreshResult.signalPriorityActive = true;

    LoopDisplayContext ctx;
    ctx.nowMs = 5000;
    ctx.overloadLateThisLoop = true;
    module.process(ctx);

    TEST_ASSERT_EQUAL(1, parsedCalls);
    TEST_ASSERT_EQUAL(1, refreshCalls);
    TEST_ASSERT_EQUAL(0, providerPipelineCalls);
    TEST_ASSERT_EQUAL(0u, dispPipeElapsedUs);
    TEST_ASSERT_EQUAL(0u, notifyElapsedMs);
    TEST_ASSERT_FALSE(lastRefreshCtx.pipelineRanThisLoop);
}

void test_notify_to_display_skips_for_zero_or_future_timestamp() {
    module.begin(makeDefaultProviders());

    displayNowMs = 3000;
    parsedSignal = ParsedFrameSignal{true, 0};
    parsedResult = DisplayOrchestrationParsedResult{true};
    module.process(LoopDisplayContext{});
    TEST_ASSERT_EQUAL(0u, notifyElapsedMs);

    resetState();
    module.begin(makeDefaultProviders());
    displayNowMs = 3000;
    parsedSignal = ParsedFrameSignal{true, 3200};
    parsedResult = DisplayOrchestrationParsedResult{true};
    module.process(LoopDisplayContext{});
    TEST_ASSERT_EQUAL(0u, notifyElapsedMs);
}

void test_wrap_safe_perf_elapsed_for_pipeline() {
    module.begin(makeDefaultProviders());

    displayNowMs = 1000;
    parsedSignal = ParsedFrameSignal{true, 900};
    parsedResult = DisplayOrchestrationParsedResult{true};
    setPerfTsSequence({0xFFFFFF00u, 0x00000020u});

    module.process(LoopDisplayContext{});

    TEST_ASSERT_EQUAL(0x120u, dispPipeElapsedUs);
}

void test_empty_providers_is_safe_noop() {
    LoopDisplayModule::Providers providers;
    module.begin(providers);

    module.process(LoopDisplayContext{});

    TEST_ASSERT_EQUAL(0, collectCalls);
    TEST_ASSERT_EQUAL(0, parsedCalls);
    TEST_ASSERT_EQUAL(0, refreshCalls);
    TEST_ASSERT_EQUAL(0, providerPipelineCalls);
}

void test_alp_event_alone_runs_pipeline() {
    module.begin(makeDefaultProviders());

    displayNowMs = 2000;
    // ALP event alone (parsedReady with no BLE frame timestamp)
    parsedSignal = ParsedFrameSignal{true, 0};
    parsedResult = DisplayOrchestrationParsedResult{true};
    refreshResult.signalPriorityActive = false;
    setPerfTsSequence({500, 550});

    LoopDisplayContext ctx;
    ctx.nowMs = 2000;
    ctx.bootSplashHoldActive = false;
    ctx.overloadLateThisLoop = false;

    module.process(ctx);

    // Should have called the pipeline even though parsedTsMs=0
    TEST_ASSERT_EQUAL(1, collectCalls);
    TEST_ASSERT_EQUAL(1, parsedCalls);
    TEST_ASSERT_EQUAL(1, refreshCalls);
    TEST_ASSERT_EQUAL(1, providerPipelineCalls);
    TEST_ASSERT_EQUAL(2000u, lastPipelineNowMs);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_process_full_pipeline_with_provider_pipeline_and_perf_records);
    RUN_TEST(test_process_skips_pipeline_when_parsed_result_disables_pipeline);
    RUN_TEST(test_notify_to_display_skips_for_zero_or_future_timestamp);
    RUN_TEST(test_wrap_safe_perf_elapsed_for_pipeline);
    RUN_TEST(test_empty_providers_is_safe_noop);
    RUN_TEST(test_alp_event_alone_runs_pipeline);
    return UNITY_END();
}
