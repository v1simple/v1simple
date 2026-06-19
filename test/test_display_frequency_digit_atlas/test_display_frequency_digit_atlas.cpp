#include <unity.h>

#include "../mocks/Arduino.h"
#include "../mocks/esp_heap_caps.h"
#include "../mocks/mock_heap_caps_state.h"
#include "../../include/display_frequency_digit_atlas.h"
#include "../../src/display_frequency_digit_atlas.cpp"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

void setUp() {
    mock_reset_heap_caps();
}

void tearDown() {}

namespace {

void storeAllCells(DisplayFrequencyDigitAtlas& atlas,
                   uint16_t* framebuffer,
                   int rawStride,
                   int rows,
                   uint16_t bg) {
    for (int i = 0; i < rawStride * rows; ++i) {
        framebuffer[i] = 0xFFFF;
    }

    for (uint8_t pos = 0; pos < DisplayFrequencyDigitAtlas::kTextPositions; ++pos) {
        if (pos == 2) {
            TEST_ASSERT_TRUE(atlas.storeCell(pos, '.', 1, 2, 4, 3, framebuffer, rawStride, bg));
            continue;
        }
        for (char digit = '0'; digit <= '9'; ++digit) {
            TEST_ASSERT_TRUE(atlas.storeCell(pos, digit, 1, 2, 4, 3, framebuffer, rawStride, bg));
        }
    }
}

void storePositionedCells(DisplayFrequencyDigitAtlas& atlas,
                          uint16_t* framebuffer,
                          int rawStride,
                          int rows,
                          uint16_t bg) {
    for (int i = 0; i < rawStride * rows; ++i) {
        framebuffer[i] = 0xFFFF;
    }

    for (uint8_t pos = 0; pos < DisplayFrequencyDigitAtlas::kTextPositions; ++pos) {
        const int16_t x = static_cast<int16_t>(1 + pos * 6);
        if (pos == 2) {
            TEST_ASSERT_TRUE(atlas.storeCell(pos, '.', x, 2, 4, 3, framebuffer, rawStride, bg));
            continue;
        }
        for (char digit = '0'; digit <= '9'; ++digit) {
            TEST_ASSERT_TRUE(atlas.storeCell(pos, digit, x, 2, 4, 3, framebuffer, rawStride, bg));
        }
    }
}

uint16_t logicalPixel(const uint16_t* framebuffer, int rawStride, int x, int y) {
    return framebuffer[x * rawStride + (rawStride - 1 - y)];
}

}  // namespace

void test_begin_allocates_bounded_alpha_pool() {
    DisplayFrequencyDigitAtlas atlas;

    TEST_ASSERT_TRUE(atlas.begin(4, 3));

    TEST_ASSERT_TRUE(atlas.enabled());
    TEST_ASSERT_EQUAL_UINT32(4u * 3u * DisplayFrequencyDigitAtlas::kSlotCount, atlas.bytes());
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_TRUE((g_mock_heap_caps_last_malloc_caps & MALLOC_CAP_SPIRAM) != 0);
    atlas.release();
}

void test_numeric_text_validation_accepts_only_dd_dot_ddd() {
    TEST_ASSERT_TRUE(DisplayFrequencyDigitAtlas::isNumericFrequencyText("35.496"));
    TEST_ASSERT_FALSE(DisplayFrequencyDigitAtlas::isNumericFrequencyText("LASER"));
    TEST_ASSERT_FALSE(DisplayFrequencyDigitAtlas::isNumericFrequencyText("--.---"));
    TEST_ASSERT_FALSE(DisplayFrequencyDigitAtlas::isNumericFrequencyText("135.49"));
}

void test_restore_misses_until_all_numeric_cells_are_present() {
    DisplayFrequencyDigitAtlas atlas;
    TEST_ASSERT_TRUE(atlas.begin(4, 3));

    constexpr int kRawStride = 8;
    constexpr int kRows = 8;
    uint16_t framebuffer[kRawStride * kRows];
    for (uint16_t& px : framebuffer) {
        px = 0xFFFF;
    }

    TEST_ASSERT_TRUE(atlas.storeCell(0, '3', 1, 2, 4, 3, framebuffer, kRawStride, 0x0000));
    TEST_ASSERT_FALSE(atlas.ready());
    TEST_ASSERT_FALSE(atlas.restoreText("35.496", 0xF800, 0x0000, framebuffer, kRawStride));
    TEST_ASSERT_EQUAL_UINT32(1u, atlas.missCount());
    atlas.release();
}

void test_restore_text_uses_rotated_geometry_and_colorizes_alpha() {
    DisplayFrequencyDigitAtlas atlas;
    TEST_ASSERT_TRUE(atlas.begin(4, 3));

    constexpr int kRawStride = 8;
    constexpr int kRows = 8;
    constexpr uint16_t kBg = 0x0000;
    constexpr uint16_t kFg = 0xF800;
    uint16_t framebuffer[kRawStride * kRows];

    storeAllCells(atlas, framebuffer, kRawStride, kRows, kBg);
    TEST_ASSERT_TRUE(atlas.ready());

    for (uint16_t& px : framebuffer) {
        px = kBg;
    }

    TEST_ASSERT_TRUE(atlas.restoreText("35.496", kFg, kBg, framebuffer, kRawStride));
    TEST_ASSERT_EQUAL_UINT32(1u, atlas.hitCount());

    // Logical x=1,y=2,w=4,h=3 maps to physical x=3,y=1,w=3,h=4.
    for (int row = 1; row < 5; ++row) {
        for (int col = 3; col < 6; ++col) {
            TEST_ASSERT_EQUAL_UINT16(kFg, framebuffer[row * kRawStride + col]);
        }
    }
    atlas.release();
}

void test_restore_reuses_blend_lut_until_color_changes() {
    DisplayFrequencyDigitAtlas atlas;
    TEST_ASSERT_TRUE(atlas.begin(4, 3));

    constexpr int kRawStride = 8;
    constexpr int kRows = 8;
    constexpr uint16_t kBg = 0x0000;
    uint16_t framebuffer[kRawStride * kRows];

    storeAllCells(atlas, framebuffer, kRawStride, kRows, kBg);
    TEST_ASSERT_TRUE(atlas.ready());

    for (uint16_t& px : framebuffer) {
        px = kBg;
    }

    TEST_ASSERT_TRUE(atlas.restoreText("35.496", 0xF800, kBg, framebuffer, kRawStride));
    TEST_ASSERT_EQUAL_UINT32(1u, atlas.blendLutBuildCount());

    TEST_ASSERT_TRUE(atlas.restoreText("35.548", 0xF800, kBg, framebuffer, kRawStride));
    TEST_ASSERT_EQUAL_UINT32(1u, atlas.blendLutBuildCount());

    TEST_ASSERT_TRUE(atlas.restoreText("35.548", 0x07E0, kBg, framebuffer, kRawStride));
    TEST_ASSERT_EQUAL_UINT32(2u, atlas.blendLutBuildCount());
    atlas.release();
}

void test_changed_text_rect_unions_old_and_new_digit_cells() {
    DisplayFrequencyDigitAtlas atlas;
    TEST_ASSERT_TRUE(atlas.begin(4, 3));

    constexpr int kRawStride = 64;
    constexpr int kRows = 64;
    constexpr uint16_t kBg = 0x0000;
    uint16_t framebuffer[kRawStride * kRows];

    storePositionedCells(atlas, framebuffer, kRawStride, kRows, kBg);
    TEST_ASSERT_TRUE(atlas.ready());

    for (uint16_t& px : framebuffer) {
        px = 0xFFFF;
    }
    TEST_ASSERT_TRUE(atlas.storeCell(4, '4', 25, 2, 2, 3, framebuffer, kRawStride, kBg));
    TEST_ASSERT_TRUE(atlas.storeCell(4, '5', 28, 2, 3, 3, framebuffer, kRawStride, kBg));

    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;
    TEST_ASSERT_TRUE(atlas.changedTextRect("35.446", "35.456", x, y, w, h));
    TEST_ASSERT_EQUAL_INT16(25, x);
    TEST_ASSERT_EQUAL_INT16(2, y);
    TEST_ASSERT_EQUAL_INT16(6, w);
    TEST_ASSERT_EQUAL_INT16(3, h);
    atlas.release();
}

void test_changed_text_rect_rejects_non_numeric_text() {
    DisplayFrequencyDigitAtlas atlas;
    TEST_ASSERT_TRUE(atlas.begin(4, 3));

    int16_t x = 0;
    int16_t y = 0;
    int16_t w = 0;
    int16_t h = 0;
    TEST_ASSERT_FALSE(atlas.changedTextRect("LASER", "35.456", x, y, w, h));
    atlas.release();
}

void test_restore_text_in_rect_only_repaints_intersecting_cells() {
    DisplayFrequencyDigitAtlas atlas;
    TEST_ASSERT_TRUE(atlas.begin(4, 3));

    constexpr int kRawStride = 64;
    constexpr int kRows = 64;
    constexpr uint16_t kBg = 0x0000;
    constexpr uint16_t kFg = 0x07E0;
    uint16_t framebuffer[kRawStride * kRows];

    storePositionedCells(atlas, framebuffer, kRawStride, kRows, kBg);
    TEST_ASSERT_TRUE(atlas.ready());

    for (uint16_t& px : framebuffer) {
        px = kBg;
    }

    TEST_ASSERT_TRUE(atlas.restoreTextInRect("35.496",
                                            kFg,
                                            kBg,
                                            framebuffer,
                                            kRawStride,
                                            1,
                                            2,
                                            4,
                                            3));

    for (int x = 1; x < 5; ++x) {
        for (int y = 2; y < 5; ++y) {
            TEST_ASSERT_EQUAL_UINT16(kFg, logicalPixel(framebuffer, kRawStride, x, y));
        }
    }
    for (int x = 7; x < 11; ++x) {
        for (int y = 2; y < 5; ++y) {
            TEST_ASSERT_EQUAL_UINT16(kBg, logicalPixel(framebuffer, kRawStride, x, y));
        }
    }
    atlas.release();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_begin_allocates_bounded_alpha_pool);
    RUN_TEST(test_numeric_text_validation_accepts_only_dd_dot_ddd);
    RUN_TEST(test_restore_misses_until_all_numeric_cells_are_present);
    RUN_TEST(test_restore_text_uses_rotated_geometry_and_colorizes_alpha);
    RUN_TEST(test_restore_reuses_blend_lut_until_color_changes);
    RUN_TEST(test_changed_text_rect_unions_old_and_new_digit_cells);
    RUN_TEST(test_changed_text_rect_rejects_non_numeric_text);
    RUN_TEST(test_restore_text_in_rect_only_repaints_intersecting_cells);
    return UNITY_END();
}
