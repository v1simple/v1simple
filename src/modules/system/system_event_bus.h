#pragma once

#include <stddef.h>
#include <stdint.h>
#ifdef UNIT_TEST
#include <atomic>
#else
#include <freertos/FreeRTOS.h>
#endif

// Event types for low-overhead module decoupling.
// Keep payloads POD and fixed-size to avoid heap/format overhead in hot paths.
enum class SystemEventType : uint8_t {
    NONE = 0,
    BLE_FRAME_PARSED = 1,
    BLE_CONNECTED = 2,
    BLE_DISCONNECTED = 3,
    ALP_STATE_CHANGED = 4,
};

struct SystemEvent {
    SystemEventType type = SystemEventType::NONE;
    uint32_t tsMs = 0;
    uint32_t seq = 0;
    uint16_t detail = 0;
};

class SystemEventBus {
public:
    static constexpr size_t kCapacity = 64;

    void reset() {
        LockGuard guard(*this);
        head_ = 0;
        tail_ = 0;
        count_ = 0;
        publishCount_ = 0;
        dropCount_ = 0;
    }

    // Reset only counters while preserving any queued events.
    void resetStats() {
        LockGuard guard(*this);
        publishCount_ = 0;
        dropCount_ = 0;
    }

    // Non-blocking publish: never waits.
    // Overflow policy favors preserving low-rate control events by dropping the
    // oldest frame event first when available.
    // Returns true if no drop occurred for this publish.
    bool publish(const SystemEvent& event) {
        LockGuard guard(*this);
        bool dropped = false;
        if (count_ == kCapacity) {
            uint8_t dropOffset = 0;
            if (!findOldestFrameOffset(dropOffset)) {
                dropOffset = 0;  // No frame events present, drop oldest control event
            }
            removeAtOffset(dropOffset, nullptr);
            dropCount_++;
            dropped = true;
        }

        ring_[head_] = event;
        head_ = nextIndex(head_);
        count_++;
        publishCount_++;

        return !dropped;
    }

    bool consume(SystemEvent& out) {
        LockGuard guard(*this);
        if (count_ == 0) {
            return false;
        }

        out = ring_[tail_];
        tail_ = nextIndex(tail_);
        count_--;
        return true;
    }

    // Consume the oldest event matching the requested type while preserving
    // FIFO order for all remaining events.
    bool consumeByType(SystemEventType type, SystemEvent& out) {
        LockGuard guard(*this);
        if (count_ == 0) {
            return false;
        }

        uint8_t idx = tail_;
        uint8_t matchOffset = 0xFF;
        for (uint8_t offset = 0; offset < count_; ++offset) {
            if (ring_[idx].type == type) {
                out = ring_[idx];
                matchOffset = offset;
                break;
            }
            idx = nextIndex(idx);
        }

        if (matchOffset == 0xFF) {
            return false;
        }

        return removeAtOffset(matchOffset, &out);
    }

    uint32_t getPublishCount() const {
        LockGuard guard(*this);
        return publishCount_;
    }
    uint32_t getDropCount() const {
        LockGuard guard(*this);
        return dropCount_;
    }
    size_t size() const {
        LockGuard guard(*this);
        return count_;
    }

private:
    void lock() const {
#ifdef UNIT_TEST
        while (lockFlag_.test_and_set(std::memory_order_acquire)) {
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
        explicit LockGuard(const SystemEventBus& ownerRef) : owner(ownerRef) {
            owner.lock();
        }
        ~LockGuard() {
            owner.unlock();
        }
        const SystemEventBus& owner;
    };

    static bool isFrameEventType(SystemEventType type) {
        return type == SystemEventType::BLE_FRAME_PARSED;
    }

    static uint8_t nextIndex(uint8_t i) {
        return static_cast<uint8_t>((i + 1u) % kCapacity);
    }

    bool removeAtOffset(uint8_t offset, SystemEvent* removed) {
        if (offset >= count_) {
            return false;
        }

        uint8_t idx = static_cast<uint8_t>((tail_ + offset) % kCapacity);
        if (removed) {
            *removed = ring_[idx];
        }

        for (uint8_t pos = offset; pos + 1u < count_; ++pos) {
            uint8_t dst = static_cast<uint8_t>((tail_ + pos) % kCapacity);
            uint8_t src = static_cast<uint8_t>((tail_ + pos + 1u) % kCapacity);
            ring_[dst] = ring_[src];
        }

        head_ = static_cast<uint8_t>((head_ + kCapacity - 1u) % kCapacity);
        count_--;
        return true;
    }

    bool findOldestFrameOffset(uint8_t& offsetOut) const {
        uint8_t idx = tail_;
        for (uint8_t offset = 0; offset < count_; ++offset) {
            if (isFrameEventType(ring_[idx].type)) {
                offsetOut = offset;
                return true;
            }
            idx = nextIndex(idx);
        }
        return false;
    }

    SystemEvent ring_[kCapacity] = {};
    uint8_t head_ = 0;
    uint8_t tail_ = 0;
    uint8_t count_ = 0;
    uint32_t publishCount_ = 0;
    uint32_t dropCount_ = 0;
#ifdef UNIT_TEST
    mutable std::atomic_flag lockFlag_ = ATOMIC_FLAG_INIT;
#else
    mutable portMUX_TYPE lockMux_ = portMUX_INITIALIZER_UNLOCKED;
#endif
};
