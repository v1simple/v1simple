/**
 * test_display_segments.cpp
 *
 * Appendix A Phase 1 Task 1.1 — unit tests for display_segments.h.
 *
 * Covers extra segment geometry and character-map edge cases that are not
 * exercised by test_display_gfx_rendering (which covers the basic smoke
 * tests).  All functions are pure math / lookup tables: no Arduino
 * dependencies, no mocks required.
 */

#include <unity.h>
#include <cstdint>
#include <initializer_list>

#include "../../include/display_segments.h"

using namespace DisplaySegments;

void setUp()    {}
void tearDown() {}

// ============================================================================
// segMetrics — geometry invariants
// ============================================================================

void test_segMetrics_digitH_is_twice_segLen_plus_3_segThick() {
    // digitH = 2*segLen + 3*segThick
    for (float s : {0.5f, 1.0f, 1.5f, 2.0f, 3.0f}) {
        auto m = segMetrics(s);
        TEST_ASSERT_EQUAL_INT(2 * m.segLen + 3 * m.segThick, m.digitH);
    }
}

void test_segMetrics_spacing_equals_segThick() {
    auto m = segMetrics(1.0f);
    TEST_ASSERT_EQUAL_INT(m.segThick, m.spacing);
}

void test_segMetrics_dot_equals_segThick() {
    auto m = segMetrics(1.0f);
    TEST_ASSERT_EQUAL_INT(m.segThick, m.dot);
}

void test_segMetrics_scale_0_clamps_segLen_to_2() {
    auto m = segMetrics(0.0f);
    TEST_ASSERT_EQUAL_INT(2, m.segLen);
}

void test_segMetrics_scale_0_clamps_segThick_to_1() {
    auto m = segMetrics(0.0f);
    TEST_ASSERT_EQUAL_INT(1, m.segThick);
}

// ============================================================================
// DIGIT_SEGMENTS — full pattern verification
// ============================================================================

void test_digit_2_has_correct_segments() {
    // 2: a,b,–,d,e,–,g → {true,true,false,true,true,false,true}
    TEST_ASSERT_TRUE (DIGIT_SEGMENTS[2][0]);  // a
    TEST_ASSERT_TRUE (DIGIT_SEGMENTS[2][1]);  // b
    TEST_ASSERT_FALSE(DIGIT_SEGMENTS[2][2]);  // c
    TEST_ASSERT_TRUE (DIGIT_SEGMENTS[2][3]);  // d
    TEST_ASSERT_TRUE (DIGIT_SEGMENTS[2][4]);  // e
    TEST_ASSERT_FALSE(DIGIT_SEGMENTS[2][5]);  // f
    TEST_ASSERT_TRUE (DIGIT_SEGMENTS[2][6]);  // g
}

void test_digit_4_has_no_top_or_bottom_or_e_segs() {
    // 4: –,b,c,–,–,f,g
    TEST_ASSERT_FALSE(DIGIT_SEGMENTS[4][0]);  // a (top)
    TEST_ASSERT_FALSE(DIGIT_SEGMENTS[4][3]);  // d (bottom)
    TEST_ASSERT_FALSE(DIGIT_SEGMENTS[4][4]);  // e (bottom-left)
}

void test_digit_5_has_no_b_or_e_segs() {
    // 5: a,–,c,d,–,f,g
    TEST_ASSERT_FALSE(DIGIT_SEGMENTS[5][1]);  // b
    TEST_ASSERT_FALSE(DIGIT_SEGMENTS[5][4]);  // e
}

void test_all_10_digits_have_exactly_7_segment_values() {
    for (int d = 0; d < 10; d++) {
        int count = 0;
        for (int s = 0; s < 7; s++) count += DIGIT_SEGMENTS[d][s] ? 1 : 0;
        // Every digit must have at least 2 segments lit (digit 1 = minimum)
        TEST_ASSERT_TRUE(count >= 2);
    }
}

// ============================================================================
// get14SegPattern — character coverage
// ============================================================================

void test_14seg_all_uppercase_letters_A_through_E_nonzero() {
    for (char c : {'A', 'B', 'C', 'D', 'E'}) {
        TEST_ASSERT_NOT_EQUAL(0, get14SegPattern(c));
    }
}

void test_14seg_L_has_only_left_and_bottom_segs() {
    uint16_t p = get14SegPattern('L');
    TEST_ASSERT_NOT_EQUAL(0u, p & S14_TL);
    TEST_ASSERT_NOT_EQUAL(0u, p & S14_BL);
    TEST_ASSERT_NOT_EQUAL(0u, p & S14_BOT);
    // Should NOT have top or right segments
    TEST_ASSERT_EQUAL(0u, p & S14_TOP);
    TEST_ASSERT_EQUAL(0u, p & S14_TR);
    TEST_ASSERT_EQUAL(0u, p & S14_BR);
}

void test_14seg_dash_has_only_middle_segs() {
    uint16_t p = get14SegPattern('-');
    TEST_ASSERT_NOT_EQUAL(0u, p & S14_ML);
    TEST_ASSERT_NOT_EQUAL(0u, p & S14_MR);
    // No top, bottom, or vertical segments
    TEST_ASSERT_EQUAL(0u, p & S14_TOP);
    TEST_ASSERT_EQUAL(0u, p & S14_BOT);
    TEST_ASSERT_EQUAL(0u, p & S14_TL);
    TEST_ASSERT_EQUAL(0u, p & S14_TR);
}

void test_14seg_zero_has_outer_ring_only() {
    uint16_t p = get14SegPattern('0');
    // Must have all outer segments
    TEST_ASSERT_NOT_EQUAL(0u, p & S14_TOP);
    TEST_ASSERT_NOT_EQUAL(0u, p & S14_TR);
    TEST_ASSERT_NOT_EQUAL(0u, p & S14_BR);
    TEST_ASSERT_NOT_EQUAL(0u, p & S14_BOT);
    TEST_ASSERT_NOT_EQUAL(0u, p & S14_BL);
    TEST_ASSERT_NOT_EQUAL(0u, p & S14_TL);
    // Must NOT have middle segments
    TEST_ASSERT_EQUAL(0u, p & S14_ML);
    TEST_ASSERT_EQUAL(0u, p & S14_MR);
}

void test_14seg_lowercase_e_matches_uppercase_E() {
    TEST_ASSERT_EQUAL_UINT16(get14SegPattern('E'), get14SegPattern('e'));
}

void test_14seg_space_and_unknown_return_zero() {
    TEST_ASSERT_EQUAL(0, get14SegPattern(' '));
    TEST_ASSERT_EQUAL(0, get14SegPattern('!'));
    TEST_ASSERT_EQUAL(0, get14SegPattern('Z'));
}

void test_char14_map_size_is_25() {
    TEST_ASSERT_EQUAL_INT(25, CHAR14_MAP_SIZE);
}

// ============================================================================
// main
// ============================================================================

int main(int argc, char** argv) {
    UNITY_BEGIN();

    RUN_TEST(test_segMetrics_digitH_is_twice_segLen_plus_3_segThick);
    RUN_TEST(test_segMetrics_spacing_equals_segThick);
    RUN_TEST(test_segMetrics_dot_equals_segThick);
    RUN_TEST(test_segMetrics_scale_0_clamps_segLen_to_2);
    RUN_TEST(test_segMetrics_scale_0_clamps_segThick_to_1);

    RUN_TEST(test_digit_2_has_correct_segments);
    RUN_TEST(test_digit_4_has_no_top_or_bottom_or_e_segs);
    RUN_TEST(test_digit_5_has_no_b_or_e_segs);
    RUN_TEST(test_all_10_digits_have_exactly_7_segment_values);

    RUN_TEST(test_14seg_all_uppercase_letters_A_through_E_nonzero);
    RUN_TEST(test_14seg_L_has_only_left_and_bottom_segs);
    RUN_TEST(test_14seg_dash_has_only_middle_segs);
    RUN_TEST(test_14seg_zero_has_outer_ring_only);
    RUN_TEST(test_14seg_lowercase_e_matches_uppercase_E);
    RUN_TEST(test_14seg_space_and_unknown_return_zero);
    RUN_TEST(test_char14_map_size_is_25);

    return UNITY_END();
}
