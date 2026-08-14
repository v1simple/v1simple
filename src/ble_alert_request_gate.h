#pragma once

#include <cstdint>

// Both the post-connect followup and stale-stream recovery request alert data.
// This main-loop-owned gate gives those producers one session-scoped send
// history so their successful writes cannot violate the retry interval.
class BleAlertDataRequestGate {
  public:
    static constexpr uint32_t MIN_INTERVAL_MS = 1000;

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
    }

  private:
    uint32_t sessionGeneration_ = 0;
    uint32_t lastSuccessfulSendMs_ = 0;
    bool hasSuccessfulSend_ = false;
};
