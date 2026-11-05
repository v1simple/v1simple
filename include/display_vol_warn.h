#pragma once

#include <Arduino.h>

struct VolumeZeroWarning {
    static constexpr uint32_t DELAY_MS = 15000;
    static constexpr uint32_t DURATION_MS = 10000;
    static constexpr uint32_t FLASH_INTERVAL_MS = 250;

    // Re-arm when volume becomes nonzero or the app connects/disconnects.
    void reset() {
        phase_ = Phase::Idle;
        phaseStartedMs_ = lastPaintMs_ = 0;
    }

    // The renderer and quiet-loop scheduler share these phase boundaries.
    bool refreshDue(uint32_t now) const {
        if (phase_ == Phase::Delay) return elapsed(now) >= DELAY_MS;
        if (phase_ != Phase::Active) return false;
        return elapsed(now) >= DURATION_MS ||
               elapsed(now) / FLASH_INTERVAL_MS != elapsed(lastPaintMs_) / FLASH_INTERVAL_MS;
    }

    bool flashOn(uint32_t now) const {
        return phase_ == Phase::Active && (elapsed(now) / FLASH_INTERVAL_MS) % 2 != 0;
    }

    /// Returns true while the warning should be drawn.
    /// @param volZero          true when mainVolume == 0 && hasVolumeData
    /// @param proxyConnected   true when BLE proxy client is connected
    /// @param speedVolZeroActive true when speed-mute intentionally set volume to 0
    bool evaluate(uint32_t now, bool volZero, bool proxyConnected, bool speedVolZeroActive = false) {
        if (!volZero || proxyConnected || speedVolZeroActive) {
            reset();
            return false;
        }
        if (phase_ == Phase::Acknowledged) {
            return false;
        }

        if (phase_ == Phase::Idle) {
            phase_ = Phase::Delay;
            phaseStartedMs_ = now;
        }

        if (phase_ == Phase::Delay) {
            if (elapsed(now) < DELAY_MS) return false;
            phase_ = Phase::Active;
            phaseStartedMs_ = now;
        }

        if (elapsed(now) < DURATION_MS) {
            lastPaintMs_ = now;
            return true;
        }

        phase_ = Phase::Acknowledged;
        return false;
    }

  private:
    enum class Phase : uint8_t { Idle, Delay, Active, Acknowledged };
    Phase phase_ = Phase::Idle;
    uint32_t phaseStartedMs_ = 0;
    uint32_t lastPaintMs_ = 0;
    uint32_t elapsed(uint32_t now) const { return static_cast<uint32_t>(now - phaseStartedMs_); }
};
