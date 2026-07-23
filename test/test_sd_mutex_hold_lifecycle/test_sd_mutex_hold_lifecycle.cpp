#include <unity.h>

#include <atomic>
#include <climits>
#include <mutex>
#include <thread>

#ifndef V1_LINKED_TEST_SD_MUTEX_HOLD_LIFECYCLE
#include "../../src/modules/storage/sd_mutex_hold_lifecycle.cpp"
#else
#include "../../src/modules/storage/sd_mutex_hold_lifecycle.h"
#endif

namespace {
struct Fixture {
    std::mutex mutex;
    std::thread::id acquireThread{};
    std::thread::id releaseThread{};
    std::atomic<uint32_t> attempts{0};
    std::atomic<uint32_t> releases{0};
};

bool tryAcquire(void* context) noexcept {
    auto& fixture = *static_cast<Fixture*>(context);
    fixture.attempts.fetch_add(1);
    if (!fixture.mutex.try_lock()) {
        return false;
    }
    fixture.acquireThread = std::this_thread::get_id();
    return true;
}

void release(void* context) noexcept {
    auto& fixture = *static_cast<Fixture*>(context);
    fixture.releaseThread = std::this_thread::get_id();
    fixture.releases.fetch_add(1);
    fixture.mutex.unlock();
}
} // namespace

void setUp() {}
void tearDown() {}

void test_real_contention_waits_without_blocking_then_releases_on_owner_thread() {
    Fixture fixture;
    fixture.mutex.lock();
    std::atomic<bool> acquired{false};
    std::atomic<bool> finish{false};
    std::atomic<uint32_t> holderResult{0};

    std::thread holder([&]() {
        SdMutexHoldLifecycle lifecycle({tryAcquire, release, &fixture});
        if (!lifecycle.begin(100, 1000)) {
            holderResult.store(1);
            return;
        }
        while (!acquired.load()) {
            const SdMutexHoldStep step = lifecycle.step(100, true);
            if (step == SdMutexHoldStep::Acquired) {
                acquired.store(true);
            } else if (step != SdMutexHoldStep::Waiting) {
                holderResult.store(2);
                return;
            }
            std::this_thread::yield();
        }
        while (!finish.load()) {
            if (lifecycle.step(101, true) != SdMutexHoldStep::Holding) {
                holderResult.store(3);
                return;
            }
            std::this_thread::yield();
        }
        if (lifecycle.step(102, false) != SdMutexHoldStep::Released) {
            holderResult.store(4);
        }
    });

    while (fixture.attempts.load() < 2u) {
        std::this_thread::yield();
    }
    TEST_ASSERT_FALSE(acquired.load());
    fixture.mutex.unlock();
    while (!acquired.load()) {
        std::this_thread::yield();
    }
    finish.store(true);
    holder.join();

    TEST_ASSERT_EQUAL_UINT32(0u, holderResult.load());
    TEST_ASSERT_EQUAL_UINT32(1u, fixture.releases.load());
    TEST_ASSERT_TRUE(fixture.acquireThread == fixture.releaseThread);
    TEST_ASSERT_TRUE(fixture.mutex.try_lock());
    fixture.mutex.unlock();
}

void test_acquisition_deadline_is_rollover_safe_and_never_releases_unowned_mutex() {
    Fixture fixture;
    fixture.mutex.lock();
    SdMutexHoldLifecycle lifecycle({tryAcquire, release, &fixture});
    const uint32_t start = UINT32_MAX - 20u;
    TEST_ASSERT_TRUE(lifecycle.begin(start, 30u));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SdMutexHoldStep::Waiting),
                            static_cast<uint8_t>(lifecycle.step(start + 29u, true)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SdMutexHoldStep::AcquisitionExpired),
                            static_cast<uint8_t>(lifecycle.step(start + 30u, true)));
    TEST_ASSERT_EQUAL_UINT32(0u, fixture.releases.load());
    fixture.mutex.unlock();
}

void test_invalid_runtime_and_duplicate_begin_fail_closed() {
    SdMutexHoldLifecycle invalid;
    TEST_ASSERT_FALSE(invalid.begin(0, 100));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SdMutexHoldStep::InvalidRuntime),
                            static_cast<uint8_t>(invalid.step(0, true)));

    Fixture fixture;
    SdMutexHoldLifecycle lifecycle({tryAcquire, release, &fixture});
    TEST_ASSERT_TRUE(lifecycle.begin(0, 100));
    TEST_ASSERT_FALSE(lifecycle.begin(1, 100));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SdMutexHoldStep::Acquired),
                            static_cast<uint8_t>(lifecycle.step(1, true)));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(SdMutexHoldStep::Released),
                            static_cast<uint8_t>(lifecycle.step(2, false)));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_real_contention_waits_without_blocking_then_releases_on_owner_thread);
    RUN_TEST(test_acquisition_deadline_is_rollover_safe_and_never_releases_unowned_mutex);
    RUN_TEST(test_invalid_runtime_and_duplicate_begin_fail_closed);
    return UNITY_END();
}
