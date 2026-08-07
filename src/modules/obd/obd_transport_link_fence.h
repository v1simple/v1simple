#pragma once

#include <atomic>
#include <cstdint>

// Single-slot handoff from BLE callback/main-loop producers to the OBD
// transport owner. Multiple publications coalesce; the most recently
// published link generation and disconnect reason are returned together.
class ObdTransportLinkFence {
  public:
    void publish(uint32_t generation, int reason) {
        generation_.store(generation, std::memory_order_relaxed);
        reason_.store(reason, std::memory_order_relaxed);
        pending_.store(true, std::memory_order_release);
    }

    bool consume(uint32_t& generation, int& reason) {
        if (!pending_.exchange(false, std::memory_order_acq_rel)) {
            return false;
        }
        generation = generation_.load(std::memory_order_acquire);
        reason = reason_.load(std::memory_order_acquire);
        return true;
    }

    bool pending() const { return pending_.load(std::memory_order_acquire); }

  private:
    std::atomic<uint32_t> generation_{0};
    std::atomic<int> reason_{0};
    std::atomic<bool> pending_{false};
};
