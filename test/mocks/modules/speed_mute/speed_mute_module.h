#pragma once

#include <cstdint>

struct SpeedMuteSettings {
    bool enabled = false;
    uint8_t thresholdMph = 25;
    uint8_t hysteresisMph = 3;
    uint8_t v1Volume = 0;
    bool voice = true;
};

struct SpeedMuteState {
    bool muteActive = false;
    uint32_t lastTransitionMs = 0;
};

class SpeedMuteModule {
public:
    SpeedMuteSettings settings_;
    SpeedMuteState state_;

    void begin(bool enabled, uint8_t thresholdMph, uint8_t hysteresisMph,
               uint8_t v1Volume = 0, bool voice = true) {
        settings_.enabled = enabled;
        settings_.thresholdMph = thresholdMph;
        settings_.hysteresisMph = hysteresisMph;
        settings_.v1Volume = v1Volume;
        settings_.voice = voice;
    }

    void syncSettings(bool enabled, uint8_t thresholdMph, uint8_t hysteresisMph,
                      uint8_t v1Volume = 0, bool voice = true) {
        settings_.enabled = enabled;
        settings_.thresholdMph = thresholdMph;
        settings_.hysteresisMph = hysteresisMph;
        settings_.v1Volume = v1Volume;
        settings_.voice = voice;
    }

    bool isBandOverridden(uint8_t band) const {
        return (band == 1 || band == 2);  // Laser and Ka always override
    }

    const SpeedMuteSettings& getSettings() const { return settings_; }
    const SpeedMuteState& getState() const { return state_; }
};
