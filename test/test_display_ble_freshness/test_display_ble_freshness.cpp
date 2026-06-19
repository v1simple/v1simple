#include <unity.h>

#include "../../include/display_ble_freshness.h"

void setUp() {}
void tearDown() {}

void test_ble_context_is_fresh_at_update_time() {
    TEST_ASSERT_TRUE(DisplayBleFreshness::isFresh(1000, 1000));
    TEST_ASSERT_TRUE(DisplayBleFreshness::isFresh(1000, 2000));
}

void test_ble_context_is_stale_after_timeout() {
    TEST_ASSERT_FALSE(DisplayBleFreshness::isFresh(1000, 2001));
    TEST_ASSERT_FALSE(DisplayBleFreshness::isFresh(0, 2000));
}

void test_ble_context_freshness_uses_wraparound_safe_unsigned_math() {
    const uint32_t updatedAt = 0xFFFFFFF0u;
    const uint32_t shortlyAfterWrap = 0x00000020u;  // 48 ms later
    const uint32_t longAfterWrap = 0x00000420u;     // 1072 ms later

    TEST_ASSERT_TRUE(DisplayBleFreshness::isFresh(updatedAt, shortlyAfterWrap));
    TEST_ASSERT_FALSE(DisplayBleFreshness::isFresh(updatedAt, longAfterWrap));
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_ble_context_is_fresh_at_update_time);
    RUN_TEST(test_ble_context_is_stale_after_timeout);
    RUN_TEST(test_ble_context_freshness_uses_wraparound_safe_unsigned_math);
    return UNITY_END();
}
