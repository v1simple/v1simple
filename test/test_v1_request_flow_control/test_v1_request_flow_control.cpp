#include <unity.h>

#include "../../src/v1_request_flow_control.h"

void setUp() {}
void tearDown() {}

void test_busy_replaces_the_held_set_and_releases_after_a_clean_display() {
    V1RequestFlowControl flow;
    const uint8_t first[] = {0x19, 0x16};
    flow.onBusy(first, 2);
    TEST_ASSERT_TRUE(flow.holds(0x19));
    TEST_ASSERT_TRUE(flow.holds(0x16));
    TEST_ASSERT_FALSE(flow.holds(0x22));

    flow.onDisplay(); // display immediately preceded by InfV1Busy
    TEST_ASSERT_TRUE(flow.holds(0x19));

    const uint8_t replacement[] = {0x22};
    flow.onBusy(replacement, 1);
    TEST_ASSERT_FALSE(flow.holds(0x19));
    TEST_ASSERT_TRUE(flow.holds(0x22));
    flow.onDisplay();
    TEST_ASSERT_TRUE(flow.holds(0x22));
    flow.onDisplay(); // no new InfV1Busy before this display
    TEST_ASSERT_FALSE(flow.holds(0x22));
}

void test_not_processed_retry_waits_for_a_display_without_preceding_busy() {
    V1RequestFlowControl flow;
    flow.onRequestNotProcessed(0x19);
    TEST_ASSERT_TRUE(flow.holds(0x19));
    TEST_ASSERT_FALSE(flow.consumeReleasedRetry(0x19));

    const uint8_t busy[] = {0x16};
    flow.onBusy(busy, 1);
    flow.onDisplay();
    TEST_ASSERT_TRUE(flow.holds(0x19));
    TEST_ASSERT_FALSE(flow.consumeReleasedRetry(0x19));

    flow.onDisplay();
    TEST_ASSERT_FALSE(flow.holds(0x19));
    TEST_ASSERT_TRUE(flow.consumeReleasedRetry(0x19));
    TEST_ASSERT_FALSE(flow.consumeReleasedRetry(0x19));
}

void test_reset_discards_all_session_scoped_state() {
    V1RequestFlowControl flow;
    const uint8_t busy[] = {0x01};
    flow.onBusy(busy, 1);
    flow.onRequestNotProcessed(0x11);
    flow.onDisplay();
    flow.onDisplay();
    TEST_ASSERT_TRUE(flow.consumeReleasedRetry(0x11));

    flow.onBusy(busy, 1);
    flow.onRequestNotProcessed(0x3C);
    flow.reset();
    TEST_ASSERT_FALSE(flow.holds(0x01));
    TEST_ASSERT_FALSE(flow.holds(0x3C));
    TEST_ASSERT_FALSE(flow.consumeReleasedRetry(0x3C));
}

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    UNITY_BEGIN();
    RUN_TEST(test_busy_replaces_the_held_set_and_releases_after_a_clean_display);
    RUN_TEST(test_not_processed_retry_waits_for_a_display_without_preceding_busy);
    RUN_TEST(test_reset_discards_all_session_scoped_state);
    return UNITY_END();
}
