// Low-speed volume/voice policy; the caller sends BLE commands.
// Fail open on lost speed, honor the configured volume on every band, and
// release at threshold + hysteresis. Decisions never block BLE or display.

#pragma once

#include <cstdint>

// --- Settings snapshot (read from V1Settings each loop) ---

struct SpeedMuteSettings {
    bool enabled = false;
    uint8_t thresholdMph = 25; // Mute below this speed (5–60 mph)
    uint8_t hysteresisMph = 3; // Unmute at threshold + hysteresis
    uint8_t v1Volume = 0;      // V1 volume when speed-muted (0-9)
    bool voice = true;         // Also suppress voice when speed-muted
};

// --- Input context — populated by caller each loop iteration ---

struct SpeedMuteContext {
    float speedMph = 0.0f;   // Current arbitrated speed (SpeedSourceSelector)
    bool speedValid = false; // Speed source is fresh & trusted
    uint32_t nowMs = 0;
};

// --- Decision output ---

struct SpeedMuteDecision {
    bool shouldMute = false; // True → apply speed-muted V1 volume
};

// --- Persistent state (owned by module instance, mutated by evaluate()) ---

struct SpeedMuteState {
    bool muteActive = false;       // Current muted state (with hysteresis applied)
    uint32_t lastTransitionMs = 0; // Timestamp of last mute/unmute transition
};

// --- Pure decision function (testable, no side effects beyond state mutation) ---

SpeedMuteDecision evaluateSpeedMute(const SpeedMuteSettings& settings, const SpeedMuteContext& ctx,
                                    SpeedMuteState& state);

// --- Module wrapper — convenience for main-loop wiring ---

class SpeedMuteModule {
  public:
    void begin(bool enabled, uint8_t thresholdMph, uint8_t hysteresisMph, uint8_t v1Volume = 0, bool voice = true);

    /// Update settings at runtime (from web UI / settings sync).
    void syncSettings(bool enabled, uint8_t thresholdMph, uint8_t hysteresisMph, uint8_t v1Volume = 0,
                      bool voice = true);

    /// Evaluate muting decision.  Call once per loop iteration.
    SpeedMuteDecision update(float speedMph, bool speedValid, uint32_t nowMs);

    const SpeedMuteSettings& getSettings() const { return settings_; }
    const SpeedMuteState& getState() const { return state_; }

  private:
    SpeedMuteSettings settings_;
    SpeedMuteState state_;
};
