#include <unity.h>
#include <array>
#include <cstdint>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../include/v1simple_logo.h"

static uint32_t crc32_update(uint32_t crc, uint8_t byte) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i) {
        crc = (crc >> 1) ^ ((crc & 1U) ? 0xEDB88320U : 0U);
    }
    return crc;
}

void test_logo_row_decode_crc_matches_reference() {
    std::array<uint16_t, V1SIMPLE_LOGO_WIDTH> row{};
    uint32_t crc = 0xFFFFFFFFU;
    uint32_t pixelCount = 0;

    for (uint16_t y = 0; y < V1SIMPLE_LOGO_HEIGHT; ++y) {
        decodeV1SimpleLogoRow(y, row.data(), row.size());
        for (uint16_t x = 0; x < V1SIMPLE_LOGO_WIDTH; ++x) {
            const uint16_t pixel = row[x];
            crc = crc32_update(crc, static_cast<uint8_t>(pixel & 0xFFU));
            crc = crc32_update(crc, static_cast<uint8_t>(pixel >> 8));
            ++pixelCount;
        }
    }

    crc ^= 0xFFFFFFFFU;

    TEST_ASSERT_EQUAL_UINT32(V1SIMPLE_LOGO_PIXEL_COUNT, pixelCount);
    TEST_ASSERT_EQUAL_HEX32(V1SIMPLE_LOGO_RAW_CRC32, crc);
}

void test_logo_row_decode_accepts_full_width_buffer() {
    std::array<uint16_t, V1SIMPLE_LOGO_WIDTH> row{};
    decodeV1SimpleLogoRow(0, row.data(), row.size());

    bool hasNonZero = false;
    for (uint16_t pixel : row) {
        if (pixel != 0U) {
            hasNonZero = true;
            break;
        }
    }

    TEST_ASSERT_TRUE(hasNonZero);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_logo_row_decode_crc_matches_reference);
    RUN_TEST(test_logo_row_decode_accepts_full_width_buffer);
    return UNITY_END();
}
