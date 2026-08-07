#pragma once

#include <atomic>
#include <cstdint>

enum class BleProxyCallbackDirection : uint8_t {
    V1ToProxy = 0,
    ProxyToV1 = 1,
};

struct BleProxyEpochObserverSnapshot {
    uint32_t currentEpoch = 0;
    uint32_t admittedEpoch = 0;
    uint32_t activeCallbacks = 0;
    uint32_t v1ToProxyCallbackEntries = 0;
    uint32_t proxyToV1CallbackEntries = 0;
    uint32_t v1ToProxyAdmissions = 0;
    uint32_t proxyToV1Admissions = 0;
    uint32_t staleV1ToProxyRejections = 0;
    uint32_t staleProxyToV1Rejections = 0;
    uint32_t allocationCount = 0;
    uint32_t disableCount = 0;
    uint32_t releaseCount = 0;
    uint32_t reenableCount = 0;
    bool activeCallbackObserved = false;
    bool releaseOpportunityObserved = false;
    bool oldEpochForwarded = false;
};

struct BleProxyEpochQualificationSnapshot {
    BleProxyEpochObserverSnapshot epoch{};
    uint32_t proxyQueueHead = 0;
    uint32_t proxyQueueTail = 0;
    uint32_t proxyQueueCount = 0;
    uint32_t proxyQueueCapacity = 0;
    uint32_t phoneQueueHead = 0;
    uint32_t phoneQueueTail = 0;
    uint32_t phoneQueueCount = 0;
    uint32_t phoneQueueCapacity = 0;
    uint32_t freeInternalBytes = 0;
    uint32_t largestInternalBlockBytes = 0;
};

// Production-owned, allocation-free observation and admission boundary for the
// proxy queue epoch. Callback instrumentation is deliberately limited to
// atomics: it never logs, waits, allocates, or changes callback timing on
// behalf of qualification.
class BleProxyEpochObserver {
  public:
    class CallbackLease {
      public:
        CallbackLease(BleProxyEpochObserver& owner, BleProxyCallbackDirection direction, uint32_t epoch) noexcept
            : owner_(&owner), disableSequence_(owner.callbackEntered(direction)), epoch_(epoch),
              admittedAtEntry_(owner.accepts(epoch)) {}

        CallbackLease(const CallbackLease&) = delete;
        CallbackLease& operator=(const CallbackLease&) = delete;

        ~CallbackLease() {
            if (owner_) {
                owner_->callbackExited(disableSequence_, epoch_, admittedAtEntry_);
            }
        }

      private:
        BleProxyEpochObserver* owner_;
        uint32_t disableSequence_;
        uint32_t epoch_;
        bool admittedAtEntry_;
    };

    bool accepts(const uint32_t epoch) const noexcept {
        return epoch != 0 && admittedEpoch_.load(std::memory_order_acquire) == epoch;
    }

    void open(const uint32_t epoch) noexcept {
        if (epoch == 0) {
            return;
        }
        const uint32_t previousOpenCount = allocationCount_.fetch_add(1, std::memory_order_relaxed);
        if (previousOpenCount != 0) {
            reenableCount_.fetch_add(1, std::memory_order_relaxed);
        }
        currentEpoch_.store(epoch, std::memory_order_release);
        admittedEpoch_.store(epoch, std::memory_order_release);
    }

    void close() noexcept {
        if (admittedEpoch_.exchange(0, std::memory_order_acq_rel) == 0) {
            return;
        }
        disableSequence_.fetch_add(1, std::memory_order_acq_rel);
        disableCount_.fetch_add(1, std::memory_order_relaxed);
    }

    void noteQueueLockAcquired(const BleProxyCallbackDirection direction) noexcept {
        activeQueueCounter(direction).fetch_add(1, std::memory_order_acq_rel);
    }

    void noteQueueLockReleased(const BleProxyCallbackDirection direction) noexcept {
        activeQueueCounter(direction).fetch_sub(1, std::memory_order_acq_rel);
    }

    void noteReleaseTryLockFailed(const BleProxyCallbackDirection direction) noexcept {
        if (activeQueueCounter(direction).load(std::memory_order_acquire) != 0) {
            releaseOpportunityObserved_.store(true, std::memory_order_release);
        }
    }

    void noteReleaseCompleted() noexcept { releaseCount_.fetch_add(1, std::memory_order_relaxed); }

    void noteAdmission(const BleProxyCallbackDirection direction, const uint32_t epoch, const bool admitted) noexcept {
        const uint32_t currentEpoch = currentEpoch_.load(std::memory_order_acquire);
        if (admitted) {
            admissionCounter(direction).fetch_add(1, std::memory_order_relaxed);
            if (epoch == 0 || epoch != currentEpoch) {
                oldEpochForwarded_.store(true, std::memory_order_release);
            }
            return;
        }
        if (currentEpoch != 0 && epoch != 0 && epoch != currentEpoch) {
            staleRejectionCounter(direction).fetch_add(1, std::memory_order_relaxed);
        }
    }

    BleProxyEpochObserverSnapshot snapshot() const noexcept {
        BleProxyEpochObserverSnapshot value;
        value.currentEpoch = currentEpoch_.load(std::memory_order_acquire);
        value.admittedEpoch = admittedEpoch_.load(std::memory_order_acquire);
        value.activeCallbacks = activeCallbacks_.load(std::memory_order_acquire);
        value.v1ToProxyCallbackEntries = v1ToProxyCallbackEntries_.load(std::memory_order_relaxed);
        value.proxyToV1CallbackEntries = proxyToV1CallbackEntries_.load(std::memory_order_relaxed);
        value.v1ToProxyAdmissions = v1ToProxyAdmissions_.load(std::memory_order_relaxed);
        value.proxyToV1Admissions = proxyToV1Admissions_.load(std::memory_order_relaxed);
        value.staleV1ToProxyRejections = staleV1ToProxyRejections_.load(std::memory_order_relaxed);
        value.staleProxyToV1Rejections = staleProxyToV1Rejections_.load(std::memory_order_relaxed);
        value.allocationCount = allocationCount_.load(std::memory_order_relaxed);
        value.disableCount = disableCount_.load(std::memory_order_relaxed);
        value.releaseCount = releaseCount_.load(std::memory_order_relaxed);
        value.reenableCount = reenableCount_.load(std::memory_order_relaxed);
        value.activeCallbackObserved = activeCallbackObserved_.load(std::memory_order_acquire);
        value.releaseOpportunityObserved = releaseOpportunityObserved_.load(std::memory_order_acquire);
        value.oldEpochForwarded = oldEpochForwarded_.load(std::memory_order_acquire);
        return value;
    }

  private:
    uint32_t callbackEntered(const BleProxyCallbackDirection direction) noexcept {
        callbackEntryCounter(direction).fetch_add(1, std::memory_order_relaxed);
        activeCallbacks_.fetch_add(1, std::memory_order_acq_rel);
        return disableSequence_.load(std::memory_order_acquire);
    }

    void callbackExited(const uint32_t enteredDisableSequence, const uint32_t epoch,
                        const bool admittedAtEntry) noexcept {
        if (admittedAtEntry &&
            (disableSequence_.load(std::memory_order_acquire) != enteredDisableSequence || !accepts(epoch))) {
            activeCallbackObserved_.store(true, std::memory_order_release);
        }
        activeCallbacks_.fetch_sub(1, std::memory_order_acq_rel);
    }

    std::atomic<uint32_t>& callbackEntryCounter(const BleProxyCallbackDirection direction) noexcept {
        return direction == BleProxyCallbackDirection::V1ToProxy ? v1ToProxyCallbackEntries_
                                                                 : proxyToV1CallbackEntries_;
    }

    std::atomic<uint32_t>& admissionCounter(const BleProxyCallbackDirection direction) noexcept {
        return direction == BleProxyCallbackDirection::V1ToProxy ? v1ToProxyAdmissions_ : proxyToV1Admissions_;
    }

    std::atomic<uint32_t>& staleRejectionCounter(const BleProxyCallbackDirection direction) noexcept {
        return direction == BleProxyCallbackDirection::V1ToProxy ? staleV1ToProxyRejections_
                                                                 : staleProxyToV1Rejections_;
    }

    std::atomic<uint32_t>& activeQueueCounter(const BleProxyCallbackDirection direction) noexcept {
        return direction == BleProxyCallbackDirection::V1ToProxy ? activeV1ToProxyQueueUsers_
                                                                 : activeProxyToV1QueueUsers_;
    }

    std::atomic<uint32_t> currentEpoch_{0};
    std::atomic<uint32_t> admittedEpoch_{0};
    std::atomic<uint32_t> disableSequence_{0};
    std::atomic<uint32_t> activeCallbacks_{0};
    std::atomic<uint32_t> activeV1ToProxyQueueUsers_{0};
    std::atomic<uint32_t> activeProxyToV1QueueUsers_{0};
    std::atomic<uint32_t> v1ToProxyCallbackEntries_{0};
    std::atomic<uint32_t> proxyToV1CallbackEntries_{0};
    std::atomic<uint32_t> v1ToProxyAdmissions_{0};
    std::atomic<uint32_t> proxyToV1Admissions_{0};
    std::atomic<uint32_t> staleV1ToProxyRejections_{0};
    std::atomic<uint32_t> staleProxyToV1Rejections_{0};
    std::atomic<uint32_t> allocationCount_{0};
    std::atomic<uint32_t> disableCount_{0};
    std::atomic<uint32_t> releaseCount_{0};
    std::atomic<uint32_t> reenableCount_{0};
    std::atomic<bool> activeCallbackObserved_{false};
    std::atomic<bool> releaseOpportunityObserved_{false};
    std::atomic<bool> oldEpochForwarded_{false};
};
