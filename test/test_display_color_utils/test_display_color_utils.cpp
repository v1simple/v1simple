/**
 * test_display_color_utils.cpp
 *
 * Appendix A Phase 1 Task 1.4 — unit tests for dimColor() and ColorThemes.
 *
 * Covers colour-math edge cases and the standard palette constants.
 * Requires the mock display_driver.h (for Arduino_Canvas type + color macros),
 * but does NOT need a full display object — tests call inline helpers directly.
 */

#include <unity.h>
#include <cstdint>

#include "../mocks/display_driver.h"  // color macros, Arduino_Canvas
#include "../../include/display_draw.h"    // dimColor()
#include "../../include/color_themes.h"   // ColorThemes::STANDARD()

#ifndef ARDUINO
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
SerialClass Serial;
#endif

void setUp()    {}
void tearDown() {}

// ============================================================================
// dimColor — channel math
// ============================================================================

// Helper: Extract and verify RGB channels from dimColor result
static void verify_dimColor_channels(uint16_t color, uint8_t percent,
                                     uint8_t expected_r, uint8_t expected_g, uint8_t expected_b,
                                     const char* description) {
    uint16_t result = dimColor(color, percent);
    uint8_t r = (result >> 11) & 0x1F;
    uint8_t g = (result >>  5) & 0x3F;
    uint8_t b =  result        & 0x1F;

    char msg[96];
    snprintf(msg, sizeof(msg), "%s: R mismatch at %u%%", description, percent);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(expected_r, r, msg);

    snprintf(msg, sizeof(msg), "%s: G mismatch at %u%%", description, percent);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(expected_g, g, msg);

    snprintf(msg, sizeof(msg), "%s: B mismatch at %u%%", description, percent);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(expected_b, b, msg);
}

void test_dimColor_pure_green_at_50pct() {
    // Pure green = 0x07E0 → G channel = 63
    // After 50%: G = 63*50/100 = 31 → 0x03E0
    verify_dimColor_channels(0x07E0, 50, 0, 31, 0, "pure_green_50pct");
}

void test_dimColor_pure_blue_at_25pct() {
    // Pure blue = 0x001F → B channel = 31
    // After 25%: B = 31*25/100 = 7 → 0x0007
    uint16_t result = dimColor(0x001F, 25);
    uint8_t b = result & 0x1F;
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(7, b, "pure_blue_25pct: B mismatch");
}

void test_dimColor_magenta_at_50pct() {
    // Magenta = 0xF81F → R=31, G=0, B=31
    // After 50%: R=15, G=0, B=15 → (15<<11)|(0<<5)|15 = 0x780F
    verify_dimColor_channels(0xF81F, 50, 15, 0, 15, "magenta_50pct");
}

void test_dimColor_white_at_0pct_equals_black() {
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0x0000, dimColor(0xFFFF, 0), "white at 0% should be black");
}

void test_dimColor_white_at_100pct_preserves_all_channels() {
    verify_dimColor_channels(0xFFFF, 100, 31, 63, 31, "white_100pct");
}

void test_dimColor_black_unchanged_at_any_percent() {
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0x0000, dimColor(0x0000, 0),   "black at 0%");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0x0000, dimColor(0x0000, 50),  "black at 50%");
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0x0000, dimColor(0x0000, 100), "black at 100%");
}

void test_dimColor_default_parameter_is_60pct() {
    // dimColor(c) == dimColor(c, 60) for arbitrary colour
    uint16_t color = 0x8410;  // mid-gray
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(dimColor(color, 60), dimColor(color), "default parameter should be 60%");
}

// ============================================================================
// ColorThemes::STANDARD — palette constants
// ============================================================================

void test_standard_palette_bg_is_black() {
    TEST_ASSERT_EQUAL_UINT16(0x0000, ColorThemes::STANDARD().bg);
}

void test_standard_palette_text_is_white() {
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, ColorThemes::STANDARD().text);
}

void test_standard_palette_colorGray_value() {
    // 0x1082 is the designated "dark gray" for resting/inactive state
    TEST_ASSERT_EQUAL_UINT16(0x1082, ColorThemes::STANDARD().colorGray);
}

void test_standard_palette_bg_is_min_value() {
    TEST_ASSERT_EQUAL_UINT16(0, ColorThemes::STANDARD().bg);
}

void test_standard_palette_is_reentrant() {
    // STANDARD() returns a const ref to a static — calling it twice must be identical
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().bg,        ColorThemes::STANDARD().bg);
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().text,      ColorThemes::STANDARD().text);
    TEST_ASSERT_EQUAL_UINT16(ColorThemes::STANDARD().colorGray, ColorThemes::STANDARD().colorGray);
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_dimColor_pure_green_at_50pct);
    RUN_TEST(test_dimColor_pure_blue_at_25pct);
    RUN_TEST(test_dimColor_magenta_at_50pct);
    RUN_TEST(test_dimColor_white_at_0pct_equals_black);
    RUN_TEST(test_dimColor_white_at_100pct_preserves_all_channels);
    RUN_TEST(test_dimColor_black_unchanged_at_any_percent);
    RUN_TEST(test_dimColor_default_parameter_is_60pct);

    RUN_TEST(test_standard_palette_bg_is_black);
    RUN_TEST(test_standard_palette_text_is_white);
    RUN_TEST(test_standard_palette_colorGray_value);
    RUN_TEST(test_standard_palette_bg_is_min_value);
    RUN_TEST(test_standard_palette_is_reentrant);

    return UNITY_END();
}
