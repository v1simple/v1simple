/**
 * test_display_dirty_flags.cpp
 *
 * Phase 1 Task 1.3 — pure logic tests for display_dirty_flags.h.
 * DisplayDirtyFlags is a plain struct with no hardware dependencies.
 */

#include <unity.h>

#include "../../include/display_dirty_flags.h"

// Provide the required extern definition
DisplayDirtyFlags dirty;

void setUp() {
    // Reset to all-false before each test
    dirty = DisplayDirtyFlags{};
}

void tearDown() {}

// ============================================================================
// Initial state
// ============================================================================

void test_dirty_flags_default_all_false() {
    DisplayDirtyFlags f{};
    TEST_ASSERT_FALSE(f.multiAlert);
    TEST_ASSERT_FALSE(f.obdIndicator);
    TEST_ASSERT_FALSE(f.resetTracking);
}

// ============================================================================
// setIndicatorFlags() — sets residual indicator flags, leaves management
// flags alone.  Renamed from setAll() (name was misleading: the method only
// sets obdIndicator + alpIndicator, never multiAlert / resetTracking).
// ============================================================================

void test_dirty_setIndicatorFlags_sets_obd_and_alp_indicators() {
    DisplayDirtyFlags f{};
    f.setIndicatorFlags();
    TEST_ASSERT_TRUE(f.obdIndicator);
    TEST_ASSERT_TRUE(f.alpIndicator);
}

void test_dirty_setIndicatorFlags_does_not_set_multiAlert() {
    DisplayDirtyFlags f{};
    f.setIndicatorFlags();
    TEST_ASSERT_FALSE(f.multiAlert);
}

void test_dirty_setIndicatorFlags_does_not_set_resetTracking() {
    DisplayDirtyFlags f{};
    f.setIndicatorFlags();
    TEST_ASSERT_FALSE(f.resetTracking);
}

void test_dirty_setIndicatorFlags_preserves_previously_set_multiAlert() {
    DisplayDirtyFlags f{};
    f.multiAlert = true;
    f.setIndicatorFlags();
    // setIndicatorFlags does not clear multiAlert
    TEST_ASSERT_TRUE(f.multiAlert);
}

void test_dirty_setIndicatorFlags_preserves_previously_set_resetTracking() {
    DisplayDirtyFlags f{};
    f.resetTracking = true;
    f.setIndicatorFlags();
    TEST_ASSERT_TRUE(f.resetTracking);
}

// ============================================================================
// Individual flag manipulation
// ============================================================================

void test_dirty_individual_flags_can_be_set_independently() {
    DisplayDirtyFlags f{};
    f.gpsIndicator = true;
    TEST_ASSERT_TRUE(f.gpsIndicator);
    TEST_ASSERT_FALSE(f.multiAlert);
    TEST_ASSERT_FALSE(f.obdIndicator);
}

void test_dirty_flag_cleared_by_assignment() {
    DisplayDirtyFlags f{};
    f.setIndicatorFlags();
    f.obdIndicator = false;
    TEST_ASSERT_FALSE(f.obdIndicator);
    // Other flags must still be set
    TEST_ASSERT_FALSE(f.multiAlert);
    TEST_ASSERT_TRUE(f.gpsIndicator);
}

// ============================================================================
// main
// ============================================================================

int main() {
    UNITY_BEGIN();

    RUN_TEST(test_dirty_flags_default_all_false);
    RUN_TEST(test_dirty_setIndicatorFlags_sets_obd_and_alp_indicators);
    RUN_TEST(test_dirty_setIndicatorFlags_does_not_set_multiAlert);
    RUN_TEST(test_dirty_setIndicatorFlags_does_not_set_resetTracking);
    RUN_TEST(test_dirty_setIndicatorFlags_preserves_previously_set_multiAlert);
    RUN_TEST(test_dirty_setIndicatorFlags_preserves_previously_set_resetTracking);
    RUN_TEST(test_dirty_individual_flags_can_be_set_independently);
    RUN_TEST(test_dirty_flag_cleared_by_assignment);

    return UNITY_END();
}
