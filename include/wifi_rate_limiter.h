#pragma once

#include <cstddef>
#include <cstdint>

struct SlidingWindowRateLimitDecision {
    bool allowed = true;
    uint32_t retryAfterMs = 0;
    size_t requestCount = 0;
};

class SlidingWindowRateLimiter {
public:
    static constexpr uint32_t WINDOW_MS = 60000;
    static constexpr size_t MAX_REQUESTS = 120;

    SlidingWindowRateLimiter() = default;

    void reset() {
        head_ = 0;
        count_ = 0;
        for (size_t i = 0; i < MAX_REQUESTS; ++i) {
            timestamps_[i] = 0;
        }
    }

    SlidingWindowRateLimitDecision evaluate(uint32_t nowMs) {
        evictExpired(nowMs);

        SlidingWindowRateLimitDecision decision;
        decision.requestCount = count_;
        if (count_ >= MAX_REQUESTS) {
            decision.allowed = false;
            decision.retryAfterMs = WINDOW_MS - elapsedMs(nowMs, timestamps_[head_]);
            return decision;
        }

        const size_t insertIndex = (head_ + count_) % MAX_REQUESTS;
        timestamps_[insertIndex] = nowMs;
        ++count_;
        decision.requestCount = count_;
        return decision;
    }

private:
    static uint32_t elapsedMs(uint32_t nowMs, uint32_t startMs) {
        return static_cast<uint32_t>(nowMs - startMs);
    }

    void evictExpired(uint32_t nowMs) {
        while (count_ > 0 && elapsedMs(nowMs, timestamps_[head_]) >= WINDOW_MS) {
            head_ = (head_ + 1) % MAX_REQUESTS;
            --count_;
        }
    }

    uint32_t timestamps_[MAX_REQUESTS] = {0};
    size_t head_ = 0;
    size_t count_ = 0;
};

