#pragma once

#include <cstdint>

// Both the post-connect followup and stale-stream recovery request alert data.
// This main-loop-owned gate gives those producers one session-scoped send
// history so their successful writes cannot violate the retry interval.
class BleAlertDataRequestGate {
  public:
    static constexpr uint32_t MIN_INTERVAL_MS = 1000;

    bool hasSuccessfulSend(uint32_t sessionGeneration) const {
        return hasSuccessfulSend_ && !stopped_ && sessionGeneration == sessionGeneration_;
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
        stopped_ = false;
        stoppingPhoneGeneration_ = 0;
    }

    // A transmitted stop ends the stream without erasing the last start's
    // pacing boundary. Phone departure can then request a new start.
    void recordStop(uint32_t sessionGeneration, uint32_t phoneGeneration = 0) {
        if (sessionGeneration != sessionGeneration_) {
            sessionGeneration_ = sessionGeneration;
            hasSuccessfulSend_ = false;
        }
        stopped_ = true;
        stoppingPhoneGeneration_ = phoneGeneration;
    }
    bool stopped(uint32_t sessionGeneration) const {
        return stopped_ && sessionGeneration == sessionGeneration_;
    }
    bool stoppedByPhone(uint32_t sessionGeneration, uint32_t phoneGeneration) const {
        return phoneGeneration != 0 && phoneGeneration == stoppingPhoneGeneration_ &&
               stopped(sessionGeneration);
    }

    void invalidate(uint32_t sessionGeneration) {
        if (hasSuccessfulSend(sessionGeneration)) hasSuccessfulSend_ = false;
    }

  private:
    uint32_t sessionGeneration_ = 0;
    uint32_t lastSuccessfulSendMs_ = 0;
    uint32_t stoppingPhoneGeneration_ = 0;
    bool hasSuccessfulSend_ = false;
    bool stopped_ = false;
};
