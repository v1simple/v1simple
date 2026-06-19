/**
 * test_display_slider_math.cpp
 *
 * Phase 1 Task 1.2 — pure logic tests for display_slider_math.h.
 * computeBrightnessSliderFill and computeBrightnessSliderPercent are
 * dependency-free inline functions; no Arduino environment required.
 */

#include <unity.h>
#include <cstdint>

#include "../../include/display_slider_math.h"

void setUp() {}
void tearDown() {}

// ============================================================================
// computeBrightnessSliderFill tests
// formula: clamp(((brightnessLevel - 80) * sliderWidth) / 175, 0, sliderWidth)
// ============================================================================

void test_sliderFill_minimum_brightness_gives_zero() {
    // brightnessLevel=80: (80-80)*W/175 = 0
    TEST_ASSERT_EQUAL_INT(0, computeBrightnessSliderFill(80, 200));
}

void test_sliderFill_maximum_brightness_fills_slider() {
    // brightnessLevel=255: (255-80)*200/175 = 175*200/175 = 200
    TEST_ASSERT_EQUAL_INT(200, computeBrightnessSliderFill(255, 200));
}

void test_sliderFill_midpoint_brightness() {
    // brightnessLevel=167: (167-80)*200/175 = 87*200/175 = 17400/175 = 99 (integer div)
    TEST_ASSERT_EQUAL_INT(99, computeBrightnessSliderFill(167, 200));
}

void test_sliderFill_below_minimum_clamped_to_zero() {
    // brightnessLevel < 80 produces negative before clamp -> clamped to 0
    TEST_ASSERT_EQUAL_INT(0, computeBrightnessSliderFill(0, 200));
    TEST_ASSERT_EQUAL_INT(0, computeBrightnessSliderFill(79, 200));
}

void test_sliderFill_narrow_slider_width() {
    // sliderWidth=100: (255-80)*100/175 = 17500/175 = 100
    TEST_ASSERT_EQUAL_INT(100, computeBrightnessSliderFill(255, 100));
}

void test_sliderFill_exact_division_no_rounding() {
    // brightnessLevel=167: (167-80)=87; sliderWidth=175 → 87*175/175 = 87 (exact)
    TEST_ASSERT_EQUAL_INT(87, computeBrightnessSliderFill(167, 175));
}

// ============================================================================
// computeBrightnessSliderPercent tests
// formula: clamp(((brightnessLevel - 80) * 100) / 175, 0, 100)
// ============================================================================

void test_sliderPercent_minimum_brightness_is_zero() {
    TEST_ASSERT_EQUAL_INT(0, computeBrightnessSliderPercent(80));
}

void test_sliderPercent_maximum_brightness_is_100() {
    // (255-80)*100/175 = 17500/175 = 100
    TEST_ASSERT_EQUAL_INT(100, computeBrightnessSliderPercent(255));
}

void test_sliderPercent_below_minimum_clamped() {
    TEST_ASSERT_EQUAL_INT(0, computeBrightnessSliderPercent(0));
    TEST_ASSERT_EQUAL_INT(0, computeBrightnessSliderPercent(79));
}

void test_sliderPercent_midpoint() {
    // brightnessLevel=167: (167-80)*100/175 = 8700/175 = 49 (integer div)
    TEST_ASSERT_EQUAL_INT(49, computeBrightnessSliderPercent(167));
}

// ============================================================================
// main
// ============================================================================

int main() {
    UNITY_BEGIN();

    // computeBrightnessSliderFill
    RUN_TEST(test_sliderFill_minimum_brightness_gives_zero);
    RUN_TEST(test_sliderFill_maximum_brightness_fills_slider);
    RUN_TEST(test_sliderFill_midpoint_brightness);
    RUN_TEST(test_sliderFill_below_minimum_clamped_to_zero);
    RUN_TEST(test_sliderFill_narrow_slider_width);
    RUN_TEST(test_sliderFill_exact_division_no_rounding);

    // computeBrightnessSliderPercent
    RUN_TEST(test_sliderPercent_minimum_brightness_is_zero);
    RUN_TEST(test_sliderPercent_maximum_brightness_is_100);
    RUN_TEST(test_sliderPercent_below_minimum_clamped);
    RUN_TEST(test_sliderPercent_midpoint);

    return UNITY_END();
}
