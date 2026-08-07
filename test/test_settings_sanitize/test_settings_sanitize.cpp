#include <unity.h>

#include "../../src/settings_sanitize.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

void setUp() {}
void tearDown() {}

void test_slot_volume_pair_preserves_valid_atomic_pair() {
    uint8_t mainVolume = 7;
    uint8_t muteVolume = 2;
    sanitizeSlotVolumePair(mainVolume, muteVolume);
    TEST_ASSERT_EQUAL_UINT8(7, mainVolume);
    TEST_ASSERT_EQUAL_UINT8(2, muteVolume);
}

void test_slot_volume_pair_preserves_atomic_no_change() {
    uint8_t mainVolume = 0xFF;
    uint8_t muteVolume = 0xFF;
    sanitizeSlotVolumePair(mainVolume, muteVolume);
    TEST_ASSERT_EQUAL_UINT8(0xFF, mainVolume);
    TEST_ASSERT_EQUAL_UINT8(0xFF, muteVolume);
}

void test_slot_volume_pair_migrates_legacy_one_sided_values_to_no_change() {
    uint8_t mainVolume = 7;
    uint8_t muteVolume = 0xFF;
    sanitizeSlotVolumePair(mainVolume, muteVolume);
    TEST_ASSERT_EQUAL_UINT8(0xFF, mainVolume);
    TEST_ASSERT_EQUAL_UINT8(0xFF, muteVolume);

    mainVolume = 0xFF;
    muteVolume = 2;
    sanitizeSlotVolumePair(mainVolume, muteVolume);
    TEST_ASSERT_EQUAL_UINT8(0xFF, mainVolume);
    TEST_ASSERT_EQUAL_UINT8(0xFF, muteVolume);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_slot_volume_pair_preserves_valid_atomic_pair);
    RUN_TEST(test_slot_volume_pair_preserves_atomic_no_change);
    RUN_TEST(test_slot_volume_pair_migrates_legacy_one_sided_values_to_no_change);
    return UNITY_END();
}
