/**
 * test_display_vol_warn.cpp
 *
 * Phase 1 Task 1.5 — state-machine tests for VolumeZeroWarning.
 * Controls time via mockMillis (provided by the Arduino.h mock).
 */

#include <unity.h>

// Arduino.h mock first — provides millis() via mockMillis
#include "Arduino.h"

#ifndef ARDUINO
// Define extern time variables expected by the mock
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
SerialClass Serial;
#endif

// Unit under test
#include "../../include/display_vol_warn.h"

// Required extern definition
VolumeZeroWarning volZeroWarn;

void setUp() {
    mockMillis   = 1000;   // Start non-zero to avoid confusing the detectedMs==0 sentinel
    volZeroWarn.reset();
}

void tearDown() {}

// ============================================================================
// Basic suppression cases — evaluate must return false and reset state
// ============================================================================

void test_volWarn_not_shown_when_vol_nonzero() {
    TEST_ASSERT_FALSE(volZeroWarn.evaluate(false, false, false));
    TEST_ASSERT_EQUAL_UINT(0u, volZeroWarn.detectedMs);
}

void test_volWarn_suppressed_when_proxy_connected() {
    // Prime the timer
    const unsigned long t0 = mockMillis;
    volZeroWarn.evaluate(true, false, false);   // detectedMs = t0
    mockMillis = t0 + VolumeZeroWarning::DELAY_MS + 1;

    // Proxy appears — should reset
    const bool active = volZeroWarn.evaluate(true, /*proxyConnected=*/true, false);
    TEST_ASSERT_FALSE(active);
    TEST_ASSERT_EQUAL_UINT(0u, volZeroWarn.detectedMs);
}

void test_volWarn_suppressed_when_speedVolZero_active() {
    const unsigned long t0 = mockMillis;
    volZeroWarn.evaluate(true, false, false);   // detectedMs = t0
    mockMillis = t0 + VolumeZeroWarning::DELAY_MS + 1;

    const bool active = volZeroWarn.evaluate(true, false, /*speedVolZeroActive=*/true);
    TEST_ASSERT_FALSE(active);
    TEST_ASSERT_EQUAL_UINT(0u, volZeroWarn.detectedMs);
}

// ============================================================================
// Delay phase — not shown until DELAY_MS has passed
// ============================================================================

void test_volWarn_not_shown_before_delay_expires() {
    const unsigned long t0 = mockMillis;
    volZeroWarn.evaluate(true, false, false);           // records detectedMs=t0

    mockMillis = t0 + VolumeZeroWarning::DELAY_MS - 1;
    TEST_ASSERT_FALSE(volZeroWarn.evaluate(true, false, false));
    TEST_ASSERT_FALSE(volZeroWarn.shown);
}

// ============================================================================
// Active phase — shown for DURATION_MS after DELAY_MS
// ============================================================================

void test_volWarn_shown_after_delay() {
    const unsigned long t0 = mockMillis;
    volZeroWarn.evaluate(true, false, false);   // detectedMs = t0

    mockMillis = t0 + VolumeZeroWarning::DELAY_MS;
    TEST_ASSERT_TRUE(volZeroWarn.evaluate(true, false, false));
    TEST_ASSERT_TRUE(volZeroWarn.shown);
}

void test_volWarn_active_during_duration() {
    const unsigned long t0 = mockMillis;
    volZeroWarn.evaluate(true, false, false);   // detectedMs = t0

    mockMillis = t0 + VolumeZeroWarning::DELAY_MS;
    volZeroWarn.evaluate(true, false, false);   // warningStartMs = t0+DELAY_MS

    mockMillis = t0 + VolumeZeroWarning::DELAY_MS + VolumeZeroWarning::DURATION_MS - 1;
    TEST_ASSERT_TRUE(volZeroWarn.evaluate(true, false, false));
}

void test_volWarn_expires_after_duration() {
    const unsigned long t0 = mockMillis;
    volZeroWarn.evaluate(true, false, false);   // detectedMs = t0

    mockMillis = t0 + VolumeZeroWarning::DELAY_MS;
    volZeroWarn.evaluate(true, false, false);   // warningStartMs set

    mockMillis = t0 + VolumeZeroWarning::DELAY_MS + VolumeZeroWarning::DURATION_MS;
    TEST_ASSERT_FALSE(volZeroWarn.evaluate(true, false, false));
    TEST_ASSERT_TRUE(volZeroWarn.acknowledged);
    TEST_ASSERT_FALSE(volZeroWarn.shown);
}

void test_volWarn_acknowledged_prevents_reshowing() {
    const unsigned long t0 = mockMillis;
    volZeroWarn.evaluate(true, false, false);   // detectedMs = t0

    mockMillis = t0 + VolumeZeroWarning::DELAY_MS;
    volZeroWarn.evaluate(true, false, false);
    mockMillis = t0 + VolumeZeroWarning::DELAY_MS + VolumeZeroWarning::DURATION_MS;
    volZeroWarn.evaluate(true, false, false);   // acknowledged

    // Advance time further — must stay false
    mockMillis += 10000;
    TEST_ASSERT_FALSE(volZeroWarn.evaluate(true, false, false));
}

// ============================================================================
// needsFlashRedraw
// ============================================================================

void test_volWarn_needsFlashRedraw_true_when_timer_not_started() {
    // detectedMs==0, volZero=true, no suppressions → force full redraw
    TEST_ASSERT_TRUE(volZeroWarn.needsFlashRedraw(true, false, false));
}

void test_volWarn_needsFlashRedraw_false_when_not_volZero() {
    TEST_ASSERT_FALSE(volZeroWarn.needsFlashRedraw(false, false, false));
}

// ============================================================================
// main
// ============================================================================

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_volWarn_not_shown_when_vol_nonzero);
    RUN_TEST(test_volWarn_suppressed_when_proxy_connected);
    RUN_TEST(test_volWarn_suppressed_when_speedVolZero_active);
    RUN_TEST(test_volWarn_not_shown_before_delay_expires);
    RUN_TEST(test_volWarn_shown_after_delay);
    RUN_TEST(test_volWarn_active_during_duration);
    RUN_TEST(test_volWarn_expires_after_duration);
    RUN_TEST(test_volWarn_acknowledged_prevents_reshowing);
    RUN_TEST(test_volWarn_needsFlashRedraw_true_when_timer_not_started);
    RUN_TEST(test_volWarn_needsFlashRedraw_false_when_not_volZero);

    return UNITY_END();
}
