#include <unity.h>

#include "../../src/modules/system/system_event_bus.h"

void setUp() {}
void tearDown() {}

void test_alp_display_edge_latches_until_consumed() {
    SystemEventBus bus;

    TEST_ASSERT_EQUAL_UINT8(0, bus.size());
    TEST_ASSERT_FALSE(bus.consumeAlpStateChanged());

    bus.publishAlpStateChanged();

    TEST_ASSERT_EQUAL_UINT8(1, bus.size());
    TEST_ASSERT_EQUAL_UINT32(1, bus.getPublishCount());
    TEST_ASSERT_TRUE(bus.consumeAlpStateChanged());
    TEST_ASSERT_FALSE(bus.consumeAlpStateChanged());
}

void test_alp_display_edges_coalesce() {
    SystemEventBus bus;

    bus.publishAlpStateChanged();
    bus.publishAlpStateChanged();
    bus.publishAlpStateChanged();

    TEST_ASSERT_EQUAL_UINT32(3, bus.getPublishCount());
    TEST_ASSERT_EQUAL_UINT8(1, bus.size());
    TEST_ASSERT_TRUE(bus.consumeAlpStateChanged());
    TEST_ASSERT_EQUAL_UINT8(0, bus.size());
}

void test_reset_stats_preserves_pending_edge() {
    SystemEventBus bus;
    bus.publishAlpStateChanged();

    bus.resetStats();

    TEST_ASSERT_EQUAL_UINT32(0, bus.getPublishCount());
    TEST_ASSERT_EQUAL_UINT8(1, bus.size());
    TEST_ASSERT_TRUE(bus.consumeAlpStateChanged());
}

void test_reset_clears_pending_edge_and_stats() {
    SystemEventBus bus;
    bus.publishAlpStateChanged();

    bus.reset();

    TEST_ASSERT_EQUAL_UINT32(0, bus.getPublishCount());
    TEST_ASSERT_EQUAL_UINT8(0, bus.size());
    TEST_ASSERT_FALSE(bus.consumeAlpStateChanged());
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_alp_display_edge_latches_until_consumed);
    RUN_TEST(test_alp_display_edges_coalesce);
    RUN_TEST(test_reset_stats_preserves_pending_edge);
    RUN_TEST(test_reset_clears_pending_edge_and_stats);
    return UNITY_END();
}
