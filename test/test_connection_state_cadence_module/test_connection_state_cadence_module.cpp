#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/ble/connection_state_cadence_module.cpp"

static ConnectionStateCadenceModule module;

static ConnectionStateCadenceContext makeContext(unsigned long nowMs,
                                                 bool bleConnectedNow,
                                                 bool bootSplashHoldActive,
                                                 bool displayPreviewRunning,
                                                 unsigned long displayUpdateIntervalMs = 50,
                                                 unsigned long scanScreenDwellMs = 400) {
    ConnectionStateCadenceContext ctx;
    ctx.nowMs = nowMs;
    ctx.displayUpdateIntervalMs = displayUpdateIntervalMs;
    ctx.scanScreenDwellMs = scanScreenDwellMs;
    ctx.bleConnectedNow = bleConnectedNow;
    ctx.bootSplashHoldActive = bootSplashHoldActive;
    ctx.displayPreviewRunning = displayPreviewRunning;
    return ctx;
}

void setUp() {
    module.reset();
}

void tearDown() {}

void test_runs_on_display_interval_when_unblocked() {
    auto d0 = module.process(makeContext(0, false, false, false));
    TEST_ASSERT_FALSE(d0.displayUpdateDue);
    TEST_ASSERT_FALSE(d0.shouldRunConnectionStateProcess);

    auto d1 = module.process(makeContext(49, false, false, false));
    TEST_ASSERT_FALSE(d1.displayUpdateDue);
    TEST_ASSERT_FALSE(d1.shouldRunConnectionStateProcess);

    auto d2 = module.process(makeContext(50, false, false, false));
    TEST_ASSERT_TRUE(d2.displayUpdateDue);
    TEST_ASSERT_TRUE(d2.shouldRunConnectionStateProcess);

    auto d3 = module.process(makeContext(80, false, false, false));
    TEST_ASSERT_FALSE(d3.displayUpdateDue);
    TEST_ASSERT_FALSE(d3.shouldRunConnectionStateProcess);

    auto d4 = module.process(makeContext(100, false, false, false));
    TEST_ASSERT_TRUE(d4.displayUpdateDue);
    TEST_ASSERT_TRUE(d4.shouldRunConnectionStateProcess);
}

void test_disconnect_then_reconnect_within_dwell_holds_process() {
    auto prime = module.process(makeContext(50, true, false, false));
    TEST_ASSERT_TRUE(prime.shouldRunConnectionStateProcess);

    auto disconnectTick = module.process(makeContext(100, false, false, false));
    TEST_ASSERT_TRUE(disconnectTick.shouldRunConnectionStateProcess);
    TEST_ASSERT_FALSE(disconnectTick.holdScanDwell);

    auto reconnectWithinDwell = module.process(makeContext(150, true, false, false));
    TEST_ASSERT_TRUE(reconnectWithinDwell.displayUpdateDue);
    TEST_ASSERT_TRUE(reconnectWithinDwell.holdScanDwell);
    TEST_ASSERT_FALSE(reconnectWithinDwell.shouldRunConnectionStateProcess);
}

void test_dwell_expiry_resumes_process_and_clears_hold() {
    module.process(makeContext(50, true, false, false));
    module.process(makeContext(100, false, false, false));
    module.process(makeContext(150, true, false, false));

    auto reconnectAfterDwell = module.process(makeContext(500, true, false, false));
    TEST_ASSERT_TRUE(reconnectAfterDwell.displayUpdateDue);
    TEST_ASSERT_FALSE(reconnectAfterDwell.holdScanDwell);
    TEST_ASSERT_TRUE(reconnectAfterDwell.shouldRunConnectionStateProcess);

    auto nextTick = module.process(makeContext(550, true, false, false));
    TEST_ASSERT_TRUE(nextTick.displayUpdateDue);
    TEST_ASSERT_FALSE(nextTick.holdScanDwell);
    TEST_ASSERT_TRUE(nextTick.shouldRunConnectionStateProcess);
}

void test_scanning_screen_shown_starts_dwell_hold() {
    module.onScanningScreenShown(100);

    auto withinDwell = module.process(makeContext(150, true, false, false));
    TEST_ASSERT_TRUE(withinDwell.displayUpdateDue);
    TEST_ASSERT_TRUE(withinDwell.holdScanDwell);
    TEST_ASSERT_FALSE(withinDwell.shouldRunConnectionStateProcess);

    auto pastDwell = module.process(makeContext(500, true, false, false));
    TEST_ASSERT_TRUE(pastDwell.displayUpdateDue);
    TEST_ASSERT_FALSE(pastDwell.holdScanDwell);
    TEST_ASSERT_TRUE(pastDwell.shouldRunConnectionStateProcess);
}

void test_boot_hold_and_preview_each_block_process_calls() {
    auto bootHold = module.process(makeContext(50, false, true, false));
    TEST_ASSERT_TRUE(bootHold.displayUpdateDue);
    TEST_ASSERT_FALSE(bootHold.shouldRunConnectionStateProcess);

    auto previewHold = module.process(makeContext(100, false, false, true));
    TEST_ASSERT_TRUE(previewHold.displayUpdateDue);
    TEST_ASSERT_FALSE(previewHold.shouldRunConnectionStateProcess);

    auto clearBlocks = module.process(makeContext(150, false, false, false));
    TEST_ASSERT_TRUE(clearBlocks.displayUpdateDue);
    TEST_ASSERT_TRUE(clearBlocks.shouldRunConnectionStateProcess);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_runs_on_display_interval_when_unblocked);
    RUN_TEST(test_disconnect_then_reconnect_within_dwell_holds_process);
    RUN_TEST(test_dwell_expiry_resumes_process_and_clears_hold);
    RUN_TEST(test_scanning_screen_shown_starts_dwell_hold);
    RUN_TEST(test_boot_hold_and_preview_each_block_process_calls);
    return UNITY_END();
}
