#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/wifi/wifi_visual_sync_module.cpp"

static WifiVisualSyncModule module;
static int drawCalls = 0;
static unsigned long lastDrawMs = 0;
static unsigned long pendingDrawNowMs = 0;

static void resetDrawState() {
    drawCalls = 0;
    lastDrawMs = 0;
    pendingDrawNowMs = 0;
}

static void recordDraw(void* /*ctx*/) {
    drawCalls++;
    lastDrawMs = pendingDrawNowMs;
}

static void processWithDraw(unsigned long nowMs,
                            bool wifiActive,
                            bool previewRunning,
                            bool bootHold) {
    pendingDrawNowMs = nowMs;
    module.process(nowMs, wifiActive, previewRunning, bootHold, recordDraw, nullptr);
}

void setUp() {
    module.reset();
    resetDrawState();
}

void tearDown() {}

void test_state_transition_triggers_draw() {
    processWithDraw(1000, false, false, false);
    TEST_ASSERT_EQUAL_INT(0, drawCalls);

    processWithDraw(1100, true, false, false);
    TEST_ASSERT_EQUAL_INT(1, drawCalls);
    TEST_ASSERT_EQUAL_UINT32(1100, lastDrawMs);
}

void test_periodic_refresh_runs_every_2s_when_active() {
    processWithDraw(100, true, false, false);
    TEST_ASSERT_EQUAL_INT(1, drawCalls);

    processWithDraw(2099, true, false, false);
    TEST_ASSERT_EQUAL_INT(1, drawCalls);

    processWithDraw(2100, true, false, false);
    TEST_ASSERT_EQUAL_INT(2, drawCalls);
    TEST_ASSERT_EQUAL_UINT32(2100, lastDrawMs);
}

void test_preview_or_boot_hold_blocks_draw_but_preserves_state_machine() {
    processWithDraw(1000, true, true, false);
    TEST_ASSERT_EQUAL_INT(0, drawCalls);

    processWithDraw(1500, true, false, false);
    TEST_ASSERT_EQUAL_INT(0, drawCalls);

    processWithDraw(2000, true, false, false);
    TEST_ASSERT_EQUAL_INT(1, drawCalls);
    TEST_ASSERT_EQUAL_UINT32(2000, lastDrawMs);

    processWithDraw(4000, true, false, true);
    TEST_ASSERT_EQUAL_INT(1, drawCalls);

    processWithDraw(4001, true, false, false);
    TEST_ASSERT_EQUAL_INT(2, drawCalls);
    TEST_ASSERT_EQUAL_UINT32(4001, lastDrawMs);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_state_transition_triggers_draw);
    RUN_TEST(test_periodic_refresh_runs_every_2s_when_active);
    RUN_TEST(test_preview_or_boot_hold_blocks_draw_but_preserves_state_machine);
    return UNITY_END();
}
