#pragma once

#include <bitset>
#include <cstddef>
#include <cstdint>

// Session-scoped implementation of ESP 3.015 V1 request flow control.
// InfV1Busy replaces the current set of request IDs owned by the V1. A busy
// set, and any respRequestNotProcessed retry, remains blocked through the
// display immediately following the busy information. The next display with
// no preceding InfV1Busy releases the IDs.
class V1RequestFlowControl {
  public:
    void reset() {
        busy_.reset();
        rejected_.reset();
        releasedRetries_.reset();
        busySinceLastDisplay_ = false;
    }

    void onBusy(const uint8_t* packetIds, size_t count) {
        busy_.reset();
        if (packetIds) {
            for (size_t i = 0; i < count; ++i) busy_.set(packetIds[i]);
        }
        busySinceLastDisplay_ = true;
    }

    void onRequestNotProcessed(uint8_t packetId) {
        rejected_.set(packetId);
        releasedRetries_.reset(packetId);
    }

    void onDisplay() {
        if (busySinceLastDisplay_) {
            busySinceLastDisplay_ = false;
            return;
        }
        busy_.reset();
        releasedRetries_ |= rejected_;
        rejected_.reset();
    }

    bool holds(uint8_t packetId) const {
        return busy_.test(packetId) || rejected_.test(packetId);
    }

    bool consumeReleasedRetry(uint8_t packetId) {
        if (!releasedRetries_.test(packetId)) return false;
        releasedRetries_.reset(packetId);
        return true;
    }

    void clearReleasedRetries() { releasedRetries_.reset(); }

  private:
    std::bitset<256> busy_;
    std::bitset<256> rejected_;
    std::bitset<256> releasedRetries_;
    bool busySinceLastDisplay_ = false;
};
