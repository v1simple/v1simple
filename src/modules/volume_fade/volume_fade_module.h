#pragma once

// Tracks fade delay, target volume and restoration; the caller sends BLE commands.

#include <stdint.h>

// Forward declarations
class SettingsManager;

/**
 * Context for volume fade decisions
 */
struct VolumeFadeContext {
    bool hasAlert;             // Currently have active alert
    bool alertEpisodeEnded;    // A clear occurred since the previous owner pass
    bool alertMuted;           // Alert is muted by user
    bool alertSuppressed;      // Alert is software-suppressed
    uint8_t currentVolume;     // Current V1 volume
    uint8_t currentMuteVolume; // Current V1 mute volume
    bool hasVolumeObservation = false;
    uint32_t volumeObservationIngress = 0;
    uint32_t sessionGeneration = 0;
    bool volumeOwnerChanged = false;
    bool restoreCommandInFlight = false; // Caller must harvest an admitted retry before confirming restoration.
    uint16_t currentFrequency; // Current priority frequency (MHz*10) for dedup
    bool priorityIsLaser;      // Priority alert is laser (no frequency; its own distinct-alert class)
    unsigned long now;         // Current timestamp

    VolumeFadeContext()
        : hasAlert(false), alertEpisodeEnded(false), alertMuted(false), alertSuppressed(false), currentVolume(0),
          currentMuteVolume(0), currentFrequency(0), priorityIsLaser(false), now(0) {}
};

/**
 * Action returned by volume fade module
 */
struct VolumeFadeAction {
    enum class Type : uint8_t {
        NONE = 0,
        FADE_DOWN, // Reduce volume
        RESTORE    // Restore original volume
    };

    Type type = Type::NONE;
    uint8_t targetVolume;      // Volume to set (FADE_DOWN)
    uint8_t targetMuteVolume;  // Mute volume to set (FADE_DOWN)
    uint8_t restoreVolume;     // Volume to restore to (RESTORE)
    uint8_t restoreMuteVolume; // Mute volume to restore to (RESTORE)

    VolumeFadeAction()
        : type(Type::NONE), targetVolume(0), targetMuteVolume(0), restoreVolume(0), restoreMuteVolume(0) {}

    bool hasAction() const { return type != Type::NONE; }
};

/**
 * VolumeFadeModule class
 */
class VolumeFadeModule {
  public:
    VolumeFadeModule();

    void begin(SettingsManager* settings);

    // Discard authority when the detector changes or a proxy phone takes over.
    // This deliberately emits no restore to the new owner.
    void reset();

    // Main decision method
    VolumeFadeAction process(const VolumeFadeContext& ctx);

    // Reconcile fresh authority before another automatic owner captures or
    // transfers our baseline. This emits no volume command.
    bool observeVolumeOwnership(const VolumeFadeContext& ctx);

    // End a cleared alert while another volume owner suppresses fade commands.
    // That owner must carry any returned restore pair until it releases volume.
    VolumeFadeAction releaseClearedAlert();

    // Record actual delivery separately from a requested restore. Cached volume
    // cannot acknowledge a deferred command or a write from another session.
    void noteRestoreSent(uint32_t sessionGeneration, uint32_t ingressBoundary);

    /// Inject a one-shot baseline hint from an external volume owner.
    /// If a new alert arrives before the V1 echoes back the true volume,
    /// VolumeFade uses this hint instead of the stale DisplayState value.
    /// Cleared after first use or 1.5 s.
    void setBaselineHint(uint8_t mainVol, uint8_t muteVol, uint32_t nowMs);

    // A captured baseline owns the pair even during the fade delay: a later
    // restore must not overwrite an Apply admitted while that baseline is held.
    // A sent restore alone does not release ownership.
    bool hasActiveVolumeOverride() const { return originalVolume_ != 0xFF || pendingRestoreVolume_ != 0xFF; }

  private:
    SettingsManager* settings_ = nullptr;

    // Tracking state
    unsigned long alertStartMs_ = 0;
    uint8_t originalVolume_ = 0;
    uint8_t originalMuteVolume_ = 0;
    bool fadeActive_ = false;
    bool commandSent_ = false;
    bool restoreLogEmitted_ = false;
    int seenCount_ = 0;
    bool seenLaser_ = false; // Laser has no frequency; deduped as its own class
    // Covers a complete 15-row V1 table. Longer continuous encounters evict
    // the oldest identity, but always remember the just-handled priority.
    static constexpr int MAX_FADE_SEEN_FREQS = 15;
    uint16_t seenFreqs_[MAX_FADE_SEEN_FREQS] = {};
    int nextSeenFrequency_ = 0;

    uint8_t fadeTargetVolume_ = 0xFF;
    uint8_t fadeTargetMuteVolume_ = 0;
    bool volumeObservationKnown_ = false;
    uint32_t volumeObservationSession_ = 0;
    uint32_t volumeObservationIngress_ = 0;
    bool manualChoiceActive_ = false;
    uint16_t manualChoiceFrequency_ = 0;
    bool manualChoiceLaser_ = false;

    // Restoration remains owed until the complete canonical pair converges or
    // an explicit owner/session reset discards authority. SENT is only delivery.
    uint8_t pendingRestoreVolume_ = 0;
    uint8_t pendingRestoreMuteVolume_ = 0;
    unsigned long lastRestoreAttemptMs_ = 0;
    bool restoreSent_ = false;
    uint32_t restoreSentSession_ = 0;
    uint32_t restoreSentIngress_ = 0;
    static constexpr unsigned long RESTORE_RETRY_MIN_INTERVAL_MS = 75;

    // External baseline hint from an external volume owner.
    uint8_t hintBaselineVolume_ = 0xFF; // 0xFF = no hint
    uint8_t hintBaselineMuteVolume_ = 0;
    unsigned long hintSetMs_ = 0;
    static constexpr unsigned long HINT_WINDOW_MS = 1500;

    void resetSessionState();
    void rememberFrequency(uint16_t frequency);
};
