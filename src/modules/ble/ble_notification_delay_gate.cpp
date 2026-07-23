#include "ble_notification_delay_gate.h"

#include <cstring>

namespace {
class AtomicLease {
  public:
    explicit AtomicLease(std::atomic<uint32_t>& gate) noexcept : gate_(gate) {
        uint32_t expected = 0;
        acquired_ = gate_.compare_exchange_strong(expected, 1, std::memory_order_acq_rel);
    }

    ~AtomicLease() {
        if (acquired_) {
            gate_.store(0, std::memory_order_release);
        }
    }

    bool acquired() const noexcept { return acquired_; }

  private:
    std::atomic<uint32_t>& gate_;
    bool acquired_ = false;
};
} // namespace

BleNotificationDelayCaptureResult BleNotificationDelayGate::capture(const uint8_t* data, const size_t length,
                                                                    const uint16_t characteristicUuid,
                                                                    const uint32_t oldGeneration) noexcept {
    if (data == nullptr || length == 0 || length > notification_.data.size() || oldGeneration == 0) {
        return BleNotificationDelayCaptureResult::Invalid;
    }

    AtomicLease lease(mutationGate_);
    if (!lease.acquired()) {
        return BleNotificationDelayCaptureResult::Busy;
    }
    if (state_.load(std::memory_order_acquire) != static_cast<uint32_t>(State::Empty)) {
        return BleNotificationDelayCaptureResult::Occupied;
    }

#if defined(UNIT_TEST)
    if (captureLeaseHook_ != nullptr) {
        captureLeaseHook_(captureLeaseContext_);
    }
#endif

    std::memcpy(notification_.data.data(), data, length);
    notification_.length = length;
    notification_.characteristicUuid = characteristicUuid;
    notification_.oldGeneration = oldGeneration;
    notification_.newGeneration = 0;
    notification_.oldSessionClosedAtMs = 0;
    notification_.newSessionOpenedAtMs = 0;
    state_.store(static_cast<uint32_t>(State::Captured), std::memory_order_release);
    return BleNotificationDelayCaptureResult::Captured;
}

void BleNotificationDelayGate::recordSessionClosed(const uint32_t generation, const uint32_t nowMs) noexcept {
    uint32_t sequence = lifecycleSequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (sequence == 0) {
        sequence = lifecycleSequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    closedAtMs_.store(nowMs, std::memory_order_relaxed);
    closedSequence_.store(sequence, std::memory_order_relaxed);
    closedGeneration_.store(generation, std::memory_order_release);
}

void BleNotificationDelayGate::recordSessionOpened(const uint32_t generation, const uint32_t nowMs) noexcept {
    uint32_t sequence = lifecycleSequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (sequence == 0) {
        sequence = lifecycleSequence_.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
    openedAtMs_.store(nowMs, std::memory_order_relaxed);
    openedSequence_.store(sequence, std::memory_order_relaxed);
    openedGeneration_.store(generation, std::memory_order_release);
}

bool BleNotificationDelayGate::claimEligible(BleDelayedNotification& notification) noexcept {
    AtomicLease lease(mutationGate_);
    if (!lease.acquired() || state_.load(std::memory_order_acquire) != static_cast<uint32_t>(State::Captured)) {
        return false;
    }

    const uint32_t closedGeneration = closedGeneration_.load(std::memory_order_acquire);
    const uint32_t openedGeneration = openedGeneration_.load(std::memory_order_acquire);
    const uint32_t closedSequence = closedSequence_.load(std::memory_order_acquire);
    const uint32_t openedSequence = openedSequence_.load(std::memory_order_acquire);
    if (closedGeneration != notification_.oldGeneration || openedGeneration == 0 ||
        openedGeneration == notification_.oldGeneration || closedSequence == 0 || openedSequence == 0 ||
        static_cast<int32_t>(openedSequence - closedSequence) <= 0) {
        return false;
    }

    notification_.newGeneration = openedGeneration;
    notification_.oldSessionClosedAtMs = closedAtMs_.load(std::memory_order_acquire);
    notification_.newSessionOpenedAtMs = openedAtMs_.load(std::memory_order_acquire);
    notification = notification_;
    state_.store(static_cast<uint32_t>(State::Claimed), std::memory_order_release);
    return true;
}

bool BleNotificationDelayGate::discard() noexcept {
    AtomicLease lease(mutationGate_);
    if (!lease.acquired() || state_.load(std::memory_order_acquire) == static_cast<uint32_t>(State::Empty)) {
        return false;
    }
    notification_ = BleDelayedNotification{};
    state_.store(static_cast<uint32_t>(State::Empty), std::memory_order_release);
    return true;
}

bool BleNotificationDelayGate::hasCaptured() const noexcept {
    return state_.load(std::memory_order_acquire) == static_cast<uint32_t>(State::Captured);
}
