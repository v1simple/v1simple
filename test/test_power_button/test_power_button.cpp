/**
 * Unit tests for processPowerButtonState() — the pure, hardware-free
 * button-logic helper extracted from BatteryManager::processPowerButton().
 *
 * These tests verify the 2-second hold logic without any GPIO hardware.
 * The release-before-press safety (buttonSeenReleasedSinceBoot_) is covered
 * by the CAR_MODE_PWR_SHORT integration tests which test the full
 * processPowerButton() path via the mock BatteryManager.
 */
#include <unity.h>

#include "../mocks/Arduino.h"

// Pull in the pure button-state helper only — no hardware TCA9554/ADC code.
#include "../../src/battery_manager_button.cpp"

void setUp() {}
void tearDown() {}

using State = BatteryManager::PwrButtonState;

// ─── Tests ────────────────────────────────────────────────────────────────────

void test_no_press_returns_false() {
    State s;
    TEST_ASSERT_FALSE(processPowerButtonState(false, 1000, s));
}

void test_short_press_and_release_returns_false() {
    State s;
    processPowerButtonState(true, 0, s);     // press
    processPowerButtonState(true, 1000, s);  // held for 1s
    processPowerButtonState(false, 1001, s); // release
    TEST_ASSERT_FALSE(processPowerButtonState(false, 2000, s));
}

void test_long_press_triggers_at_2000ms() {
    State s;
    processPowerButtonState(true, 0, s);     // press
    TEST_ASSERT_FALSE(processPowerButtonState(true, 1999, s)); // just under
    TEST_ASSERT_TRUE(processPowerButtonState(true, 2000, s));  // exactly 2s
}

void test_long_press_triggers_after_2000ms() {
    State s;
    processPowerButtonState(true, 0, s);
    TEST_ASSERT_TRUE(processPowerButtonState(true, 3000, s));
}

void test_release_resets_press_state() {
    State s;
    processPowerButtonState(true, 0, s);
    TEST_ASSERT_TRUE(s.buttonWasPressed);
    processPowerButtonState(false, 500, s); // release
    TEST_ASSERT_FALSE(s.buttonWasPressed);
    // Re-press must restart the 2s timer from the new press time.
    processPowerButtonState(true, 1000, s);  // new press at t=1000
    TEST_ASSERT_FALSE(processPowerButtonState(true, 2999, s)); // 1999ms held
    TEST_ASSERT_TRUE(processPowerButtonState(true, 3000, s));  // 2000ms held
}

void test_press_start_captured_correctly() {
    State s;
    processPowerButtonState(true, 5000, s);
    TEST_ASSERT_EQUAL_UINT32(5000u, s.buttonPressStart);
}

void test_result_is_idempotent_once_held() {
    // Once past 2s, every subsequent call while held should return true.
    State s;
    processPowerButtonState(true, 0, s);
    TEST_ASSERT_TRUE(processPowerButtonState(true, 2000, s));
    TEST_ASSERT_TRUE(processPowerButtonState(true, 2100, s));
    TEST_ASSERT_TRUE(processPowerButtonState(true, 5000, s));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_no_press_returns_false);
    RUN_TEST(test_short_press_and_release_returns_false);
    RUN_TEST(test_long_press_triggers_at_2000ms);
    RUN_TEST(test_long_press_triggers_after_2000ms);
    RUN_TEST(test_release_resets_press_state);
    RUN_TEST(test_press_start_captured_correctly);
    RUN_TEST(test_result_is_idempotent_once_held);
    return UNITY_END();
}
