#pragma once

#include <Arduino.h>
#include <stdint.h>

class V1BLEClient;
class PacketParser;
struct VoiceContext;

enum class QuietOwner : uint8_t {
    None = 0,
    SpeedVolume,
    VolumeFade,
    TapGesture,
    WifiCommand,
    AutoPush,
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

    bool sendMute(QuietOwner owner, bool muted);
    bool sendVolume(QuietOwner owner, uint8_t volume, uint8_t muteVolume);

    bool retryPendingSpeedVolRestore(uint32_t nowMs);

    template <typename SpeedMuteLike, typename VolumeFadeLike>
    bool processSpeedVolume(uint32_t nowMs,
                            const SpeedMuteLike& speedMute,
                            VolumeFadeLike* volumeFade);

    template <typename VolumeFadeLike>
    bool executeVolumeFade(uint32_t nowMs,
                           VolumeFadeLike* volumeFade);

    template <typename SpeedMuteLike>
    void applyVoicePresentation(VoiceContext& voiceCtx,
                                const SpeedMuteLike* speedMute,
                                bool hasRenderablePriority,
                                uint8_t priorityBand);

    const QuietDesiredState& getDesiredState() const { return desired_; }
    QuietCommittedState getCommittedState();
    const QuietPresentationState& getPresentationState() const { return presentation_; }

private:
    void reset();
    void syncCommittedState();
    void refreshPendingState();

    template <typename SpeedMuteLike>
    void updateSpeedVolPresentation(const SpeedMuteLike* speedMute);

    V1BLEClient* ble_ = nullptr;
    PacketParser* parser_ = nullptr;

    QuietDesiredState desired_{};
    QuietCommittedState committed_{};
    QuietPresentationState presentation_{};

    bool speedVolActive_ = false;
    uint8_t speedVolSavedOriginal_ = 0xFF;
    uint8_t speedVolSavedMuteVol_ = 0;
    uint8_t pendingSpeedVolRestoreVol_ = 0xFF;
    uint8_t pendingSpeedVolRestoreMuteVol_ = 0;
    uint32_t pendingSpeedVolRestoreSetMs_ = 0;
    uint32_t pendingSpeedVolRestoreLastRetryMs_ = 0;
    uint32_t speedVolLastRetryMs_ = 0;
    static constexpr uint32_t SPEED_VOL_RETRY_INTERVAL_MS = 75;
    static constexpr uint32_t SPEED_VOL_RESTORE_TIMEOUT_MS = 2000;
};
