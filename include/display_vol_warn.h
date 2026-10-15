#pragma once

#include <Arduino.h>

struct VolumeZeroWarning {
    unsigned long detectedMs = 0;     // When volume=0 was first detected
    unsigned long warningStartMs = 0; // When warning display actually started
    bool shown = false;
    bool acknowledged = false;

    static constexpr unsigned long DELAY_MS = 15000;
    static constexpr unsigned long DURATION_MS = 10000;

    // Re-arm when volume becomes nonzero or the app connects/disconnects.
    void reset() {
        detectedMs = 0;
        warningStartMs = 0;
        shown = false;
        acknowledged = false;
    }

    /// Returns true while the warning should be drawn.
    /// @param volZero          true when mainVolume == 0 && hasVolumeData
    /// @param proxyConnected   true when BLE proxy client is connected
    /// @param speedVolZeroActive true when speed-mute intentionally set volume to 0
    bool evaluate(bool volZero, bool proxyConnected, bool speedVolZeroActive = false) {
        if (!volZero || proxyConnected || speedVolZeroActive) {
            reset();
            return false;
        }
        if (acknowledged) {
            return false;
        }

        const unsigned long now = millis();
        if (detectedMs == 0) {
            detectedMs = now;
        }

        if ((now - detectedMs) < DELAY_MS) {
            return false;
        }

        if (warningStartMs == 0) {
            warningStartMs = now;
            shown = true;
        }

        if ((now - warningStartMs) < DURATION_MS) {
            return true;
        }

        acknowledged = true;
        shown = false;
        return false;
    }

    /// Return true when a flashing redraw is needed even on the incremental path.
    /// Used in the resting-screen early-exit check.
    bool needsFlashRedraw(bool volZero, bool proxyConnected, bool speedVolZeroActive = false) const {
        if (!volZero || proxyConnected || speedVolZeroActive || acknowledged) {
            return false;
        }
        if (detectedMs == 0) {
            return true; // Timer not started yet — force full redraw to start it
        }
        if ((millis() - detectedMs) >= DELAY_MS) {
            if (warningStartMs == 0 || (millis() - warningStartMs) < DURATION_MS) {
                return true;
            }
        }
        return shown;
    }
};
