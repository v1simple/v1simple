/**
 * Focused tests for the production settings sanitizers.
 *
 * Stateful WiFi-mode and password-obfuscation behavior is covered by
 * test_settings_persistence, which compiles the owning production sources.
 */

#include <unity.h>

#include "../../src/settings_sanitize.h"

void test_clamp_u8_respects_bounds() {
    TEST_ASSERT_EQUAL_UINT8(1, clampU8(0, 1, 255));
    TEST_ASSERT_EQUAL_UINT8(1, clampU8(1, 1, 255));
    TEST_ASSERT_EQUAL_UINT8(128, clampU8(128, 1, 255));
    TEST_ASSERT_EQUAL_UINT8(255, clampU8(255, 1, 255));
    TEST_ASSERT_EQUAL_UINT8(255, clampU8(300, 1, 255));
}

void test_slot_volume_clamp_preserves_no_change_sentinel() {
    TEST_ASSERT_EQUAL_UINT8(0, clampSlotVolumeValue(-1));
    TEST_ASSERT_EQUAL_UINT8(0, clampSlotVolumeValue(0));
    TEST_ASSERT_EQUAL_UINT8(9, clampSlotVolumeValue(9));
    TEST_ASSERT_EQUAL_UINT8(9, clampSlotVolumeValue(10));
    TEST_ASSERT_EQUAL_UINT8(0xFF, clampSlotVolumeValue(0xFF));
}

void test_slot_volume_pair_rejects_one_sided_legacy_values() {
    uint8_t volume = 7;
    uint8_t muteVolume = 0xFF;

    sanitizeSlotVolumePair(volume, muteVolume);

    TEST_ASSERT_EQUAL_UINT8(0xFF, volume);
    TEST_ASSERT_EQUAL_UINT8(0xFF, muteVolume);
}

void test_ap_timeout_clamp_preserves_disabled_and_bounds_enabled_values() {
    TEST_ASSERT_EQUAL_UINT8(0, clampApTimeoutValue(0));
    TEST_ASSERT_EQUAL_UINT8(5, clampApTimeoutValue(1));
    TEST_ASSERT_EQUAL_UINT8(30, clampApTimeoutValue(30));
    TEST_ASSERT_EQUAL_UINT8(60, clampApTimeoutValue(90));
}

void setUp(void) {}
void tearDown(void) {}

void runAllTests() {
    RUN_TEST(test_clamp_u8_respects_bounds);
    RUN_TEST(test_slot_volume_clamp_preserves_no_change_sentinel);
    RUN_TEST(test_slot_volume_pair_rejects_one_sided_legacy_values);
    RUN_TEST(test_ap_timeout_clamp_preserves_disabled_and_bounds_enabled_values);
}

#ifdef ARDUINO
void setup() {
    delay(2000);
    UNITY_BEGIN();
    runAllTests();
    UNITY_END();
}
void loop() {}
#else
int main(int argc, char** argv) {
    UNITY_BEGIN();
    runAllTests();
    return UNITY_END();
}
#endif
