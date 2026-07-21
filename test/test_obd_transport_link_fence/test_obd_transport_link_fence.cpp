#include <unity.h>

#include "../../src/modules/obd/obd_transport_link_fence.h"

void setUp() {}
void tearDown() {}

void test_fence_starts_clear_and_empty_consume_preserves_output() {
    ObdTransportLinkFence fence;
    uint32_t generation = 91;
    int reason = 73;

    TEST_ASSERT_FALSE(fence.pending());
    TEST_ASSERT_FALSE(fence.consume(generation, reason));
    TEST_ASSERT_EQUAL_UINT32(91, generation);
    TEST_ASSERT_EQUAL(73, reason);
    TEST_ASSERT_FALSE(fence.pending());
}

void test_publish_is_consumed_once() {
    ObdTransportLinkFence fence;
    uint32_t generation = 0;
    int reason = 0;

    fence.publish(7, 534);

    TEST_ASSERT_TRUE(fence.pending());
    TEST_ASSERT_TRUE(fence.consume(generation, reason));
    TEST_ASSERT_EQUAL_UINT32(7, generation);
    TEST_ASSERT_EQUAL(534, reason);
    TEST_ASSERT_FALSE(fence.pending());
    TEST_ASSERT_FALSE(fence.consume(generation, reason));
    TEST_ASSERT_EQUAL(534, reason);
}

void test_latest_reason_wins_before_consume() {
    ObdTransportLinkFence fence;
    uint32_t generation = 0;
    int reason = 0;

    fence.publish(1, 8);
    fence.publish(2, 19);
    fence.publish(3, 62);

    TEST_ASSERT_TRUE(fence.pending());
    TEST_ASSERT_TRUE(fence.consume(generation, reason));
    TEST_ASSERT_EQUAL_UINT32(3, generation);
    TEST_ASSERT_EQUAL(62, reason);
    TEST_ASSERT_FALSE(fence.pending());
}

void test_reason_value_is_not_used_as_pending_sentinel() {
    ObdTransportLinkFence fence;
    uint32_t generation = 0;
    int reason = 99;

    fence.publish(4, 0);
    TEST_ASSERT_TRUE(fence.consume(generation, reason));
    TEST_ASSERT_EQUAL_UINT32(4, generation);
    TEST_ASSERT_EQUAL(0, reason);

    fence.publish(5, -7);
    TEST_ASSERT_TRUE(fence.consume(generation, reason));
    TEST_ASSERT_EQUAL_UINT32(5, generation);
    TEST_ASSERT_EQUAL(-7, reason);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_fence_starts_clear_and_empty_consume_preserves_output);
    RUN_TEST(test_publish_is_consumed_once);
    RUN_TEST(test_latest_reason_wins_before_consume);
    RUN_TEST(test_reason_value_is_not_used_as_pending_sentinel);
    return UNITY_END();
}
