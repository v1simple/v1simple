#include <unity.h>
#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_post_display_module.cpp"

static LoopPostDisplayModule module;

enum CallId {
    CALL_AUTO_PUSH = 1,
    CALL_DISPATCH = 5,
};

static int callLog[32];
static size_t callLogCount = 0;

static uint32_t providerDispatchNowMs = 0;
static bool providerBleConnected = false;

static ConnectionStateDispatchContext lastDispatchCtx;

static int providerAutoPushCalls = 0;
static int providerDispatchCalls = 0;

static void noteCall(int id) {
    if (callLogCount < (sizeof(callLog) / sizeof(callLog[0]))) {
        callLog[callLogCount++] = id;
    }
}

static void resetState() {
    callLogCount = 0;
    providerDispatchNowMs = 0;
    providerBleConnected = false;
    lastDispatchCtx = ConnectionStateDispatchContext{};
    providerAutoPushCalls = 0;
    providerDispatchCalls = 0;
}

static void runProviderAutoPush(void*) {
    providerAutoPushCalls++;
    noteCall(CALL_AUTO_PUSH);
}

static uint32_t readDispatchNowMs(void*) {
    return providerDispatchNowMs;
}

static bool readBleConnectedNow(void*) {
    return providerBleConnected;
}

static void runProviderDispatch(void*, const ConnectionStateDispatchContext& dispatchCtx) {
    providerDispatchCalls++;
    lastDispatchCtx = dispatchCtx;
    noteCall(CALL_DISPATCH);
}

static LoopPostDisplayModule::Providers makeDefaultProviders() {
    LoopPostDisplayModule::Providers providers;
    providers.runAutoPush = runProviderAutoPush;
    providers.readDispatchNowMs = readDispatchNowMs;
    providers.readBleConnectedNow = readBleConnectedNow;
    providers.runConnectionStateDispatch = runProviderDispatch;
    return providers;
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_process_provider_path_updates_outputs_and_dispatch_context() {
    module.begin(makeDefaultProviders());

    providerDispatchNowMs = 2222;
    providerBleConnected = true;

    LoopPostDisplayContext ctx;
    ctx.nowMs = 1500;
    ctx.displayUpdateIntervalMs = 60;
    ctx.scanScreenDwellMs = 333;
    ctx.bootSplashHoldActive = false;
    ctx.displayPreviewRunning = true;
    ctx.maxProcessGapMs = 1200;

    const LoopPostDisplayResult result = module.process(ctx);

    TEST_ASSERT_EQUAL(2222u, result.dispatchNowMs);
    TEST_ASSERT_TRUE(result.bleConnectedNow);
    TEST_ASSERT_EQUAL(1, providerAutoPushCalls);
    TEST_ASSERT_EQUAL(1, providerDispatchCalls);
    TEST_ASSERT_EQUAL(2222u, lastDispatchCtx.nowMs);
    TEST_ASSERT_EQUAL(60u, lastDispatchCtx.displayUpdateIntervalMs);
    TEST_ASSERT_EQUAL(333u, lastDispatchCtx.scanScreenDwellMs);
    TEST_ASSERT_TRUE(lastDispatchCtx.bleConnectedNow);
    TEST_ASSERT_FALSE(lastDispatchCtx.bootSplashHoldActive);
    TEST_ASSERT_TRUE(lastDispatchCtx.displayPreviewRunning);
    TEST_ASSERT_EQUAL(1200u, lastDispatchCtx.maxProcessGapMs);

    TEST_ASSERT_EQUAL(2, callLogCount);
    TEST_ASSERT_EQUAL(CALL_AUTO_PUSH, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_DISPATCH, callLog[1]);
}

void test_process_ctx_fallbacks_apply_when_read_providers_are_missing() {
    LoopPostDisplayModule::Providers providers = makeDefaultProviders();
    providers.readDispatchNowMs = nullptr;
    providers.readBleConnectedNow = nullptr;
    module.begin(providers);

    LoopPostDisplayContext ctx;
    ctx.nowMs = 900;
    ctx.displayUpdateIntervalMs = 50;
    ctx.scanScreenDwellMs = 77;
    ctx.bootSplashHoldActive = true;
    ctx.displayPreviewRunning = false;
    ctx.maxProcessGapMs = 1000;
    ctx.bleConnectedNow = true;

    const LoopPostDisplayResult result = module.process(ctx);

    TEST_ASSERT_EQUAL(900u, result.dispatchNowMs);
    TEST_ASSERT_TRUE(result.bleConnectedNow);
    TEST_ASSERT_EQUAL(1, providerAutoPushCalls);
    TEST_ASSERT_EQUAL(1, providerDispatchCalls);
    TEST_ASSERT_EQUAL(900u, lastDispatchCtx.nowMs);
    TEST_ASSERT_TRUE(lastDispatchCtx.bleConnectedNow);
    TEST_ASSERT_TRUE(lastDispatchCtx.bootSplashHoldActive);
    TEST_ASSERT_FALSE(lastDispatchCtx.displayPreviewRunning);
}

void test_empty_providers_returns_ctx_fallbacks() {
    LoopPostDisplayModule::Providers providers;
    module.begin(providers);

    LoopPostDisplayContext ctx;
    ctx.nowMs = 77;
    ctx.bleConnectedNow = true;

    const LoopPostDisplayResult result = module.process(ctx);

    TEST_ASSERT_EQUAL(77u, result.dispatchNowMs);
    TEST_ASSERT_TRUE(result.bleConnectedNow);
}

void test_auto_push_only_skips_speed_and_dispatch() {
    module.begin(makeDefaultProviders());

    LoopPostDisplayContext ctx;
    ctx.enableAutoPush = true;
    ctx.runSpeedAndDispatch = false;
    ctx.nowMs = 4321;
    ctx.bleConnectedNow = true;

    const LoopPostDisplayResult result = module.process(ctx);

    TEST_ASSERT_EQUAL(1, providerAutoPushCalls);
    TEST_ASSERT_EQUAL(0, providerDispatchCalls);
    TEST_ASSERT_EQUAL(4321u, result.dispatchNowMs);
    TEST_ASSERT_TRUE(result.bleConnectedNow);
    TEST_ASSERT_EQUAL(1, callLogCount);
    TEST_ASSERT_EQUAL(CALL_AUTO_PUSH, callLog[0]);
}

void test_speed_dispatch_only_skips_auto_push() {
    module.begin(makeDefaultProviders());

    providerDispatchNowMs = 2468;
    providerBleConnected = false;

    LoopPostDisplayContext ctx;
    ctx.enableAutoPush = false;
    ctx.runSpeedAndDispatch = true;
    ctx.nowMs = 1111;
    ctx.displayUpdateIntervalMs = 66;
    ctx.scanScreenDwellMs = 777;
    ctx.bootSplashHoldActive = false;
    ctx.displayPreviewRunning = true;
    ctx.maxProcessGapMs = 3210;

    const LoopPostDisplayResult result = module.process(ctx);

    TEST_ASSERT_EQUAL(0, providerAutoPushCalls);
    TEST_ASSERT_EQUAL(1, providerDispatchCalls);
    TEST_ASSERT_EQUAL(2468u, result.dispatchNowMs);
    TEST_ASSERT_FALSE(result.bleConnectedNow);
    TEST_ASSERT_EQUAL(2468u, lastDispatchCtx.nowMs);
    TEST_ASSERT_EQUAL(66u, lastDispatchCtx.displayUpdateIntervalMs);
    TEST_ASSERT_EQUAL(777u, lastDispatchCtx.scanScreenDwellMs);
    TEST_ASSERT_FALSE(lastDispatchCtx.bleConnectedNow);
    TEST_ASSERT_FALSE(lastDispatchCtx.bootSplashHoldActive);
    TEST_ASSERT_TRUE(lastDispatchCtx.displayPreviewRunning);
    TEST_ASSERT_EQUAL(3210u, lastDispatchCtx.maxProcessGapMs);
    TEST_ASSERT_EQUAL(1, callLogCount);
    TEST_ASSERT_EQUAL(CALL_DISPATCH, callLog[0]);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_process_provider_path_updates_outputs_and_dispatch_context);
    RUN_TEST(test_process_ctx_fallbacks_apply_when_read_providers_are_missing);
    RUN_TEST(test_empty_providers_returns_ctx_fallbacks);
    RUN_TEST(test_auto_push_only_skips_speed_and_dispatch);
    RUN_TEST(test_speed_dispatch_only_skips_auto_push);
    return UNITY_END();
}
