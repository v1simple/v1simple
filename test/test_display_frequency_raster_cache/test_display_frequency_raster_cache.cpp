#include <unity.h>

#include <cstring>

#include "../mocks/Arduino.h"
#include "../mocks/esp_heap_caps.h"
#include "../mocks/mock_heap_caps_state.h"
#include "../../include/display_frequency_raster_cache.h"
#include "../../src/display_frequency_raster_cache.cpp"

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

FrequencyRasterKey makeKey() {
    FrequencyRasterKey key{};
    std::strncpy(key.text, "34.700", sizeof(key.text));
    key.color = 0x07E0;
    key.bg = 0x0000;
    key.fontSize = 82;
    key.paletteRevision = 3;
    key.x = 1;
    key.y = 2;
    key.w = 4;
    key.h = 3;
    key.flags = 0x01;
    return key;
}

}  // namespace

void test_begin_allocates_fixed_psram_pool() {
    DisplayFrequencyRasterCache cache;

    TEST_ASSERT_TRUE(cache.begin(4, 3, 2));

    TEST_ASSERT_TRUE(cache.enabled());
    TEST_ASSERT_EQUAL_UINT8(2, cache.capacity());
    TEST_ASSERT_EQUAL_UINT32(4u * 3u * 2u * sizeof(uint16_t), cache.bytes());
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_heap_caps_malloc_calls);
    TEST_ASSERT_TRUE((g_mock_heap_caps_last_malloc_caps & MALLOC_CAP_SPIRAM) != 0);
    cache.release();
}

void test_begin_failure_disables_cache() {
    DisplayFrequencyRasterCache cache;
    g_mock_heap_caps_fail_malloc = true;

    TEST_ASSERT_FALSE(cache.begin(4, 3, 2));

    TEST_ASSERT_FALSE(cache.enabled());
    TEST_ASSERT_EQUAL_UINT8(0, cache.capacity());
    TEST_ASSERT_EQUAL_UINT32(0u, cache.bytes());
    cache.release();
}

void test_store_and_restore_use_rotated_canvas_geometry() {
    DisplayFrequencyRasterCache cache;
    TEST_ASSERT_TRUE(cache.begin(4, 3, 2));

    constexpr int kRawStride = 8;
    constexpr int kRows = 8;
    uint16_t framebuffer[kRawStride * kRows];
    for (int row = 0; row < kRows; ++row) {
        for (int col = 0; col < kRawStride; ++col) {
            framebuffer[row * kRawStride + col] = static_cast<uint16_t>(row * 100 + col);
        }
    }

    const FrequencyRasterKey key = makeKey();
    TEST_ASSERT_TRUE(cache.store(key, framebuffer, kRawStride));
    TEST_ASSERT_EQUAL_UINT32(1u, cache.storeCount());

    // Logical x=1,y=2,w=4,h=3 maps to physical x=3,y=1,w=3,h=4.
    for (int row = 1; row < 5; ++row) {
        for (int col = 3; col < 6; ++col) {
            framebuffer[row * kRawStride + col] = 0xEEEE;
        }
    }

    TEST_ASSERT_TRUE(cache.restore(key, framebuffer, kRawStride));
    TEST_ASSERT_EQUAL_UINT32(1u, cache.hitCount());

    for (int row = 1; row < 5; ++row) {
        for (int col = 3; col < 6; ++col) {
            TEST_ASSERT_EQUAL_UINT16(static_cast<uint16_t>(row * 100 + col),
                                     framebuffer[row * kRawStride + col]);
        }
    }
    cache.release();
}

void test_restore_miss_does_not_touch_framebuffer() {
    DisplayFrequencyRasterCache cache;
    TEST_ASSERT_TRUE(cache.begin(4, 3, 1));

    constexpr int kRawStride = 8;
    uint16_t framebuffer[kRawStride * kRawStride];
    for (uint16_t& px : framebuffer) {
        px = 0x1234;
    }

    FrequencyRasterKey key = makeKey();
    std::strncpy(key.text, "35.500", sizeof(key.text));

    TEST_ASSERT_FALSE(cache.restore(key, framebuffer, kRawStride));
    TEST_ASSERT_EQUAL_UINT32(1u, cache.missCount());
    for (const uint16_t px : framebuffer) {
        TEST_ASSERT_EQUAL_UINT16(0x1234, px);
    }
    cache.release();
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_begin_allocates_fixed_psram_pool);
    RUN_TEST(test_begin_failure_disables_cache);
    RUN_TEST(test_store_and_restore_use_rotated_canvas_geometry);
    RUN_TEST(test_restore_miss_does_not_touch_framebuffer);
    return UNITY_END();
}
