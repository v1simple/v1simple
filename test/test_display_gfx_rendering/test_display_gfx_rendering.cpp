/**
 * test_display_gfx_rendering.cpp
 *
 * Tests for display rendering primitives and data tables.
 * Covers the GFX-level code that has no other unit test coverage:
 *   - dimColor() colour math
 *   - DisplaySegments::segMetrics() scale computation
 *   - DIGIT_SEGMENTS 7-segment lookup table correctness
 *   - CHAR14_MAP / get14SegPattern 14-segment lookup correctness
 *   - Arduino_Canvas GFX call recording infrastructure
 */

#include <unity.h>
#include <cstdint>
#include <algorithm>

// Include the mocked display driver (provides Arduino_Canvas with GFX recording)
#include "../mocks/display_driver.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

// Pure display utilities under test
#include "../../include/display_draw.h"
#include "../../include/display_segments.h"
#include "../../include/color_themes.h"

// ============================================================================
// setUp / tearDown
// ============================================================================

void setUp() {}
void tearDown() {}

// ============================================================================
// dimColor tests
// ============================================================================

void test_dimColor_black_stays_black() {
    TEST_ASSERT_EQUAL_UINT16(0x0000, dimColor(0x0000));
}

void test_dimColor_white_dimmed_60pct() {
    // White = 0xFFFF: R=31, G=63, B=31
    // After 60%: R=18, G=37, B=18
    // Packed: (18<<11)|(37<<5)|18 = 0x9269
    const uint16_t result = dimColor(0xFFFF, 60);
    const uint8_t r = (result >> 11) & 0x1F;
    const uint8_t g = (result >> 5)  & 0x3F;
    const uint8_t b = result & 0x1F;
    TEST_ASSERT_EQUAL_UINT8((31 * 60) / 100, r);
    TEST_ASSERT_EQUAL_UINT8((63 * 60) / 100, g);
    TEST_ASSERT_EQUAL_UINT8((31 * 60) / 100, b);
}

void test_dimColor_pure_red_dimmed_50pct() {
    // Pure red 0xF800: R=31, G=0, B=0 -> after 50%: R=15
    const uint16_t result = dimColor(0xF800, 50);
    const uint8_t r = (result >> 11) & 0x1F;
    const uint8_t g = (result >> 5)  & 0x3F;
    const uint8_t b = result & 0x1F;
    TEST_ASSERT_EQUAL_UINT8((31 * 50) / 100, r);
    TEST_ASSERT_EQUAL_UINT8(0, g);
    TEST_ASSERT_EQUAL_UINT8(0, b);
}

void test_dimColor_100pct_is_identity() {
    const uint16_t color = 0xF81F;  // magenta
    // 100% scale: R=31, G=0, B=31 -> should match input
    const uint16_t result = dimColor(color, 100);
    const uint8_t r_in = (color >> 11) & 0x1F;
    const uint8_t g_in = (color >> 5)  & 0x3F;
    const uint8_t b_in = color & 0x1F;
    const uint8_t r_out = (result >> 11) & 0x1F;
    const uint8_t g_out = (result >> 5)  & 0x3F;
    const uint8_t b_out = result & 0x1F;
    TEST_ASSERT_EQUAL_UINT8(r_in, r_out);
    TEST_ASSERT_EQUAL_UINT8(g_in, g_out);
    TEST_ASSERT_EQUAL_UINT8(b_in, b_out);
}

void test_dimColor_0pct_produces_black() {
    TEST_ASSERT_EQUAL_UINT16(0x0000, dimColor(0xFFFF, 0));
}

void test_dimColor_default_scale_is_60pct() {
    // dimColor(c) should equal dimColor(c, 60)
    const uint16_t color = 0x07E0;  // pure green
    TEST_ASSERT_EQUAL_UINT16(dimColor(color, 60), dimColor(color));
}

// ============================================================================
// DisplaySegments::segMetrics tests
// ============================================================================

void test_segMetrics_scale_1_produces_expected_values() {
    const auto m = DisplaySegments::segMetrics(1.0f);
    // segLen = round(8*1) = 8, segThick = round(3*1) = 3
    TEST_ASSERT_EQUAL_INT(8, m.segLen);
    TEST_ASSERT_EQUAL_INT(3, m.segThick);
    // digitW = segLen + 2*segThick = 8+6=14
    TEST_ASSERT_EQUAL_INT(14, m.digitW);
    // digitH = 2*segLen + 3*segThick = 16+9=25
    TEST_ASSERT_EQUAL_INT(25, m.digitH);
}

void test_segMetrics_scale_2_doubles_geometry() {
    const auto m1 = DisplaySegments::segMetrics(1.0f);
    const auto m2 = DisplaySegments::segMetrics(2.0f);
    TEST_ASSERT_EQUAL_INT(m1.segLen * 2,   m2.segLen);
    TEST_ASSERT_EQUAL_INT(m1.segThick * 2, m2.segThick);
    TEST_ASSERT_EQUAL_INT(m1.digitW * 2,   m2.digitW);
    TEST_ASSERT_EQUAL_INT(m1.digitH * 2,   m2.digitH);
}

void test_segMetrics_small_scale_clamps_to_minimum() {
    // Very small scale — segLen must be >= 2, segThick >= 1
    const auto m = DisplaySegments::segMetrics(0.01f);
    TEST_ASSERT_TRUE(m.segLen >= 2);
    TEST_ASSERT_TRUE(m.segThick >= 1);
}

// ============================================================================
// DIGIT_SEGMENTS 7-segment lookup correctness
// ============================================================================

// Segments: a=0, b=1, c=2, d=3, e=4, f=5, g=6
// 0 should have a,b,c,d,e,f lit and g off
void test_7seg_digit_0_correct() {
    const bool* d = DisplaySegments::DIGIT_SEGMENTS[0];
    TEST_ASSERT_TRUE(d[0]);  // a
    TEST_ASSERT_TRUE(d[1]);  // b
    TEST_ASSERT_TRUE(d[2]);  // c
    TEST_ASSERT_TRUE(d[3]);  // d
    TEST_ASSERT_TRUE(d[4]);  // e
    TEST_ASSERT_TRUE(d[5]);  // f
    TEST_ASSERT_FALSE(d[6]); // g (middle bar off for 0)
}

void test_7seg_digit_1_only_b_c_lit() {
    const bool* d = DisplaySegments::DIGIT_SEGMENTS[1];
    TEST_ASSERT_FALSE(d[0]); // a
    TEST_ASSERT_TRUE(d[1]);  // b
    TEST_ASSERT_TRUE(d[2]);  // c
    TEST_ASSERT_FALSE(d[3]); // d
    TEST_ASSERT_FALSE(d[4]); // e
    TEST_ASSERT_FALSE(d[5]); // f
    TEST_ASSERT_FALSE(d[6]); // g
}

void test_7seg_digit_8_all_segments_lit() {
    const bool* d = DisplaySegments::DIGIT_SEGMENTS[8];
    for (int i = 0; i < 7; ++i) {
        TEST_ASSERT_TRUE(d[i]);
    }
}

void test_7seg_digit_7_minimal_segments() {
    // 7 = a, b, c only
    const bool* d = DisplaySegments::DIGIT_SEGMENTS[7];
    TEST_ASSERT_TRUE(d[0]);  // a (top)
    TEST_ASSERT_TRUE(d[1]);  // b (top-right)
    TEST_ASSERT_TRUE(d[2]);  // c (bottom-right)
    TEST_ASSERT_FALSE(d[3]); // d
    TEST_ASSERT_FALSE(d[4]); // e
    TEST_ASSERT_FALSE(d[5]); // f
    TEST_ASSERT_FALSE(d[6]); // g
}

void test_7seg_all_digits_exactly_7_entries() {
    // Compile-time array check — runtime verify count-of-lit vs expected
    // 0:6, 1:2, 2:5, 3:5, 4:4, 5:5, 6:6, 7:3, 8:7, 9:6
    int expected[10] = {6, 2, 5, 5, 4, 5, 6, 3, 7, 6};
    for (int digit = 0; digit < 10; ++digit) {
        int lit = 0;
        for (int seg = 0; seg < 7; ++seg) {
            if (DisplaySegments::DIGIT_SEGMENTS[digit][seg]) ++lit;
        }
        TEST_ASSERT_EQUAL_INT(expected[digit], lit);
    }
}

// ============================================================================
// CHAR14_MAP / get14SegPattern correctness
// ============================================================================

void test_14seg_digit_0_has_six_outer_segments() {
    // '0' should have TOP, TR, BR, BOT, BL, TL (6 segments, no middle)
    const uint16_t pattern = DisplaySegments::get14SegPattern('0');
    TEST_ASSERT_TRUE(pattern & DisplaySegments::S14_TOP);
    TEST_ASSERT_TRUE(pattern & DisplaySegments::S14_TR);
    TEST_ASSERT_TRUE(pattern & DisplaySegments::S14_BR);
    TEST_ASSERT_TRUE(pattern & DisplaySegments::S14_BOT);
    TEST_ASSERT_TRUE(pattern & DisplaySegments::S14_BL);
    TEST_ASSERT_TRUE(pattern & DisplaySegments::S14_TL);
    TEST_ASSERT_FALSE(pattern & DisplaySegments::S14_ML);
    TEST_ASSERT_FALSE(pattern & DisplaySegments::S14_MR);
}

void test_14seg_digit_1_only_right_verticals() {
    const uint16_t pattern = DisplaySegments::get14SegPattern('1');
    TEST_ASSERT_TRUE(pattern & DisplaySegments::S14_TR);
    TEST_ASSERT_TRUE(pattern & DisplaySegments::S14_BR);
    // Should not have left side or top/bot
    TEST_ASSERT_FALSE(pattern & DisplaySegments::S14_TL);
    TEST_ASSERT_FALSE(pattern & DisplaySegments::S14_BL);
    TEST_ASSERT_FALSE(pattern & DisplaySegments::S14_TOP);
}

void test_14seg_lowercase_matches_uppercase() {
    TEST_ASSERT_EQUAL_UINT16(
        DisplaySegments::get14SegPattern('A'),
        DisplaySegments::get14SegPattern('a'));
    TEST_ASSERT_EQUAL_UINT16(
        DisplaySegments::get14SegPattern('L'),
        DisplaySegments::get14SegPattern('l'));
}

void test_14seg_unknown_char_returns_zero() {
    TEST_ASSERT_EQUAL_UINT16(0, DisplaySegments::get14SegPattern('$'));
    TEST_ASSERT_EQUAL_UINT16(0, DisplaySegments::get14SegPattern('?'));
}

void test_14seg_dash_is_middle_segs_only() {
    using namespace DisplaySegments;
    const uint16_t pattern = get14SegPattern('-');
    TEST_ASSERT_EQUAL_UINT16(S14_ML | S14_MR, pattern);
}

void test_14seg_dot_returns_zero() {
    TEST_ASSERT_EQUAL_UINT16(0, DisplaySegments::get14SegPattern('.'));
}

void test_14seg_all_digits_0_through_9_nonzero() {
    for (char c = '0'; c <= '9'; c++) {
        TEST_ASSERT_NOT_EQUAL(0, DisplaySegments::get14SegPattern(c));
    }
}

void test_char14_map_size_is_25() {
    TEST_ASSERT_EQUAL_INT(25, DisplaySegments::CHAR14_MAP_SIZE);
}

void test_segMetrics_fractional_scale_rounds_correctly() {
    // scale=1.5: segLen=int(12.0+0.5)=12, segThick=int(4.5+0.5)=5
    const auto m = DisplaySegments::segMetrics(1.5f);
    TEST_ASSERT_EQUAL_INT(12, m.segLen);
    TEST_ASSERT_EQUAL_INT(5,  m.segThick);
    TEST_ASSERT_EQUAL_INT(22, m.digitW);    // 12 + 2*5
    TEST_ASSERT_EQUAL_INT(39, m.digitH);    // 2*12 + 3*5
}

void test_segMetrics_digitW_invariant() {
    // digitW == segLen + 2*segThick for any scale
    for (int s = 1; s <= 4; s++) {
        const auto m = DisplaySegments::segMetrics(static_cast<float>(s));
        TEST_ASSERT_EQUAL_INT(m.segLen + 2 * m.segThick, m.digitW);
    }
}

// ============================================================================
// ColorThemes palette tests (Task 1.4)
// ============================================================================

void test_color_themes_standard_bg_is_black() {
    TEST_ASSERT_EQUAL_UINT16(0x0000, ColorThemes::STANDARD().bg);
}

void test_color_themes_standard_text_is_white() {
    TEST_ASSERT_EQUAL_UINT16(0xFFFF, ColorThemes::STANDARD().text);
}

void test_color_themes_standard_gray_value() {
    TEST_ASSERT_EQUAL_UINT16(0x1082, ColorThemes::STANDARD().colorGray);
}

// ============================================================================
// Arduino_Canvas GFX call recording smoke tests
// ============================================================================

void test_canvas_records_fill_rect_calls() {
    Arduino_Canvas canvas(640, 172, nullptr);
    canvas.clearRecordedCalls();

    canvas.fillRect(10, 20, 50, 5, 0x07E0);
    canvas.fillRect(100, 50, 30, 10, 0xF800);

    TEST_ASSERT_EQUAL_UINT(2u, canvas.fillRectCalls.size());
    TEST_ASSERT_EQUAL_INT16(10,     canvas.fillRectCalls[0].x);
    TEST_ASSERT_EQUAL_INT16(20,     canvas.fillRectCalls[0].y);
    TEST_ASSERT_EQUAL_INT16(50,     canvas.fillRectCalls[0].w);
    TEST_ASSERT_EQUAL_INT16(5,      canvas.fillRectCalls[0].h);
    TEST_ASSERT_EQUAL_UINT16(0x07E0, canvas.fillRectCalls[0].color);
    TEST_ASSERT_EQUAL_UINT16(0xF800, canvas.fillRectCalls[1].color);
}

void test_canvas_records_draw_rect_calls() {
    Arduino_Canvas canvas(640, 172, nullptr);
    canvas.clearRecordedCalls();

    canvas.drawRect(5, 5, 100, 50, TFT_WHITE);

    TEST_ASSERT_EQUAL_UINT(1u, canvas.drawRectCalls.size());
    TEST_ASSERT_EQUAL_INT16(5,    canvas.drawRectCalls[0].x);
    TEST_ASSERT_EQUAL_UINT16(TFT_WHITE, canvas.drawRectCalls[0].color);
}

void test_canvas_clear_recorded_calls_resets_all_vectors() {
    Arduino_Canvas canvas(640, 172, nullptr);
    canvas.fillRect(0, 0, 10, 10, 0x0000);
    canvas.drawRect(0, 0, 10, 10, 0x0000);
    canvas.fillCircle(5, 5, 3, 0x0000);

    canvas.clearRecordedCalls();

    TEST_ASSERT_EQUAL_UINT(0u, canvas.fillRectCalls.size());
    TEST_ASSERT_EQUAL_UINT(0u, canvas.drawRectCalls.size());
    TEST_ASSERT_EQUAL_UINT(0u, canvas.fillCircleCalls.size());
}

void test_canvas_flush_increments_count() {
    Arduino_Canvas canvas(640, 172, nullptr);
    canvas.resetCounters();

    canvas.flush();
    canvas.flush();

    TEST_ASSERT_EQUAL_INT(2, canvas.getFlushCount());
}

// ============================================================================
// main
// ============================================================================

int main() {
    UNITY_BEGIN();

    // dimColor
    RUN_TEST(test_dimColor_black_stays_black);
    RUN_TEST(test_dimColor_white_dimmed_60pct);
    RUN_TEST(test_dimColor_pure_red_dimmed_50pct);
    RUN_TEST(test_dimColor_100pct_is_identity);
    RUN_TEST(test_dimColor_0pct_produces_black);
    RUN_TEST(test_dimColor_default_scale_is_60pct);

    // segMetrics
    RUN_TEST(test_segMetrics_scale_1_produces_expected_values);
    RUN_TEST(test_segMetrics_scale_2_doubles_geometry);
    RUN_TEST(test_segMetrics_small_scale_clamps_to_minimum);

    // 7-segment lookup
    RUN_TEST(test_7seg_digit_0_correct);
    RUN_TEST(test_7seg_digit_1_only_b_c_lit);
    RUN_TEST(test_7seg_digit_8_all_segments_lit);
    RUN_TEST(test_7seg_digit_7_minimal_segments);
    RUN_TEST(test_7seg_all_digits_exactly_7_entries);

    // 14-segment lookup
    RUN_TEST(test_14seg_digit_0_has_six_outer_segments);
    RUN_TEST(test_14seg_digit_1_only_right_verticals);
    RUN_TEST(test_14seg_lowercase_matches_uppercase);
    RUN_TEST(test_14seg_unknown_char_returns_zero);
    RUN_TEST(test_14seg_dash_is_middle_segs_only);
    RUN_TEST(test_14seg_dot_returns_zero);
    RUN_TEST(test_14seg_all_digits_0_through_9_nonzero);
    RUN_TEST(test_char14_map_size_is_25);

    // segMetrics extended
    RUN_TEST(test_segMetrics_fractional_scale_rounds_correctly);
    RUN_TEST(test_segMetrics_digitW_invariant);

    // ColorThemes palette
    RUN_TEST(test_color_themes_standard_bg_is_black);
    RUN_TEST(test_color_themes_standard_text_is_white);
    RUN_TEST(test_color_themes_standard_gray_value);

    // GFX call recording
    RUN_TEST(test_canvas_records_fill_rect_calls);
    RUN_TEST(test_canvas_records_draw_rect_calls);
    RUN_TEST(test_canvas_clear_recorded_calls_resets_all_vectors);
    RUN_TEST(test_canvas_flush_increments_count);

    return UNITY_END();
}
