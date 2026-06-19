#include <unity.h>

#include "../../include/wifi_rate_limiter.h"

void setUp(void) {}
void tearDown(void) {}

void test_first_window_of_requests_is_allowed() {
    SlidingWindowRateLimiter limiter;

    for (size_t i = 0; i < SlidingWindowRateLimiter::MAX_REQUESTS; ++i) {
        const SlidingWindowRateLimitDecision decision = limiter.evaluate(1000);
        TEST_ASSERT_TRUE(decision.allowed);
        TEST_ASSERT_EQUAL_UINT32(i + 1, decision.requestCount);
        TEST_ASSERT_EQUAL_UINT32(0, decision.retryAfterMs);
    }
}

void test_request_after_capacity_is_rate_limited() {
    SlidingWindowRateLimiter limiter;
    for (size_t i = 0; i < SlidingWindowRateLimiter::MAX_REQUESTS; ++i) {
        limiter.evaluate(1000);
    }

    const SlidingWindowRateLimitDecision decision = limiter.evaluate(1001);
    TEST_ASSERT_FALSE(decision.allowed);
    TEST_ASSERT_EQUAL_UINT32(SlidingWindowRateLimiter::MAX_REQUESTS, decision.requestCount);
    TEST_ASSERT_EQUAL_UINT32(SlidingWindowRateLimiter::WINDOW_MS - 1, decision.retryAfterMs);
}

void test_boundary_burst_is_blocked_across_window_edge() {
    SlidingWindowRateLimiter limiter;
    for (size_t i = 0; i < SlidingWindowRateLimiter::MAX_REQUESTS; ++i) {
        limiter.evaluate(60000);
    }

    const SlidingWindowRateLimitDecision decision = limiter.evaluate(60001);
    TEST_ASSERT_FALSE(decision.allowed);
    TEST_ASSERT_EQUAL_UINT32(SlidingWindowRateLimiter::WINDOW_MS - 1, decision.retryAfterMs);
}

void test_request_is_allowed_when_oldest_timestamp_expires() {
    SlidingWindowRateLimiter limiter;
    for (size_t i = 0; i < SlidingWindowRateLimiter::MAX_REQUESTS; ++i) {
        limiter.evaluate(1000);
    }

    const SlidingWindowRateLimitDecision decision = limiter.evaluate(
        1000 + SlidingWindowRateLimiter::WINDOW_MS);
    TEST_ASSERT_TRUE(decision.allowed);
    TEST_ASSERT_EQUAL_UINT32(1, decision.requestCount);
    TEST_ASSERT_EQUAL_UINT32(0, decision.retryAfterMs);
}

void test_retry_after_uses_oldest_retained_timestamp() {
    SlidingWindowRateLimiter limiter;
    for (size_t i = 0; i < SlidingWindowRateLimiter::MAX_REQUESTS / 2; ++i) {
        limiter.evaluate(1000);
        limiter.evaluate(5000);
    }

    const SlidingWindowRateLimitDecision decision = limiter.evaluate(7000);
    TEST_ASSERT_FALSE(decision.allowed);
    TEST_ASSERT_EQUAL_UINT32(54000, decision.retryAfterMs);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_first_window_of_requests_is_allowed);
    RUN_TEST(test_request_after_capacity_is_rate_limited);
    RUN_TEST(test_boundary_burst_is_blocked_across_window_edge);
    RUN_TEST(test_request_is_allowed_when_oldest_timestamp_expires);
    RUN_TEST(test_retry_after_uses_oldest_retained_timestamp);
    return UNITY_END();
}
