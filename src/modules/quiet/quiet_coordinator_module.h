#pragma once

#include <Arduino.h>
#include <stdint.h>
#include "modules/in_the_box/in_the_box_module.h"

class V1BLEClient;
class PacketParser;
struct VoiceContext;
enum class SendResult;

enum class QuietOwner : uint8_t {
    None = 0,
    SpeedVolume,
    VolumeFade,
    TapGesture,
    WifiCommand,
    AutoPush,
    InTheBox,
};

const char* quietOwnerName(QuietOwner owner);

struct QuietIntent {
    QuietOwner owner = QuietOwner::None;
    bool hasMute = false;
    bool mute = false;
    bool hasVolume = false;
    uint8_t volume = 0xFF;
    uint8_t muteVolume = 0;
};

struct QuietDesiredState {
    QuietOwner muteOwner = QuietOwner::None;
    bool mutePending = false;
    bool mute = false;

    QuietOwner volumeOwner = QuietOwner::None;
    bool volumePending = false;
    uint8_t volume = 0xFF;
    uint8_t muteVolume = 0;
};

struct QuietCommittedState {
    bool connected = false;
    bool hasDisplayState = false;
    bool muted = false;
    uint8_t mainVolume = 0;
    uint8_t muteVolume = 0;
};

struct QuietPresentationState {
    QuietOwner activeMuteOwner = QuietOwner::None;
    QuietOwner activeVolumeOwner = QuietOwner::None;
    bool speedVolZeroActive = false;
    bool voiceSuppressed = false;
    bool voiceAllowVolZeroBypass = false;
    bool effectiveMuted = false;
};

class QuietCoordinatorModule {
  public:
    void begin(V1BLEClient* bleClient, PacketParser* parser);

    // The applied configuration belongs to one detector session. Persistence
    // and profile activation are owned by the runtime, not the audio loop.
    void setInTheBoxSettings(const V1InTheBoxSettings& settings, uint32_t sessionGeneration);
    void processInTheBox(uint32_t nowMs);
    void setInTheBoxSuspended(bool suspended) { inTheBoxSuspended_ = suspended; }
    const V1InTheBoxSettings& inTheBoxSettings() const { return inTheBoxSettings_; }
    bool inTheBoxMuteActive() const { return inTheBoxOwnsMute_; }

    bool sendMute(QuietOwner owner, bool muted);
    SendResult sendMuteResult(QuietOwner owner, bool muted);
    bool sendVolume(QuietOwner owner, uint8_t volume, uint8_t muteVolume);
    SendResult sendVolumeResult(QuietOwner owner, uint8_t volume, uint8_t muteVolume);
    // Apply an AutoPush baseline pair without lifting an active speed-volume override.
    bool sendAutoPushVolume(uint8_t volume, uint8_t muteVolume);
    bool sendAutoPushVolume(uint8_t volume, uint8_t muteVolume, uint8_t aux0);
    // A settings transaction may only promise exact volume readback when the
    // coordinator will send the requested pair immediately. During an active
    // speed-volume override AutoPush updates a later restore baseline instead.
    bool canApplyAutoPushVolumeExactly() const {
        return !speedVolActive_ && pendingSpeedVolRestoreVol_ == 0xFF && !pendingFadeAction_ &&
               presentation_.activeVolumeOwner != QuietOwner::VolumeFade;
    }
    // Hold competing volume writes between the profile write and its canonical
    // 0x3D all-volume readback. Other owners receive NOT_YET and retain their existing
    // retry behavior rather than overwriting the value being verified.
    bool beginAutoPushVolumeTransaction();
    void endAutoPushVolumeTransaction() { autoPushVolumeTransactionActive_ = false; }

    bool retryPendingSpeedVolRestore(uint32_t nowMs);

    template <typename SpeedMuteLike, typename VolumeFadeLike>
    bool processSpeedVolume(uint32_t nowMs, const SpeedMuteLike& speedMute, VolumeFadeLike* volumeFade);

    template <typename VolumeFadeLike> bool executeVolumeFade(uint32_t nowMs, VolumeFadeLike* volumeFade);

    template <typename SpeedMuteLike>
    void applyVoicePresentation(VoiceContext& voiceCtx, const SpeedMuteLike* speedMute, bool hasRenderablePriority,
                                uint8_t priorityBand);

    const QuietDesiredState& getDesiredState() const { return desired_; }
    QuietCommittedState getCommittedState();
    const QuietPresentationState& getPresentationState() const { return presentation_; }

  private:
    void reset();
    void syncCommittedState();
    void refreshPendingState();
    void resetInTheBoxSession();

    template <typename SpeedMuteLike> void updateSpeedVolPresentation(const SpeedMuteLike* speedMute);

    V1BLEClient* ble_ = nullptr;
    PacketParser* parser_ = nullptr;

    QuietDesiredState desired_{};
    QuietCommittedState committed_{};
    QuietPresentationState presentation_{};

    bool speedVolActive_ = false;
    uint8_t speedVolSavedOriginal_ = 0xFF;
    uint8_t speedVolSavedMuteVol_ = 0;
    bool speedVolBaselineUpdated_ = false; // A newer AutoPush pair supersedes the captured fade baseline.
    uint8_t pendingSpeedVolRestoreVol_ = 0xFF;
    uint8_t pendingSpeedVolRestoreMuteVol_ = 0;
    uint32_t pendingSpeedVolRestoreSetMs_ = 0;
    uint32_t pendingSpeedVolRestoreLastRetryMs_ = 0;
    uint32_t speedVolLastRetryMs_ = 0;
    bool pendingFadeAction_ = false;
    bool pendingFadeRestore_ = false;
    uint8_t pendingFadeVolume_ = 0;
    uint8_t pendingFadeMuteVolume_ = 0;
    uint16_t pendingFadeFrequency_ = 0;
    bool pendingFadeLaser_ = false;
    uint32_t pendingFadeLastAttemptMs_ = 0;
    bool autoPushVolumeTransactionActive_ = false;
    V1InTheBoxSettings inTheBoxSettings_{};
    InTheBoxModule inTheBox_;
    uint32_t inTheBoxSession_ = 0;
    uint32_t inTheBoxLifetime_ = 0;
    bool inTheBoxLifetimeKnown_ = false;
    bool inTheBoxOwnsMute_ = false;
    bool inTheBoxManualOverride_ = false;
    bool inTheBoxInsidePending_ = false;
    bool inTheBoxCommandPending_ = false;
    bool inTheBoxCommandMute_ = false;
    bool inTheBoxCommandSent_ = false;
    bool inTheBoxMuteConfirmed_ = false;
    uint32_t inTheBoxCommandRevision_ = 0;
    uint32_t inTheBoxCommandIngress_ = 0;
    uint32_t inTheBoxLastAttemptMs_ = 0;
    bool inTheBoxAttempted_ = false;
    bool inTheBoxSuppressVoice_ = false;
    bool inTheBoxSuspended_ = false;
    static constexpr uint32_t IN_THE_BOX_RETRY_MS = 100;
    static constexpr uint32_t FADE_RETRY_INTERVAL_MS = 25;
    static constexpr uint32_t SPEED_VOL_RETRY_INTERVAL_MS = 75;
    static constexpr uint32_t SPEED_VOL_RESTORE_TIMEOUT_MS = 2000;
};
