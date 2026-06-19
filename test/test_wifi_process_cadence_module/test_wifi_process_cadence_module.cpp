#include <unity.h>

#include "../mocks/Arduino.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

#include "../../src/modules/wifi/wifi_process_cadence_module.cpp"

static WifiProcessCadenceModule module;

static WifiProcessCadenceContext makeContext(uint32_t nowProcessUs, uint32_t minIntervalUs) {
    WifiProcessCadenceContext ctx;
    ctx.nowProcessUs = nowProcessUs;
    ctx.minIntervalUs = minIntervalUs;
    return ctx;
}

void setUp() {
    module.reset();
}

void tearDown() {}

void test_first_run_and_interval_gating() {
    auto first = module.process(makeContext(1000, 2000));
    TEST_ASSERT_TRUE(first.shouldRunProcess);

    auto early = module.process(makeContext(2500, 2000));
    TEST_ASSERT_FALSE(early.shouldRunProcess);

    auto justBefore = module.process(makeContext(2999, 2000));
    TEST_ASSERT_FALSE(justBefore.shouldRunProcess);

    auto atInterval = module.process(makeContext(3000, 2000));
    TEST_ASSERT_TRUE(atInterval.shouldRunProcess);
}

void test_custom_interval_is_respected() {
    auto first = module.process(makeContext(100, 500));
    TEST_ASSERT_TRUE(first.shouldRunProcess);

    auto tooSoon = module.process(makeContext(599, 500));
    TEST_ASSERT_FALSE(tooSoon.shouldRunProcess);

    auto due = module.process(makeContext(600, 500));
    TEST_ASSERT_TRUE(due.shouldRunProcess);
}

void test_wrap_safe_elapsed_calculation() {
    auto prime = module.process(makeContext(0xFFFFFF00u, 200));
    TEST_ASSERT_TRUE(prime.shouldRunProcess);

    auto wrappedDue = module.process(makeContext(0x00000020u, 200));
    TEST_ASSERT_TRUE(wrappedDue.shouldRunProcess);
}

void test_wrap_safe_skip_until_interval_reached() {
    auto prime = module.process(makeContext(0xFFFFFFF0u, 64));
    TEST_ASSERT_TRUE(prime.shouldRunProcess);

    auto wrappedEarly = module.process(makeContext(0x00000010u, 64));
    TEST_ASSERT_FALSE(wrappedEarly.shouldRunProcess);

    auto wrappedDue = module.process(makeContext(0x00000030u, 64));
    TEST_ASSERT_TRUE(wrappedDue.shouldRunProcess);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_first_run_and_interval_gating);
    RUN_TEST(test_custom_interval_is_respected);
    RUN_TEST(test_wrap_safe_elapsed_calculation);
    RUN_TEST(test_wrap_safe_skip_until_interval_reached);
    return UNITY_END();
}
