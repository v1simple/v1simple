#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

// Both the post-connect followup and stale-stream recovery request alert data.
// This main-loop-owned gate gives those producers one session-scoped send
// history so their successful writes cannot violate the retry interval.
class BleAlertDataRequestGate {
  public:
    static constexpr uint32_t MIN_INTERVAL_MS = 1000;

    bool hasSuccessfulSend(uint32_t sessionGeneration) const {
        return hasSuccessfulSend_ && !stopped(sessionGeneration) && !restartRequired_ &&
               sessionGeneration == sessionGeneration_;
    }

    bool permits(uint32_t sessionGeneration, uint32_t nowMs) const {
        if (!hasSuccessfulSend_ || sessionGeneration != sessionGeneration_) {
            return true;
        }
        return static_cast<uint32_t>(nowMs - lastSuccessfulSendMs_) >= MIN_INTERVAL_MS;
    }

    void recordSuccessfulSend(uint32_t sessionGeneration, uint32_t sentAtMs) {
        sessionGeneration_ = sessionGeneration;
        lastSuccessfulSendMs_ = sentAtMs;
        hasSuccessfulSend_ = true;
        stopCount_ = 0;
        compacted_ = {};
        restartRequired_ = false;
    }

    // A transmitted stop ends the stream without erasing the last start's
    // pacing boundary. Phone departure can then request a new start.
    void recordStop(uint32_t sessionGeneration, uint32_t phoneGeneration,
                    uint32_t sendIngress, uint32_t clearanceIngress) {
        if (sessionGeneration != sessionGeneration_) {
            sessionGeneration_ = sessionGeneration;
            hasSuccessfulSend_ = false;
            stopCount_ = 0;
            compacted_ = {};
        }
        if (stopCount_ == stops_.size()) {
            // Keep a compact count for older sends so repeated valid STOPs do
            // not revoke a phone's hold. Their exact clearance boundaries are
            // no longer available, so withhold that display inference.
            const Stop& oldest = stops_[0];
            if (compacted_.count == 0) {
                compacted_.phoneGeneration = oldest.phoneGeneration;
                compacted_.firstSendIngress = oldest.sendIngress;
            } else if (compacted_.phoneGeneration != oldest.phoneGeneration) {
                compacted_.phoneGeneration = 0;
            }
            if (compacted_.count != UINT32_MAX) ++compacted_.count;
            for (size_t i = 1; i < stopCount_; ++i) stops_[i - 1] = stops_[i];
            --stopCount_;
        }
        stops_[stopCount_++] = {phoneGeneration, sendIngress, clearanceIngress};
    }
    void recordRejectedStop(uint32_t sessionGeneration) {
        if (sessionGeneration == sessionGeneration_) restartRequired_ = true;
    }
    void rejectStop(uint32_t sessionGeneration, uint32_t ingress) {
        if (sessionGeneration != sessionGeneration_) return;
        for (size_t remaining = stopCount_; remaining > 0; --remaining) {
            const size_t index = remaining - 1;
            if (!follows(ingress, stops_[index].sendIngress)) continue;
            for (size_t i = remaining; i < stopCount_; ++i) stops_[i - 1] = stops_[i];
            --stopCount_;
            recordRejectedStop(sessionGeneration);
            return;
        }
        if (compacted_.count && follows(ingress, compacted_.firstSendIngress)) {
            --compacted_.count;
            recordRejectedStop(sessionGeneration);
        }
    }
    bool stopped(uint32_t sessionGeneration) const {
        return (stopCount_ > 0 || compacted_.count > 0) && sessionGeneration == sessionGeneration_;
    }
    bool stoppedByPhone(uint32_t sessionGeneration, uint32_t phoneGeneration) const {
        if (phoneGeneration == 0 || !stopped(sessionGeneration)) return false;
        const uint32_t owningPhone = stopCount_ ? stops_[stopCount_ - 1].phoneGeneration
                                               : compacted_.phoneGeneration;
        return phoneGeneration == owningPhone;
    }
    bool receivedAfterPhoneStop(uint32_t sessionGeneration, uint32_t phoneGeneration,
                               uint32_t ingressSequence) const {
        return !compacted_.count && stoppedByPhone(sessionGeneration, phoneGeneration) &&
               follows(ingressSequence, stops_[stopCount_ - 1].clearanceIngress);
    }

    static bool follows(uint32_t ingress, uint32_t boundary) {
        return ingress != 0 && static_cast<int32_t>(ingress - boundary) > 0;
    }

    void invalidate(uint32_t sessionGeneration) {
        if (hasSuccessfulSend(sessionGeneration)) hasSuccessfulSend_ = false;
    }

  private:
    struct Stop {
        uint32_t phoneGeneration = 0;
        uint32_t sendIngress = 0;
        uint32_t clearanceIngress = 0;
    };
    struct CompactedStops {
        uint32_t phoneGeneration = 0;
        uint32_t firstSendIngress = 0;
        uint32_t count = 0;
    };
    // Track exact ingress for a full burst of the phone command queue.
    static constexpr size_t kMaxStopReceipts = 16;
    uint32_t sessionGeneration_ = 0;
    uint32_t lastSuccessfulSendMs_ = 0;
    std::array<Stop, kMaxStopReceipts> stops_{};
    CompactedStops compacted_{};
    size_t stopCount_ = 0;
    bool hasSuccessfulSend_ = false;
    bool restartRequired_ = false;
};
