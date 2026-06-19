#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_connection_early_module.cpp"

static LoopConnectionEarlyModule module;

enum CallId {
    CALL_RUNTIME = 1,
    CALL_SHOW_SCANNING = 2,
    CALL_DISPLAY_EARLY = 3,
};

static int callLog[16];
static size_t callLogCount = 0;

static ConnectionRuntimeSnapshot snapshotValue;

static uint32_t runtimeNowMs = 0;
static uint32_t runtimeNowUs = 0;
static uint32_t runtimeLastLoopUs = 0;
static bool runtimeBootSplashHoldActive = false;
static uint32_t runtimeBootSplashHoldUntilMs = 0;
static bool runtimeInitialScanningShown = false;

static DisplayOrchestrationEarlyContext lastDisplayEarlyCtx;

static int providerRuntimeCalls = 0;
static int providerShowCalls = 0;
static int providerDisplayCalls = 0;

static bool proxyConnectedValue = false;
static int connectionRssiValue = 0;
static int proxyRssiValue = 0;

static void noteCall(int id) {
    if (callLogCount < (sizeof(callLog) / sizeof(callLog[0]))) {
        callLog[callLogCount++] = id;
    }
}

static void resetState() {
    callLogCount = 0;
    snapshotValue = ConnectionRuntimeSnapshot{};
    runtimeNowMs = 0;
    runtimeNowUs = 0;
    runtimeLastLoopUs = 0;
    runtimeBootSplashHoldActive = false;
    runtimeBootSplashHoldUntilMs = 0;
    runtimeInitialScanningShown = false;
    lastDisplayEarlyCtx = DisplayOrchestrationEarlyContext{};
    providerRuntimeCalls = 0;
    providerShowCalls = 0;
    providerDisplayCalls = 0;
    proxyConnectedValue = false;
    connectionRssiValue = 0;
    proxyRssiValue = 0;
}

static ConnectionRuntimeSnapshot runRuntimeProvider(void*,
                                                    uint32_t nowMs,
                                                    uint32_t nowUs,
                                                    uint32_t lastLoopUs,
                                                    bool bootSplashHoldActive,
                                                    uint32_t bootSplashHoldUntilMs,
                                                    bool initialScanningScreenShown) {
    providerRuntimeCalls++;
    runtimeNowMs = nowMs;
    runtimeNowUs = nowUs;
    runtimeLastLoopUs = lastLoopUs;
    runtimeBootSplashHoldActive = bootSplashHoldActive;
    runtimeBootSplashHoldUntilMs = bootSplashHoldUntilMs;
    runtimeInitialScanningShown = initialScanningScreenShown;
    noteCall(CALL_RUNTIME);
    return snapshotValue;
}

static void runShowProvider(void*) {
    providerShowCalls++;
    noteCall(CALL_SHOW_SCANNING);
}

static void runDisplayProvider(void*, const DisplayOrchestrationEarlyContext& displayEarlyCtx) {
    providerDisplayCalls++;
    lastDisplayEarlyCtx = displayEarlyCtx;
    noteCall(CALL_DISPLAY_EARLY);
}

static bool readProxyConnected(void*) {
    return proxyConnectedValue;
}

static int readConnectionRssi(void*) {
    return connectionRssiValue;
}

static int readProxyRssi(void*) {
    return proxyRssiValue;
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_provider_path_forwards_snapshot_and_display_context() {
    LoopConnectionEarlyModule::Providers providers;
    providers.runConnectionRuntime = runRuntimeProvider;
    providers.showInitialScanning = runShowProvider;
    providers.readProxyConnected = readProxyConnected;
    providers.readConnectionRssi = readConnectionRssi;
    providers.readProxyRssi = readProxyRssi;
    providers.runDisplayEarly = runDisplayProvider;
    module.begin(providers);

    snapshotValue.connected = true;
    snapshotValue.receiving = true;
    snapshotValue.backpressured = true;
    snapshotValue.skipNonCore = true;
    snapshotValue.overloaded = true;
    snapshotValue.bootSplashHoldActive = false;
    snapshotValue.initialScanningScreenShown = false;
    snapshotValue.requestShowInitialScanning = true;

    proxyConnectedValue = true;
    connectionRssiValue = -48;
    proxyRssiValue = -66;

    LoopConnectionEarlyContext ctx;
    ctx.nowMs = 1234;
    ctx.nowUs = 4567;
    ctx.lastLoopUs = 89;
    ctx.bootSplashHoldActive = true;
    ctx.bootSplashHoldUntilMs = 9000;
    ctx.initialScanningScreenShown = false;

    const LoopConnectionEarlyResult result = module.process(ctx);

    TEST_ASSERT_EQUAL(1, providerRuntimeCalls);
    TEST_ASSERT_EQUAL(1, providerShowCalls);
    TEST_ASSERT_EQUAL(1, providerDisplayCalls);

    TEST_ASSERT_EQUAL(1234u, runtimeNowMs);
    TEST_ASSERT_EQUAL(4567u, runtimeNowUs);
    TEST_ASSERT_EQUAL(89u, runtimeLastLoopUs);
    TEST_ASSERT_TRUE(runtimeBootSplashHoldActive);
    TEST_ASSERT_EQUAL(9000u, runtimeBootSplashHoldUntilMs);
    TEST_ASSERT_FALSE(runtimeInitialScanningShown);

    TEST_ASSERT_FALSE(result.bootSplashHoldActive);
    TEST_ASSERT_TRUE(result.initialScanningScreenShown);
    TEST_ASSERT_TRUE(result.bleConnectedNow);
    TEST_ASSERT_TRUE(result.bleBackpressure);
    TEST_ASSERT_TRUE(result.skipNonCoreThisLoop);
    TEST_ASSERT_TRUE(result.overloadThisLoop);
    TEST_ASSERT_TRUE(result.bleReceiving);

    TEST_ASSERT_EQUAL(1234u, lastDisplayEarlyCtx.nowMs);
    TEST_ASSERT_FALSE(lastDisplayEarlyCtx.bootSplashHoldActive);
    TEST_ASSERT_TRUE(lastDisplayEarlyCtx.overloadThisLoop);
    TEST_ASSERT_TRUE(lastDisplayEarlyCtx.bleContext.v1Connected);
    TEST_ASSERT_TRUE(lastDisplayEarlyCtx.bleContext.proxyConnected);
    TEST_ASSERT_EQUAL(-48, lastDisplayEarlyCtx.bleContext.v1Rssi);
    TEST_ASSERT_EQUAL(-66, lastDisplayEarlyCtx.bleContext.proxyRssi);
    TEST_ASSERT_TRUE(lastDisplayEarlyCtx.bleReceiving);

    TEST_ASSERT_EQUAL(3, callLogCount);
    TEST_ASSERT_EQUAL(CALL_RUNTIME, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_SHOW_SCANNING, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_DISPLAY_EARLY, callLog[2]);
}

void test_request_show_marks_scanning_shown_even_without_handler() {
    LoopConnectionEarlyModule::Providers providers;
    providers.runConnectionRuntime = runRuntimeProvider;
    module.begin(providers);

    snapshotValue.requestShowInitialScanning = true;
    snapshotValue.initialScanningScreenShown = false;

    const LoopConnectionEarlyResult result = module.process(LoopConnectionEarlyContext{});

    TEST_ASSERT_EQUAL(1, providerRuntimeCalls);
    TEST_ASSERT_EQUAL(0, providerShowCalls);
    TEST_ASSERT_TRUE(result.initialScanningScreenShown);
}

void test_empty_providers_and_context_is_safe() {
    LoopConnectionEarlyModule::Providers providers;
    module.begin(providers);

    const LoopConnectionEarlyResult result = module.process(LoopConnectionEarlyContext{});

    TEST_ASSERT_FALSE(result.bootSplashHoldActive);
    TEST_ASSERT_FALSE(result.initialScanningScreenShown);
    TEST_ASSERT_FALSE(result.bleConnectedNow);
    TEST_ASSERT_FALSE(result.bleBackpressure);
    TEST_ASSERT_FALSE(result.skipNonCoreThisLoop);
    TEST_ASSERT_FALSE(result.overloadThisLoop);
    TEST_ASSERT_FALSE(result.bleReceiving);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_provider_path_forwards_snapshot_and_display_context);
    RUN_TEST(test_request_show_marks_scanning_shown_even_without_handler);
    RUN_TEST(test_empty_providers_and_context_is_safe);
    return UNITY_END();
}
