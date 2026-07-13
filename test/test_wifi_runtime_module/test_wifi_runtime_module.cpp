#include <unity.h>
#include <initializer_list>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/wifi/wifi_runtime_module.cpp"

static WifiRuntimeModule module;

enum CallId {
    CALL_AUTO_START = 1,
    CALL_SET_TRANSITION_ADMISSION = 2,
    CALL_POLICY = 3,
    CALL_CADENCE = 4,
    CALL_WIFI_PROCESS = 5,
    CALL_PERF_RECORD = 6,
    CALL_VISUAL_SYNC = 7,
};

static int callLog[24];
static size_t callLogCount = 0;

static bool autoStartSetDone = false;
static bool autoStartSetDoneValue = false;
static bool autoStartClearManualIntent = false;
static int autoStartCalls = 0;
static uint32_t lastAutoStartNowMs = 0;
static uint32_t lastAutoStartV1ConnectedAtMs = 0;
static bool lastAutoStartEnableWifi = false;
static bool lastAutoStartBleConnected = false;
static bool lastAutoStartCanStartDma = false;
static bool lastAutoStartAllowed = false;
static bool lastAutoStartManualIntentLatched = false;

static bool scriptedPolicyResult = false;
static int policyCalls = 0;

static uint32_t timestampSequence[8];
static size_t timestampSequenceCount = 0;
static size_t timestampSequenceIndex = 0;

static bool scriptedCadenceShouldRun = false;
static int cadenceCalls = 0;
static WifiProcessCadenceContext lastCadenceCtx;
static bool haveLastCadenceCtx = false;
static int transitionAdmissionCalls = 0;
static bool lastAllowTransitionWork = false;

static int wifiProcessCalls = 0;
static bool scriptedLifecyclePending = false;

static int perfRecordCalls = 0;
static uint32_t lastPerfElapsedUs = 0;

static bool scriptedWifiServiceActive = false;
static bool scriptedWifiConnected = false;
static int visualSyncCalls = 0;
static uint32_t lastVisualNowMs = 0;
static bool lastVisualActive = false;
static bool lastVisualPreviewRunning = false;
static bool lastVisualBootSplashHold = false;
static uint32_t scriptedVisualNowMs = 0;
static int readVisualNowCalls = 0;

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

static void runWifiAutoStartProcess(void*,
                                    uint32_t nowMs,
                                    uint32_t v1ConnectedAtMs,
                                    bool enableWifi,
                                    bool bleConnected,
                                    bool canStartDma,
                                    bool wifiAutoStartAllowed,
                                    bool& wifiManualStartIntentLatched,
                                    bool& wifiAutoStartDone) {
    noteCall(CALL_AUTO_START);
    autoStartCalls++;
    lastAutoStartNowMs = nowMs;
    lastAutoStartV1ConnectedAtMs = v1ConnectedAtMs;
    lastAutoStartEnableWifi = enableWifi;
    lastAutoStartBleConnected = bleConnected;
    lastAutoStartCanStartDma = canStartDma;
    lastAutoStartAllowed = wifiAutoStartAllowed;
    lastAutoStartManualIntentLatched = wifiManualStartIntentLatched;
    if (autoStartClearManualIntent) {
        wifiManualStartIntentLatched = false;
    }
    if (autoStartSetDone) {
        wifiAutoStartDone = autoStartSetDoneValue;
    }
}

static bool shouldRunWifiProcessingPolicy(void*) {
    noteCall(CALL_POLICY);
    policyCalls++;
    return scriptedPolicyResult;
}

static WifiProcessCadenceDecision runWifiCadence(void*, const WifiProcessCadenceContext& cadenceCtx) {
    noteCall(CALL_CADENCE);
    cadenceCalls++;
    lastCadenceCtx = cadenceCtx;
    haveLastCadenceCtx = true;
    WifiProcessCadenceDecision decision;
    decision.shouldRunProcess = scriptedCadenceShouldRun;
    return decision;
}

static void setWifiTransitionAdmission(void*, bool allowTransitionWork) {
    noteCall(CALL_SET_TRANSITION_ADMISSION);
    transitionAdmissionCalls++;
    lastAllowTransitionWork = allowTransitionWork;
}

static void runWifiManagerProcess(void*) {
    noteCall(CALL_WIFI_PROCESS);
    wifiProcessCalls++;
}

static bool readWifiLifecyclePending(void*) {
    return scriptedLifecyclePending;
}

static void recordWifiProcessUs(void*, uint32_t elapsedUs) {
    noteCall(CALL_PERF_RECORD);
    perfRecordCalls++;
    lastPerfElapsedUs = elapsedUs;
}

static bool readWifiServiceActive(void*) {
    return scriptedWifiServiceActive;
}

static bool readWifiConnected(void*) {
    return scriptedWifiConnected;
}

static uint32_t readVisualNowMs(void*) {
    readVisualNowCalls++;
    return scriptedVisualNowMs;
}

static void runWifiVisualSync(void*,
                              uint32_t nowMs,
                              bool wifiVisualActiveNow,
                              bool displayPreviewRunning,
                              bool bootSplashHoldActive) {
    noteCall(CALL_VISUAL_SYNC);
    visualSyncCalls++;
    lastVisualNowMs = nowMs;
    lastVisualActive = wifiVisualActiveNow;
    lastVisualPreviewRunning = displayPreviewRunning;
    lastVisualBootSplashHold = bootSplashHoldActive;
}

static WifiRuntimeModule::Providers makeDefaultProviders() {
    WifiRuntimeModule::Providers providers;
    providers.runWifiAutoStartProcess = runWifiAutoStartProcess;
    providers.shouldRunWifiProcessingPolicy = shouldRunWifiProcessingPolicy;
    providers.perfTimestampUs = nextTimestampUs;
    providers.runWifiCadence = runWifiCadence;
    providers.setWifiTransitionAdmission = setWifiTransitionAdmission;
    providers.runWifiManagerProcess = runWifiManagerProcess;
    providers.readWifiLifecyclePending = readWifiLifecyclePending;
    providers.recordWifiProcessUs = recordWifiProcessUs;
    providers.readWifiServiceActive = readWifiServiceActive;
    providers.readWifiConnected = readWifiConnected;
    providers.readVisualNowMs = readVisualNowMs;
    providers.runWifiVisualSync = runWifiVisualSync;
    return providers;
}

static void resetState() {
    callLogCount = 0;
    autoStartSetDone = false;
    autoStartSetDoneValue = false;
    autoStartClearManualIntent = false;
    autoStartCalls = 0;
    lastAutoStartNowMs = 0;
    lastAutoStartV1ConnectedAtMs = 0;
    lastAutoStartEnableWifi = false;
    lastAutoStartBleConnected = false;
    lastAutoStartCanStartDma = false;
    lastAutoStartAllowed = false;
    lastAutoStartManualIntentLatched = false;
    scriptedPolicyResult = false;
    policyCalls = 0;
    timestampSequenceCount = 0;
    timestampSequenceIndex = 0;
    scriptedCadenceShouldRun = false;
    cadenceCalls = 0;
    haveLastCadenceCtx = false;
    transitionAdmissionCalls = 0;
    lastAllowTransitionWork = false;
    wifiProcessCalls = 0;
    scriptedLifecyclePending = false;
    perfRecordCalls = 0;
    lastPerfElapsedUs = 0;
    scriptedWifiServiceActive = false;
    scriptedWifiConnected = false;
    visualSyncCalls = 0;
    lastVisualNowMs = 0;
    lastVisualActive = false;
    lastVisualPreviewRunning = false;
    lastVisualBootSplashHold = false;
    scriptedVisualNowMs = 0;
    readVisualNowCalls = 0;
}

void setUp() {
    resetState();
    module.begin(makeDefaultProviders());
}

void tearDown() {}

void test_process_runs_wifi_path_with_updated_autostart_state_and_perf_recording() {
    autoStartSetDone = true;
    autoStartSetDoneValue = true;
    autoStartClearManualIntent = true;
    scriptedPolicyResult = true;
    scriptedCadenceShouldRun = true;
    scriptedWifiServiceActive = false;
    scriptedWifiConnected = true;
    scriptedVisualNowMs = 777;
    setTimestampSequence({1000, 1200, 1300});

    WifiRuntimeContext ctx;
    ctx.nowMs = 5000;
    ctx.v1ConnectedAtMs = 4500;
    ctx.enableWifi = true;
    ctx.bleConnected = true;
    ctx.canStartDma = true;
    ctx.wifiAutoStartAllowed = true;
    ctx.wifiAutoStartDone = false;
    ctx.wifiManualStartIntentLatched = true;
    ctx.skipLateNonCoreThisLoop = false;
    ctx.displayPreviewRunning = true;
    ctx.bootSplashHoldActive = false;

    const WifiRuntimeResult result = module.process(ctx);

    TEST_ASSERT_TRUE(result.wifiAutoStartDone);
    TEST_ASSERT_FALSE(result.wifiManualStartIntentLatched);
    TEST_ASSERT_EQUAL(1, autoStartCalls);
    TEST_ASSERT_EQUAL(5000u, lastAutoStartNowMs);
    TEST_ASSERT_EQUAL(4500u, lastAutoStartV1ConnectedAtMs);
    TEST_ASSERT_TRUE(lastAutoStartEnableWifi);
    TEST_ASSERT_TRUE(lastAutoStartBleConnected);
    TEST_ASSERT_TRUE(lastAutoStartCanStartDma);
    TEST_ASSERT_TRUE(lastAutoStartAllowed);
    TEST_ASSERT_TRUE(lastAutoStartManualIntentLatched);

    TEST_ASSERT_EQUAL(1, transitionAdmissionCalls);
    TEST_ASSERT_TRUE(lastAllowTransitionWork);
    TEST_ASSERT_EQUAL(1, policyCalls);

    TEST_ASSERT_EQUAL(1, cadenceCalls);
    TEST_ASSERT_TRUE(haveLastCadenceCtx);
    TEST_ASSERT_EQUAL(1000u, lastCadenceCtx.nowProcessUs);
    TEST_ASSERT_EQUAL(2000u, lastCadenceCtx.minIntervalUs);

    TEST_ASSERT_EQUAL(1, wifiProcessCalls);
    TEST_ASSERT_EQUAL(1, perfRecordCalls);
    TEST_ASSERT_EQUAL(100u, lastPerfElapsedUs);

    TEST_ASSERT_EQUAL(1, visualSyncCalls);
    TEST_ASSERT_EQUAL(777u, lastVisualNowMs);
    TEST_ASSERT_TRUE(lastVisualActive);
    TEST_ASSERT_TRUE(lastVisualPreviewRunning);
    TEST_ASSERT_FALSE(lastVisualBootSplashHold);

    TEST_ASSERT_EQUAL(7, callLogCount);
    TEST_ASSERT_EQUAL(CALL_AUTO_START, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_SET_TRANSITION_ADMISSION, callLog[1]);
    TEST_ASSERT_EQUAL(CALL_POLICY, callLog[2]);
    TEST_ASSERT_EQUAL(CALL_CADENCE, callLog[3]);
    TEST_ASSERT_EQUAL(CALL_WIFI_PROCESS, callLog[4]);
    TEST_ASSERT_EQUAL(CALL_PERF_RECORD, callLog[5]);
    TEST_ASSERT_EQUAL(CALL_VISUAL_SYNC, callLog[6]);
}

void test_skip_non_core_blocks_policy_wifi_process_and_visual_sync() {
    scriptedPolicyResult = true;
    scriptedCadenceShouldRun = true;
    scriptedWifiServiceActive = true;
    scriptedVisualNowMs = 123;

    WifiRuntimeContext ctx;
    ctx.nowMs = 900;
    ctx.enableWifi = true;
    ctx.wifiAutoStartAllowed = false;
    ctx.wifiAutoStartDone = true;
    ctx.wifiManualStartIntentLatched = true;
    ctx.skipLateNonCoreThisLoop = true;

    const WifiRuntimeResult result = module.process(ctx);

    TEST_ASSERT_TRUE(result.wifiAutoStartDone);
    TEST_ASSERT_TRUE(result.wifiManualStartIntentLatched);
    TEST_ASSERT_EQUAL(0, autoStartCalls);
    TEST_ASSERT_EQUAL(1, transitionAdmissionCalls);
    TEST_ASSERT_FALSE(lastAllowTransitionWork);
    TEST_ASSERT_EQUAL(0, policyCalls);
    TEST_ASSERT_EQUAL(0, cadenceCalls);
    TEST_ASSERT_EQUAL(0, wifiProcessCalls);
    TEST_ASSERT_EQUAL(0, perfRecordCalls);
    TEST_ASSERT_EQUAL(0, visualSyncCalls);
    TEST_ASSERT_EQUAL(0, readVisualNowCalls);
}

void test_policy_false_skips_cadence_and_wifi_process() {
    scriptedPolicyResult = false;
    scriptedWifiServiceActive = false;
    scriptedWifiConnected = false;
    scriptedVisualNowMs = 321;

    WifiRuntimeContext ctx;
    ctx.nowMs = 1400;
    ctx.enableWifi = true;
    ctx.wifiAutoStartAllowed = true;
    ctx.wifiAutoStartDone = true;

    module.process(ctx);

    TEST_ASSERT_EQUAL(1, transitionAdmissionCalls);
    TEST_ASSERT_TRUE(lastAllowTransitionWork);
    TEST_ASSERT_EQUAL(1, policyCalls);
    TEST_ASSERT_EQUAL(0, cadenceCalls);
    TEST_ASSERT_EQUAL(0, wifiProcessCalls);
    TEST_ASSERT_EQUAL(0, perfRecordCalls);
    TEST_ASSERT_EQUAL(1, visualSyncCalls);
    TEST_ASSERT_FALSE(lastVisualActive);
}

void test_cadence_false_skips_wifi_process_and_perf_record() {
    scriptedPolicyResult = true;
    scriptedCadenceShouldRun = false;
    setTimestampSequence({55});

    WifiRuntimeContext ctx;
    ctx.nowMs = 200;
    ctx.enableWifi = true;
    ctx.wifiAutoStartAllowed = true;
    ctx.wifiAutoStartDone = true;

    module.process(ctx);

    TEST_ASSERT_EQUAL(1, transitionAdmissionCalls);
    TEST_ASSERT_TRUE(lastAllowTransitionWork);
    TEST_ASSERT_EQUAL(1, policyCalls);
    TEST_ASSERT_EQUAL(1, cadenceCalls);
    TEST_ASSERT_EQUAL(0, wifiProcessCalls);
    TEST_ASSERT_EQUAL(0, perfRecordCalls);
}

void test_visual_uses_ctx_now_when_visual_now_provider_missing() {
    WifiRuntimeModule::Providers providers = makeDefaultProviders();
    providers.readVisualNowMs = nullptr;
    module.begin(providers);

    scriptedWifiServiceActive = true;

    WifiRuntimeContext ctx;
    ctx.nowMs = 6060;
    ctx.wifiAutoStartAllowed = true;
    ctx.wifiAutoStartDone = true;
    module.process(ctx);

    TEST_ASSERT_EQUAL(0, readVisualNowCalls);
    TEST_ASSERT_EQUAL(1, visualSyncCalls);
    TEST_ASSERT_EQUAL(6060u, lastVisualNowMs);
}

void test_transition_gate_closes_for_connect_burst_pressure_signals() {
    scriptedPolicyResult = true;
    scriptedCadenceShouldRun = true;

    WifiRuntimeContext ctx;
    ctx.nowMs = 1234;
    ctx.enableWifi = true;
    ctx.wifiAutoStartAllowed = true;
    ctx.wifiAutoStartDone = true;
    ctx.bleBackpressure = true;
    ctx.overloadLateThisLoop = true;
    ctx.bleConnectBurstSettling = true;

    module.process(ctx);

    TEST_ASSERT_EQUAL(1, transitionAdmissionCalls);
    TEST_ASSERT_FALSE(lastAllowTransitionWork);
    TEST_ASSERT_EQUAL(0, autoStartCalls);
    TEST_ASSERT_EQUAL(0, policyCalls);
    TEST_ASSERT_EQUAL(0, cadenceCalls);
    TEST_ASSERT_EQUAL(0, wifiProcessCalls);
    TEST_ASSERT_EQUAL(0, visualSyncCalls);
    TEST_ASSERT_EQUAL(0, readVisualNowCalls);
}

void test_pending_lifecycle_work_still_runs_wifi_process_under_pressure() {
    scriptedPolicyResult = true;
    scriptedCadenceShouldRun = true;
    scriptedLifecyclePending = true;
    setTimestampSequence({100, 125, 150});

    WifiRuntimeContext ctx;
    ctx.nowMs = 1234;
    ctx.enableWifi = true;
    ctx.wifiAutoStartAllowed = true;
    ctx.wifiAutoStartDone = true;
    ctx.bleBackpressure = true;

    module.process(ctx);

    TEST_ASSERT_EQUAL(1, transitionAdmissionCalls);
    TEST_ASSERT_FALSE(lastAllowTransitionWork);
    TEST_ASSERT_EQUAL(0, autoStartCalls);
    TEST_ASSERT_EQUAL(1, policyCalls);
    TEST_ASSERT_EQUAL(1, cadenceCalls);
    TEST_ASSERT_EQUAL(1, wifiProcessCalls);
    TEST_ASSERT_EQUAL(1, perfRecordCalls);
    TEST_ASSERT_EQUAL(25u, lastPerfElapsedUs);
    TEST_ASSERT_EQUAL(0, visualSyncCalls);
}

void test_visual_sync_defers_for_each_ble_pressure_signal() {
    scriptedWifiServiceActive = true;
    scriptedWifiConnected = true;
    scriptedVisualNowMs = 456;

    WifiRuntimeContext ctx;
    ctx.nowMs = 456;
    ctx.enableWifi = true;
    ctx.wifiAutoStartAllowed = true;
    ctx.wifiAutoStartDone = true;

    ctx.bleBackpressure = true;
    module.process(ctx);
    TEST_ASSERT_EQUAL(0, visualSyncCalls);
    TEST_ASSERT_EQUAL(0, readVisualNowCalls);
    TEST_ASSERT_FALSE(lastAllowTransitionWork);

    resetState();
    module.begin(makeDefaultProviders());
    scriptedWifiServiceActive = true;
    scriptedWifiConnected = true;
    scriptedVisualNowMs = 456;
    ctx = WifiRuntimeContext{};
    ctx.nowMs = 456;
    ctx.enableWifi = true;
    ctx.wifiAutoStartAllowed = true;
    ctx.wifiAutoStartDone = true;
    ctx.overloadLateThisLoop = true;
    module.process(ctx);
    TEST_ASSERT_EQUAL(0, visualSyncCalls);
    TEST_ASSERT_EQUAL(0, readVisualNowCalls);
    TEST_ASSERT_FALSE(lastAllowTransitionWork);

    resetState();
    module.begin(makeDefaultProviders());
    scriptedWifiServiceActive = true;
    scriptedWifiConnected = true;
    scriptedVisualNowMs = 456;
    ctx = WifiRuntimeContext{};
    ctx.nowMs = 456;
    ctx.enableWifi = true;
    ctx.wifiAutoStartAllowed = true;
    ctx.wifiAutoStartDone = true;
    ctx.bleConnectBurstSettling = true;
    module.process(ctx);
    TEST_ASSERT_EQUAL(0, visualSyncCalls);
    TEST_ASSERT_EQUAL(0, readVisualNowCalls);
    TEST_ASSERT_FALSE(lastAllowTransitionWork);
}

void test_empty_providers_is_safe_noop() {
    WifiRuntimeModule::Providers providers;
    module.begin(providers);

    WifiRuntimeContext ctx;
    ctx.nowMs = 777;
    ctx.wifiAutoStartDone = true;
    const WifiRuntimeResult result = module.process(ctx);

    TEST_ASSERT_TRUE(result.wifiAutoStartDone);
    TEST_ASSERT_FALSE(result.wifiManualStartIntentLatched);
    TEST_ASSERT_EQUAL(0, autoStartCalls);
    TEST_ASSERT_EQUAL(0, policyCalls);
    TEST_ASSERT_EQUAL(0, cadenceCalls);
    TEST_ASSERT_EQUAL(0, wifiProcessCalls);
    TEST_ASSERT_EQUAL(0, perfRecordCalls);
    TEST_ASSERT_EQUAL(0, visualSyncCalls);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_process_runs_wifi_path_with_updated_autostart_state_and_perf_recording);
    RUN_TEST(test_skip_non_core_blocks_policy_wifi_process_and_visual_sync);
    RUN_TEST(test_policy_false_skips_cadence_and_wifi_process);
    RUN_TEST(test_cadence_false_skips_wifi_process_and_perf_record);
    RUN_TEST(test_visual_uses_ctx_now_when_visual_now_provider_missing);
    RUN_TEST(test_transition_gate_closes_for_connect_burst_pressure_signals);
    RUN_TEST(test_pending_lifecycle_work_still_runs_wifi_process_under_pressure);
    RUN_TEST(test_visual_sync_defers_for_each_ble_pressure_signal);
    RUN_TEST(test_empty_providers_is_safe_noop);
    return UNITY_END();
}
