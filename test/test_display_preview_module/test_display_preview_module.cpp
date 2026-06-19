#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/display.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/display/display_preview_module.cpp"

PerfCounters perfCounters;
PerfExtendedMetrics perfExtended;

namespace {
V1Display testDisplay;
DisplayPreviewModule preview;
PerfDisplayRenderScenario lastScenario = PerfDisplayRenderScenario::None;
int scenarioSetCalls = 0;
int scenarioClearCalls = 0;
int scenarioRenderCalls = 0;
uint32_t lastScenarioRenderUs = 0;
}

void perfSetDisplayRenderScenario(PerfDisplayRenderScenario scenario) {
    lastScenario = scenario;
    ++scenarioSetCalls;
}

PerfDisplayRenderScenario perfGetDisplayRenderScenario() {
    return lastScenario;
}

void perfClearDisplayRenderScenario() {
    lastScenario = PerfDisplayRenderScenario::None;
    ++scenarioClearCalls;
}

void perfRecordDisplayScenarioRenderUs(uint32_t us) {
    lastScenarioRenderUs = us;
    ++scenarioRenderCalls;
}

void setUp() {
    testDisplay.reset();
    preview = DisplayPreviewModule();
    preview.begin(&testDisplay);
    mockMillis = 10000;
    mockMicros = 1000000;
    lastScenario = PerfDisplayRenderScenario::None;
    scenarioSetCalls = 0;
    scenarioClearCalls = 0;
    scenarioRenderCalls = 0;
    lastScenarioRenderUs = 0;
}

void tearDown() {}

void test_preview_renders_first_step_on_initial_update() {
    preview.requestHold(5500);

    preview.update();

    TEST_ASSERT_EQUAL_INT(1, testDisplay.updateCalls);
    TEST_ASSERT_TRUE(testDisplay.hasLastPriorityAlert);
    TEST_ASSERT_EQUAL_UINT32(10525u, testDisplay.lastPriorityAlert.frequency);
    TEST_ASSERT_EQUAL_INT(BAND_X, testDisplay.lastPriorityAlert.band);
    TEST_ASSERT_EQUAL_INT(DIR_FRONT, testDisplay.lastPriorityAlert.direction);
    TEST_ASSERT_EQUAL_INT(1, scenarioSetCalls);
    TEST_ASSERT_EQUAL(PerfDisplayRenderScenario::None, lastScenario);
    TEST_ASSERT_EQUAL_INT(1, scenarioClearCalls);
    TEST_ASSERT_EQUAL_INT(1, scenarioRenderCalls);
    TEST_ASSERT_EQUAL_INT(1, testDisplay.setPreviewIndicatorOverridesActiveCalls);
    TEST_ASSERT_TRUE(testDisplay.lastPreviewIndicatorOverridesActive);
}

void test_preview_skips_missed_steps_without_catchup_burst() {
    preview.requestHold(0);

    // Simulate preview work being skipped for long enough that the timed table
    // has advanced from step 0 to step 3.  The preview module must drop obsolete
    // visual frames and perform only one display update in this loop.
    mockMillis += 6000;
    mockMicros += 50;

    preview.update();

    TEST_ASSERT_EQUAL_INT(1, testDisplay.updateCalls);
    TEST_ASSERT_TRUE(testDisplay.hasLastPriorityAlert);
    TEST_ASSERT_EQUAL_UINT32(24150u, testDisplay.lastPriorityAlert.frequency);
    TEST_ASSERT_EQUAL_INT(BAND_K, testDisplay.lastPriorityAlert.band);
    TEST_ASSERT_EQUAL_INT(DIR_FRONT, testDisplay.lastPriorityAlert.direction);
    TEST_ASSERT_EQUAL_INT(1, scenarioSetCalls);
    TEST_ASSERT_EQUAL_INT(1, scenarioClearCalls);
    TEST_ASSERT_EQUAL_INT(1, scenarioRenderCalls);
    TEST_ASSERT_TRUE_MESSAGE(preview.isRunning(),
                             "zero-duration request should use full diagnostic auto-duration");

    preview.update();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, testDisplay.updateCalls,
                                  "same timed step must not render again");
}

void test_preview_honors_short_requested_duration() {
    preview.requestHold(5500);

    preview.update();
    mockMillis += 2000;
    preview.update();
    mockMillis += 2000;
    preview.update();

    TEST_ASSERT_TRUE(preview.isRunning());
    TEST_ASSERT_EQUAL_INT(3, testDisplay.updateCalls);

    const int updatesBeforeExpiry = testDisplay.updateCalls;
    const int clearsBeforeExpiry = testDisplay.clearAlpFrequencyOverrideCalls;

    mockMillis += 1500;
    preview.update();

    TEST_ASSERT_FALSE(preview.isRunning());
    TEST_ASSERT_TRUE(preview.consumeEnded());
    TEST_ASSERT_EQUAL_INT_MESSAGE(updatesBeforeExpiry, testDisplay.updateCalls,
                                  "expired short preview must not render another frame");
    TEST_ASSERT_EQUAL_INT(clearsBeforeExpiry + 1, testDisplay.clearAlpFrequencyOverrideCalls);
    TEST_ASSERT_FALSE(testDisplay.lastAlpPreviewEnabled);
    TEST_ASSERT_FALSE(testDisplay.lastObdPreviewEnabled);
    TEST_ASSERT_FALSE(testDisplay.lastBleProxyEnabled);
    TEST_ASSERT_EQUAL_INT(2, testDisplay.setPreviewIndicatorOverridesActiveCalls);
    TEST_ASSERT_FALSE(testDisplay.lastPreviewIndicatorOverridesActive);
}

void test_preview_fast_forward_preserves_carry_state_for_rendered_step() {
    preview.requestHold(0);

    // Step 36 changes mode to Logic ('l') while keeping profile slot inherited
    // from the earlier carry state.  Jumping directly to it should still apply
    // skipped carry-state changes without rendering each skipped frame.
    mockMillis += 36UL * 2000UL;

    preview.update();

    TEST_ASSERT_EQUAL_INT(1, testDisplay.updateCalls);
    TEST_ASSERT_TRUE(testDisplay.hasLastAlertDisplayState);
    TEST_ASSERT_TRUE(testDisplay.lastAlertDisplayState.hasMode);
    TEST_ASSERT_EQUAL_CHAR('l', testDisplay.lastAlertDisplayState.modeChar);
    TEST_ASSERT_EQUAL_INT(1, testDisplay.setProfileIndicatorSlotCalls);
    TEST_ASSERT_EQUAL_INT(0, testDisplay.lastProfileIndicatorSlotValue);
}

void test_long_requested_preview_loops_visual_sequence() {
    preview.requestHold(90000);

    mockMillis += 89000;
    preview.update();

    TEST_ASSERT_TRUE(preview.isRunning());
    TEST_ASSERT_EQUAL_INT(1, testDisplay.updateCalls);
    TEST_ASSERT_TRUE(testDisplay.hasLastPriorityAlert);
    TEST_ASSERT_EQUAL_UINT32(10525u, testDisplay.lastPriorityAlert.frequency);
    TEST_ASSERT_EQUAL_INT(BAND_X, testDisplay.lastPriorityAlert.band);
    TEST_ASSERT_EQUAL_INT(DIR_FRONT, testDisplay.lastPriorityAlert.direction);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_preview_renders_first_step_on_initial_update);
    RUN_TEST(test_preview_skips_missed_steps_without_catchup_burst);
    RUN_TEST(test_preview_honors_short_requested_duration);
    RUN_TEST(test_preview_fast_forward_preserves_carry_state_for_rendered_step);
    RUN_TEST(test_long_requested_preview_loops_visual_sequence);
    return UNITY_END();
}
