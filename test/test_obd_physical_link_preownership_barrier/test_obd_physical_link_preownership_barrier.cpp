#include <unity.h>

#include <atomic>
#include <cstdint>
#include <thread>

#ifndef V1_LINKED_TEST_OBD_PHYSICAL_LINK_PREOWNERSHIP_BARRIER
#define V1_LINKED_TEST_OBD_PHYSICAL_LINK_PREOWNERSHIP_BARRIER
#define V1_INLINE_TEST_OBD_PHYSICAL_LINK_PREOWNERSHIP_BARRIER
#endif
#include "../../src/modules/obd/obd_physical_link_preownership_barrier.h"
#ifdef V1_INLINE_TEST_OBD_PHYSICAL_LINK_PREOWNERSHIP_BARRIER
#include "../../src/modules/obd/obd_physical_link_preownership_barrier.cpp"
#endif

namespace {
struct Fixture {
    std::atomic<uint32_t> nowMs{100};
    std::atomic<uint32_t> cancellationEpoch{12};
    std::atomic<bool> linkDown{false};
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

ObdPhysicalLinkPreownershipRuntime runtimeFor(Fixture& fixture) {
    return {clockMs, cancellationEpoch, linkDownConfirmed, &fixture};
}

ObdPhysicalLinkPreownershipRequest requestFor(const Fixture& fixture, uint32_t maximumHoldMs = 1000) {
    return {fixture.cancellationEpoch.load(), 41, fixture.nowMs.load(), maximumHoldMs};
}
} // namespace

void test_holds_only_while_generation_is_live_and_uncancelled() {
    Fixture fixture;
    const auto observation = ObdPhysicalLinkPreownershipBarrier::observe(requestFor(fixture), runtimeFor(fixture));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ObdPhysicalLinkPreownershipOutcome::HoldOwnership),
                            static_cast<uint8_t>(observation.outcome));
    TEST_ASSERT_EQUAL_UINT32(12, observation.cancellationEpoch);
}

void test_cancellation_requires_matching_link_down_before_confirmation() {
    Fixture fixture;
    const auto request = requestFor(fixture);
    fixture.cancellationEpoch.store(14);
    auto observation = ObdPhysicalLinkPreownershipBarrier::observe(request, runtimeFor(fixture));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ObdPhysicalLinkPreownershipOutcome::CancellationObserved),
                            static_cast<uint8_t>(observation.outcome));

    fixture.linkDown.store(true);
    observation = ObdPhysicalLinkPreownershipBarrier::observe(request, runtimeFor(fixture));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ObdPhysicalLinkPreownershipOutcome::PreemptionConfirmed),
                            static_cast<uint8_t>(observation.outcome));
}

void test_link_down_without_cancellation_cannot_qualify_preemption() {
    Fixture fixture;
    const auto request = requestFor(fixture);
    fixture.linkDown.store(true);
    const auto observation = ObdPhysicalLinkPreownershipBarrier::observe(request, runtimeFor(fixture));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ObdPhysicalLinkPreownershipOutcome::LinkDownWithoutCancellation),
                            static_cast<uint8_t>(observation.outcome));
}

void test_deadline_is_bounded_across_clock_rollover() {
    Fixture fixture;
    fixture.nowMs.store(UINT32_MAX - 5);
    const auto request = requestFor(fixture, 10);
    fixture.nowMs.store(4);
    const auto observation = ObdPhysicalLinkPreownershipBarrier::observe(request, runtimeFor(fixture));
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ObdPhysicalLinkPreownershipOutcome::DeadlineReached),
                            static_cast<uint8_t>(observation.outcome));
}

void test_invalid_runtime_or_identity_never_holds_ownership() {
    Fixture fixture;
    ObdPhysicalLinkPreownershipRuntime invalid{};
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(ObdPhysicalLinkPreownershipOutcome::InvalidRuntime),
        static_cast<uint8_t>(ObdPhysicalLinkPreownershipBarrier::observe(requestFor(fixture), invalid).outcome));

    auto request = requestFor(fixture);
    request.activeGeneration = 0;
    TEST_ASSERT_EQUAL_UINT8(
        static_cast<uint8_t>(ObdPhysicalLinkPreownershipOutcome::InvalidRuntime),
        static_cast<uint8_t>(ObdPhysicalLinkPreownershipBarrier::observe(request, runtimeFor(fixture)).outcome));
}

void test_real_concurrent_preemption_requires_both_atomic_publications() {
    Fixture fixture;
    const auto request = requestFor(fixture, 5000);
    std::atomic<bool> observerStarted{false};
    std::thread callbackPublisher([&]() {
        while (!observerStarted.load()) {
            std::this_thread::yield();
        }
        fixture.cancellationEpoch.store(request.dispatchEpoch + 2);
        std::this_thread::yield();
        fixture.linkDown.store(true);
    });

    ObdPhysicalLinkPreownershipOutcome outcome = ObdPhysicalLinkPreownershipOutcome::HoldOwnership;
    observerStarted.store(true);
    for (uint32_t attempt = 0; attempt < 100000 && outcome != ObdPhysicalLinkPreownershipOutcome::PreemptionConfirmed;
         ++attempt) {
        outcome = ObdPhysicalLinkPreownershipBarrier::observe(request, runtimeFor(fixture)).outcome;
        std::this_thread::yield();
    }
    callbackPublisher.join();
    TEST_ASSERT_EQUAL_UINT8(static_cast<uint8_t>(ObdPhysicalLinkPreownershipOutcome::PreemptionConfirmed),
                            static_cast<uint8_t>(outcome));
}

int main(int, char**) {
    UNITY_BEGIN();
    RUN_TEST(test_holds_only_while_generation_is_live_and_uncancelled);
    RUN_TEST(test_cancellation_requires_matching_link_down_before_confirmation);
    RUN_TEST(test_link_down_without_cancellation_cannot_qualify_preemption);
    RUN_TEST(test_deadline_is_bounded_across_clock_rollover);
    RUN_TEST(test_invalid_runtime_or_identity_never_holds_ownership);
    RUN_TEST(test_real_concurrent_preemption_requires_both_atomic_publications);
    return UNITY_END();
}
