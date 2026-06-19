#pragma once

/**
 * VolumeFadeModule - V1 volume fade logic
 *
 * Responsibilities:
 * - Track when alert starts (for fade delay)
 * - Decide when to fade volume down
 * - Track original volume for restoration
 * - Decide when to restore volume
 *
 * Does NOT:
 * - Send BLE commands (returns action for main to execute)
 */

#include <stdint.h>

// Forward declarations
class SettingsManager;

/**
 * Context for volume fade decisions
 */
struct VolumeFadeContext {
    bool hasAlert;              // Currently have active alert
    bool alertMuted;            // Alert is muted by user
    bool alertSuppressed;       // Alert is software-suppressed
    uint8_t currentVolume;      // Current V1 volume
    uint8_t currentMuteVolume;  // Current V1 mute volume
    uint16_t currentFrequency;  // Current priority frequency (MHz*10) for dedup
    unsigned long now;          // Current timestamp

    VolumeFadeContext() :
        hasAlert(false), alertMuted(false), alertSuppressed(false),
        currentVolume(0), currentMuteVolume(0), currentFrequency(0),
        now(0) {}
};

/**
 * Action returned by volume fade module
 */
struct VolumeFadeAction {
    enum class Type : uint8_t {
        NONE = 0,
        FADE_DOWN,      // Reduce volume
        RESTORE         // Restore original volume
    };

    Type type = Type::NONE;
    uint8_t targetVolume;       // Volume to set (FADE_DOWN)
    uint8_t targetMuteVolume;   // Mute volume to set (FADE_DOWN)
    uint8_t restoreVolume;      // Volume to restore to (RESTORE)
    uint8_t restoreMuteVolume;  // Mute volume to restore to (RESTORE)

    VolumeFadeAction() :
        type(Type::NONE), targetVolume(0), targetMuteVolume(0),
        restoreVolume(0), restoreMuteVolume(0) {}

    bool hasAction() const { return type != Type::NONE; }
};

/**
 * VolumeFadeModule class
 */
class VolumeFadeModule {
public:
    VolumeFadeModule();

    void begin(SettingsManager* settings);

    // Main decision method
    VolumeFadeAction process(const VolumeFadeContext& ctx);

    /// Inject a one-shot baseline hint from an external volume owner.
    /// If a new alert arrives before the V1 echoes back the true volume,
    /// VolumeFade uses this hint instead of the stale DisplayState value.
    /// Cleared after first use or 1.5 s.
    void setBaselineHint(uint8_t mainVol, uint8_t muteVol, uint32_t nowMs);

private:
    void reset();
    SettingsManager* settings_ = nullptr;

    // Tracking state
    unsigned long alertStartMs_ = 0;
    uint8_t originalVolume_ = 0;
    uint8_t originalMuteVolume_ = 0;
    bool fadeActive_ = false;
    bool commandSent_ = false;
    bool restoreLogEmitted_ = false;
    int seenCount_ = 0;
    static constexpr int MAX_FADE_SEEN_FREQS = 12;
    uint16_t seenFreqs_[MAX_FADE_SEEN_FREQS] = {};

    // Short-lived carry-over after issuing RESTORE: if a new alert arrives before
    // V1 applies the restore command, don't recapture the faded volume as baseline.
    uint8_t pendingRestoreVolume_ = 0;
    uint8_t pendingRestoreMuteVolume_ = 0;
    unsigned long pendingRestoreSetMs_ = 0;
    unsigned long lastRestoreAttemptMs_ = 0;
    static constexpr unsigned long PENDING_RESTORE_WINDOW_MS = 1500;
    static constexpr unsigned long RESTORE_RETRY_MIN_INTERVAL_MS = 75;

    // External baseline hint from an external volume owner.
    uint8_t hintBaselineVolume_ = 0xFF;     // 0xFF = no hint
    uint8_t hintBaselineMuteVolume_ = 0;
    unsigned long hintSetMs_ = 0;
    static constexpr unsigned long HINT_WINDOW_MS = 1500;

    void resetSessionState();
};
