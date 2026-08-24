#pragma once

#include <atomic>
#include <cstdint>

enum class BleProxyCallbackDirection : uint8_t {
    V1ToProxy = 0,
    ProxyToV1 = 1,
};

// Allocation-free admission boundary for proxy queue epochs. Closing an epoch
// rejects stale callbacks, while CallbackLease lets teardown wait until every
// callback that already entered has left before releasing queue storage.
class BleProxyEpochObserver {
  public:
    class CallbackLease {
      public:
        CallbackLease(BleProxyEpochObserver& owner, BleProxyCallbackDirection, uint32_t) noexcept : owner_(&owner) {
            owner_->activeCallbacks_.fetch_add(1, std::memory_order_acq_rel);
        }

        CallbackLease(const CallbackLease&) = delete;
        CallbackLease& operator=(const CallbackLease&) = delete;

        ~CallbackLease() {
            if (owner_) {
                owner_->activeCallbacks_.fetch_sub(1, std::memory_order_acq_rel);
            }
        }

      private:
        BleProxyEpochObserver* owner_;
    };

    bool accepts(uint32_t epoch) const noexcept {
        return epoch != 0 && admittedEpoch_.load(std::memory_order_acquire) == epoch;
    }

    void open(uint32_t epoch) noexcept {
        if (epoch != 0) {
            admittedEpoch_.store(epoch, std::memory_order_release);
        }
    }

    void close() noexcept { admittedEpoch_.store(0, std::memory_order_release); }

    bool hasActiveCallbacks() const noexcept {
        return activeCallbacks_.load(std::memory_order_acquire) != 0;
    }

  private:
    std::atomic<uint32_t> admittedEpoch_{0};
    std::atomic<uint32_t> activeCallbacks_{0};
};
