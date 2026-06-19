#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/system/loop_pre_ingest_module.cpp"

static LoopPreIngestModule module;

enum CallId {
    CALL_OPEN_BOOT_READY = 1,
    CALL_WIFI_PRIORITY = 2,
};

static int callLog[24];
static size_t callLogCount = 0;

static uint32_t openedAtMs = 0;
static uint32_t wifiNowMs = 0;

static int providerOpenCalls = 0;
static int providerWifiCalls = 0;

static void noteCall(int id) {
    if (callLogCount < (sizeof(callLog) / sizeof(callLog[0]))) {
        callLog[callLogCount++] = id;
    }
}

static void resetState() {
    callLogCount = 0;

    openedAtMs = 0;
    wifiNowMs = 0;

    providerOpenCalls = 0;
    providerWifiCalls = 0;
}

static void providerOpenBootReady(void*, uint32_t nowMs) {
    providerOpenCalls++;
    openedAtMs = nowMs;
    noteCall(CALL_OPEN_BOOT_READY);
}

static void providerWifiPriority(void*, uint32_t nowMs) {
    providerWifiCalls++;
    wifiNowMs = nowMs;
    noteCall(CALL_WIFI_PRIORITY);
}

void setUp() {
    resetState();
}

void tearDown() {}

void test_timeout_opens_boot_ready_then_runs_wifi() {
    LoopPreIngestModule::Providers providers;
    providers.openBootReadyGate = providerOpenBootReady;
    providers.runWifiPriorityApply = providerWifiPriority;
    module.begin(providers);

    LoopPreIngestContext ctx;
    ctx.nowMs = 5000;
    ctx.bootReady = false;
    ctx.bootReadyDeadlineMs = 4000;

    const LoopPreIngestResult result = module.process(ctx);

    TEST_ASSERT_TRUE(result.bootReady);
    TEST_ASSERT_TRUE(result.bootReadyOpenedByTimeout);
    TEST_ASSERT_TRUE(result.runBleProcessThisLoop);
    TEST_ASSERT_EQUAL(1, providerOpenCalls);
    TEST_ASSERT_EQUAL(1, providerWifiCalls);
    TEST_ASSERT_EQUAL(5000u, openedAtMs);
    TEST_ASSERT_EQUAL(5000u, wifiNowMs);

    TEST_ASSERT_EQUAL(2, callLogCount);
    TEST_ASSERT_EQUAL(CALL_OPEN_BOOT_READY, callLog[0]);
    TEST_ASSERT_EQUAL(CALL_WIFI_PRIORITY, callLog[1]);
}

void test_before_deadline_skips_boot_open_but_runs_wifi() {
    LoopPreIngestModule::Providers providers;
    providers.runWifiPriorityApply = providerWifiPriority;
    module.begin(providers);

    LoopPreIngestContext ctx;
    ctx.nowMs = 2999;
    ctx.bootReady = false;
    ctx.bootReadyDeadlineMs = 3000;

    const LoopPreIngestResult result = module.process(ctx);

    TEST_ASSERT_FALSE(result.bootReady);
    TEST_ASSERT_FALSE(result.bootReadyOpenedByTimeout);
    TEST_ASSERT_TRUE(result.runBleProcessThisLoop);
    TEST_ASSERT_EQUAL(0, providerOpenCalls);
    TEST_ASSERT_EQUAL(1, providerWifiCalls);

    TEST_ASSERT_EQUAL(1, callLogCount);
    TEST_ASSERT_EQUAL(CALL_WIFI_PRIORITY, callLog[0]);
}

void test_empty_handlers_is_safe_and_keeps_expected_flags() {
    LoopPreIngestModule::Providers providers;
    module.begin(providers);

    LoopPreIngestContext ctx;
    ctx.nowMs = 10;
    ctx.bootReady = true;
    ctx.bootReadyDeadlineMs = 20;

    const LoopPreIngestResult result = module.process(ctx);

    TEST_ASSERT_TRUE(result.bootReady);
    TEST_ASSERT_FALSE(result.bootReadyOpenedByTimeout);
    TEST_ASSERT_TRUE(result.runBleProcessThisLoop);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_timeout_opens_boot_ready_then_runs_wifi);
    RUN_TEST(test_before_deadline_skips_boot_open_but_runs_wifi);
    RUN_TEST(test_empty_handlers_is_safe_and_keeps_expected_flags);
    return UNITY_END();
}
