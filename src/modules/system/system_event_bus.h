#pragma once

#include <stdint.h>
#ifdef UNIT_TEST
#include <atomic>
#else
#include <freertos/FreeRTOS.h>
#endif

// ALP state changes only need to wake the display path once before its next
// collection pass. Multiple changes before that pass are therefore one pending
// edge.
class SystemEventBus {
  public:
    void reset() {
        LockGuard guard(*this);
        alpStateChangedPending_ = false;
    }

    void publishAlpStateChanged() {
        LockGuard guard(*this);
        alpStateChangedPending_ = true;
    }

    bool consumeAlpStateChanged() {
        LockGuard guard(*this);
        if (!alpStateChangedPending_) {
            return false;
        }
        alpStateChangedPending_ = false;
        return true;
    }

    uint8_t size() const {
        LockGuard guard(*this);
        return alpStateChangedPending_ ? 1 : 0;
    }

  private:
    void lock() const {
#ifdef UNIT_TEST
        while (lockFlag_.test_and_set(std::memory_order_acquire)) {
            // EMPTY_BODY_OK: test builds intentionally spin until the lock is acquired.
        }
#else
        portENTER_CRITICAL(&lockMux_);
#endif
    }

    void unlock() const {
#ifdef UNIT_TEST
        lockFlag_.clear(std::memory_order_release);
#else
        portEXIT_CRITICAL(&lockMux_);
#endif
    }

    struct LockGuard {
        explicit LockGuard(const SystemEventBus& ownerRef) : owner(ownerRef) { owner.lock(); }
        ~LockGuard() { owner.unlock(); }
        const SystemEventBus& owner;
    };

    bool alpStateChangedPending_ = false;
#ifdef UNIT_TEST
    mutable std::atomic_flag lockFlag_ = ATOMIC_FLAG_INIT;
#else
    mutable portMUX_TYPE lockMux_ = portMUX_INITIALIZER_UNLOCKED;
#endif
};
