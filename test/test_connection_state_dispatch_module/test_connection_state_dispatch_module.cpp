#include <unity.h>
#include <initializer_list>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/ble/connection_state_dispatch_module.cpp"

static ConnectionStateDispatchModule module;

static ConnectionStateCadenceDecision cadenceScript[8];
static size_t cadenceScriptCount = 0;
static size_t cadenceScriptIndex = 0;
static int cadenceCalls = 0;

static ConnectionStateCadenceContext lastCadenceCtx;
static bool haveLastCadenceCtx = false;

static uint32_t processCallNowMs[8];
static size_t processCallCount = 0;

static void resetState() {
    cadenceScriptCount = 0;
    cadenceScriptIndex = 0;
    cadenceCalls = 0;
    haveLastCadenceCtx = false;
    processCallCount = 0;
}

static void setCadenceScript(std::initializer_list<bool> shouldRunValues) {
    cadenceScriptCount = shouldRunValues.size();
    cadenceScriptIndex = 0;
    size_t i = 0;
    for (bool shouldRun : shouldRunValues) {
        ConnectionStateCadenceDecision decision;
        decision.shouldRunConnectionStateProcess = shouldRun;
        decision.displayUpdateDue = shouldRun;
        cadenceScript[i++] = decision;
    }
}

static ConnectionStateCadenceDecision runCadence(void*, const ConnectionStateCadenceContext& ctx) {
    cadenceCalls++;
    lastCadenceCtx = ctx;
    haveLastCadenceCtx = true;
    if (cadenceScriptIndex < cadenceScriptCount) {
        return cadenceScript[cadenceScriptIndex++];
    }
    ConnectionStateCadenceDecision emptyDecision;
    return emptyDecision;
}

static void runConnectionStateProcess(void*, uint32_t nowMs) {
    if (processCallCount < (sizeof(processCallNowMs) / sizeof(processCallNowMs[0]))) {
        processCallNowMs[processCallCount++] = nowMs;
    }
}

static ConnectionStateDispatchContext makeContext(uint32_t nowMs,
                                                  bool bleConnectedNow,
                                                  bool bootSplashHoldActive,
                                                  bool displayPreviewRunning,
                                                  uint32_t maxProcessGapMs = 1000) {
    ConnectionStateDispatchContext ctx;
    ctx.nowMs = nowMs;
    ctx.displayUpdateIntervalMs = 50;
    ctx.scanScreenDwellMs = 400;
    ctx.bleConnectedNow = bleConnectedNow;
    ctx.bootSplashHoldActive = bootSplashHoldActive;
    ctx.displayPreviewRunning = displayPreviewRunning;
    ctx.maxProcessGapMs = maxProcessGapMs;
    return ctx;
}

void setUp() {
    resetState();
    ConnectionStateDispatchModule::Providers providers;
    providers.runCadence = runCadence;
    providers.runConnectionStateProcess = runConnectionStateProcess;
    module.begin(providers);
}

void tearDown() {}

void test_runs_process_when_cadence_allows_and_forwards_context() {
    setCadenceScript({true});

    const auto decision = module.process(makeContext(120, true, false, false));

    TEST_ASSERT_EQUAL(1, cadenceCalls);
    TEST_ASSERT_TRUE(haveLastCadenceCtx);
    TEST_ASSERT_EQUAL(120u, lastCadenceCtx.nowMs);
    TEST_ASSERT_EQUAL(50u, lastCadenceCtx.displayUpdateIntervalMs);
    TEST_ASSERT_EQUAL(400u, lastCadenceCtx.scanScreenDwellMs);
    TEST_ASSERT_TRUE(lastCadenceCtx.bleConnectedNow);
    TEST_ASSERT_FALSE(lastCadenceCtx.bootSplashHoldActive);
    TEST_ASSERT_FALSE(lastCadenceCtx.displayPreviewRunning);

    TEST_ASSERT_EQUAL(1, processCallCount);
    TEST_ASSERT_EQUAL(120u, processCallNowMs[0]);
    TEST_ASSERT_TRUE(decision.ranConnectionStateProcess);
    TEST_ASSERT_FALSE(decision.watchdogForced);
}

void test_watchdog_forces_process_when_cadence_keeps_blocking() {
    setCadenceScript({true, false, false});

    module.process(makeContext(100, false, false, false, 200));
    const auto notYetForced = module.process(makeContext(250, false, false, false, 200));
    const auto forced = module.process(makeContext(320, false, false, false, 200));

    TEST_ASSERT_FALSE(notYetForced.ranConnectionStateProcess);
    TEST_ASSERT_FALSE(notYetForced.watchdogForced);
    TEST_ASSERT_FALSE(forced.cadence.shouldRunConnectionStateProcess);
    TEST_ASSERT_TRUE(forced.ranConnectionStateProcess);
    TEST_ASSERT_TRUE(forced.watchdogForced);
    TEST_ASSERT_EQUAL(2, processCallCount);
    TEST_ASSERT_EQUAL(100u, processCallNowMs[0]);
    TEST_ASSERT_EQUAL(320u, processCallNowMs[1]);
}

void test_watchdog_is_disabled_while_boot_hold_or_preview_is_active() {
    setCadenceScript({true, false, false});

    module.process(makeContext(100, false, false, false, 200));
    const auto bootHoldBlocked = module.process(makeContext(400, false, true, false, 200));
    const auto previewBlocked = module.process(makeContext(700, false, false, true, 200));

    TEST_ASSERT_FALSE(bootHoldBlocked.ranConnectionStateProcess);
    TEST_ASSERT_FALSE(bootHoldBlocked.watchdogForced);
    TEST_ASSERT_FALSE(previewBlocked.ranConnectionStateProcess);
    TEST_ASSERT_FALSE(previewBlocked.watchdogForced);
    TEST_ASSERT_EQUAL(1, processCallCount);
}

void test_watchdog_elapsed_is_wrap_safe() {
    setCadenceScript({true, false});

    module.process(makeContext(0xFFFFFFF0u, false, false, false, 0x20u));
    const auto forced = module.process(makeContext(0x00000020u, false, false, false, 0x20u));

    TEST_ASSERT_TRUE(forced.ranConnectionStateProcess);
    TEST_ASSERT_TRUE(forced.watchdogForced);
    TEST_ASSERT_EQUAL(2, processCallCount);
    TEST_ASSERT_EQUAL(0xFFFFFFF0u, processCallNowMs[0]);
    TEST_ASSERT_EQUAL(0x00000020u, processCallNowMs[1]);
}

void test_missing_providers_is_safe_noop() {
    ConnectionStateDispatchModule::Providers providers;
    module.begin(providers);

    const auto decision = module.process(makeContext(900, true, false, false));

    TEST_ASSERT_FALSE(decision.ranConnectionStateProcess);
    TEST_ASSERT_FALSE(decision.watchdogForced);
    TEST_ASSERT_FALSE(decision.cadence.displayUpdateDue);
    TEST_ASSERT_FALSE(decision.cadence.shouldRunConnectionStateProcess);
    TEST_ASSERT_EQUAL(0, processCallCount);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_runs_process_when_cadence_allows_and_forwards_context);
    RUN_TEST(test_watchdog_forces_process_when_cadence_keeps_blocking);
    RUN_TEST(test_watchdog_is_disabled_while_boot_hold_or_preview_is_active);
    RUN_TEST(test_watchdog_elapsed_is_wrap_safe);
    RUN_TEST(test_missing_providers_is_safe_noop);
    return UNITY_END();
}
