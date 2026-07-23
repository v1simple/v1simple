#include <unity.h>

#include <atomic>
#include <cstdint>
#include <thread>

#ifndef V1_LINKED_TEST_OBD_TRANSPORT_OPERATION_BARRIER
#define V1_LINKED_TEST_OBD_TRANSPORT_OPERATION_BARRIER
#define V1_INLINE_TEST_OBD_TRANSPORT_OPERATION_BARRIER
#endif
#include "../../src/modules/obd/obd_transport_operation_barrier.h"
#ifdef V1_INLINE_TEST_OBD_TRANSPORT_OPERATION_BARRIER
#include "../../src/modules/obd/obd_transport_operation_barrier.cpp"
#endif

namespace {
struct Fixture {
    std::atomic<uint32_t> nowMs{100};
    std::atomic<uint32_t> cancellationEpoch{12};
    std::atomic<bool> linkDown{false};
    std::atomic<uint32_t> yields{0};
    std::atomic<bool> continuePause{true};
};

uint32_t clockMs(void* context) noexcept {
    return static_cast<Fixture*>(context)->nowMs.load();
}

uint32_t cancellationEpoch(void* context) noexcept {
    return static_cast<Fixture*>(context)->cancellationEpoch.load();
}

bool linkDownConfirmed(uint32_t, void* context) noexcept {
    return static_cast<Fixture*>(context)->linkDown.load();
}

void yieldTransportOwner(void* context) noexcept {
    auto& fixture = *static_cast<Fixture*>(context);
    fixture.yields.fetch_add(1);
    fixture.nowMs.fetch_add(1);
    std::this_thread::yield();
}

bool continuePause(uint32_t, void* context) noexcept {
    return static_cast<Fixture*>(context)->continuePause.load();
}

ObdTransportBarrierRuntime runtimeFor(Fixture& fixture) {
    return {clockMs, cancellationEpoch, linkDownConfirmed, yieldTransportOwner, &fixture};
}

ObdTransportBarrierRequest requestFor(const Fixture& fixture, uint32_t maximumWaitMs = 1000) {
    return {fixture.cancellationEpoch.load(), 41, fixture.nowMs.load(), maximumWaitMs};
}
} // namespace

void test_newer_cancellation_epoch_wins_before_another_transport_operation() {
    Fixture fixture;
    const ObdTransportBarrierRequest request = requestFor(fixture);
    fixture.cancellationEpoch.store(request.dispatchEpoch + 2);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ObdTransportBarrierOutcome::CancellationEpochAdvanced),
                            static_cast<uint8_t>(ObdTransportOperationBarrier::wait(request, runtimeFor(fixture),
                                                                                    continuePause, &fixture)));
    TEST_ASSERT_NOT_EQUAL(request.dispatchEpoch, fixture.cancellationEpoch.load());
    TEST_ASSERT_EQUAL_UINT32(0, fixture.yields.load());
}

void test_matching_link_down_wins_before_another_transport_operation() {
    Fixture fixture;
    const ObdTransportBarrierRequest request = requestFor(fixture);
    fixture.linkDown.store(true);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ObdTransportBarrierOutcome::LinkDownConfirmed),
                            static_cast<uint8_t>(ObdTransportOperationBarrier::wait(request, runtimeFor(fixture),
                                                                                    continuePause, &fixture)));
    TEST_ASSERT_EQUAL_UINT32(request.dispatchEpoch, fixture.cancellationEpoch.load());
    TEST_ASSERT_EQUAL_UINT32(0, fixture.yields.load());
}

void test_deadline_is_bounded_across_clock_rollover() {
    Fixture fixture;
    fixture.nowMs.store(UINT32_MAX - 5);
    const ObdTransportBarrierRequest request = requestFor(fixture, 10);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ObdTransportBarrierOutcome::DeadlineReached),
                            static_cast<uint8_t>(ObdTransportOperationBarrier::wait(request, runtimeFor(fixture),
                                                                                    continuePause, &fixture)));
    TEST_ASSERT_EQUAL_UINT32(10, fixture.yields.load());
    TEST_ASSERT_EQUAL_UINT32(4, fixture.nowMs.load());
}

void test_pause_end_and_invalid_runtime_fail_open_without_spinning() {
    Fixture fixture;
    fixture.continuePause.store(false);
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ObdTransportBarrierOutcome::PauseEnded),
                            static_cast<uint8_t>(ObdTransportOperationBarrier::wait(
                                requestFor(fixture), runtimeFor(fixture), continuePause, &fixture)));
    TEST_ASSERT_EQUAL_UINT32(0, fixture.yields.load());

    ObdTransportBarrierRuntime invalid{};
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ObdTransportBarrierOutcome::InvalidRuntime),
                            static_cast<uint8_t>(ObdTransportOperationBarrier::wait(requestFor(fixture), invalid,
                                                                                    continuePause, &fixture)));
}

void test_real_concurrent_cancellation_releases_the_transport_owner() {
    Fixture fixture;
    const ObdTransportBarrierRequest request = requestFor(fixture, 5000);
    ObdTransportBarrierOutcome outcome = ObdTransportBarrierOutcome::InvalidRuntime;
    std::thread transportOwner(
        [&]() { outcome = ObdTransportOperationBarrier::wait(request, runtimeFor(fixture), continuePause, &fixture); });

    while (fixture.yields.load() == 0) {
        std::this_thread::yield();
    }
    fixture.cancellationEpoch.store(request.dispatchEpoch + 2);
    transportOwner.join();

    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ObdTransportBarrierOutcome::CancellationEpochAdvanced),
                            static_cast<uint8_t>(outcome));
    TEST_ASSERT_NOT_EQUAL(request.dispatchEpoch, fixture.cancellationEpoch.load());
    TEST_ASSERT_GREATER_THAN_UINT32(0, fixture.yields.load());
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_newer_cancellation_epoch_wins_before_another_transport_operation);
    RUN_TEST(test_matching_link_down_wins_before_another_transport_operation);
    RUN_TEST(test_deadline_is_bounded_across_clock_rollover);
    RUN_TEST(test_pause_end_and_invalid_runtime_fail_open_without_spinning);
    RUN_TEST(test_real_concurrent_cancellation_releases_the_transport_owner);
    return UNITY_END();
}
