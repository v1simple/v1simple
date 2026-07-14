// Speed-Aware Muting Module — implementation

#include "speed_mute_module.h"

#ifndef UNIT_TEST
#include <Arduino.h>
#endif

// Minimum debounce between mute↔unmute transitions (ms).
// Prevents rapid toggling when speed oscillates near threshold.
static constexpr uint32_t TRANSITION_DEBOUNCE_MS = 1500;

// --- Pure decision function ---

SpeedMuteDecision evaluateSpeedMute(const SpeedMuteSettings& settings, const SpeedMuteContext& ctx,
                                    SpeedMuteState& state) {

    SpeedMuteDecision decision;

    // Feature disabled → never mute
    if (!settings.enabled) {
        if (state.muteActive) {
            state.muteActive = false;
            state.lastTransitionMs = ctx.nowMs;
        }
        return decision;
    }

    // Fail-open: no valid speed → never mute (safety first)
    if (!ctx.speedValid) {
        if (state.muteActive) {
            state.muteActive = false;
            state.lastTransitionMs = ctx.nowMs;
        }
        return decision;
    }

    // Hysteresis logic:
    //   mute   when speed drops BELOW threshold
    //   unmute when speed rises ABOVE (threshold + hysteresis)
    const float muteBelow = static_cast<float>(settings.thresholdMph);
    const float unmuteAbove = static_cast<float>(settings.thresholdMph + settings.hysteresisMph);

    bool wantMute = state.muteActive;
    if (!state.muteActive && ctx.speedMph < muteBelow) {
        wantMute = true;
    } else if (state.muteActive && ctx.speedMph >= unmuteAbove) {
        wantMute = false;
    }

    // Debounce rapid transitions
    if (wantMute != state.muteActive) {
        const uint32_t elapsed = ctx.nowMs - state.lastTransitionMs;
        if (elapsed >= TRANSITION_DEBOUNCE_MS) {
            state.muteActive = wantMute;
            state.lastTransitionMs = ctx.nowMs;
        }
        // else: keep current state until debounce passes
    }

    decision.shouldMute = state.muteActive;
    return decision;
}

// --- Module wrapper ---

void SpeedMuteModule::begin(bool enabled, uint8_t thresholdMph, uint8_t hysteresisMph, uint8_t v1Volume, bool voice) {
    settings_.enabled = enabled;
    settings_.thresholdMph = thresholdMph;
    settings_.hysteresisMph = hysteresisMph;
    settings_.v1Volume = v1Volume;
    settings_.voice = voice;
    state_ = {};
}

void SpeedMuteModule::syncSettings(bool enabled, uint8_t thresholdMph, uint8_t hysteresisMph, uint8_t v1Volume,
                                   bool voice) {
    settings_.enabled = enabled;
    settings_.thresholdMph = thresholdMph;
    settings_.hysteresisMph = hysteresisMph;
    settings_.v1Volume = v1Volume;
    settings_.voice = voice;
}

SpeedMuteDecision SpeedMuteModule::update(float speedMph, bool speedValid, uint32_t nowMs) {
    SpeedMuteContext ctx;
    ctx.speedMph = speedMph;
    ctx.speedValid = speedValid;
    ctx.nowMs = nowMs;
    return evaluateSpeedMute(settings_, ctx, state_);
}

bool SpeedMuteModule::isBandOverridden(uint8_t band) const {
    // Laser and Ka always override speed mute — never suppress these.
    return (band == 1 || band == 2); // BAND_LASER=1, BAND_KA=2
}
