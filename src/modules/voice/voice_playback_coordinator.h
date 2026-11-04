#pragma once

#include <array>
#include <stdint.h>
#include "voice_module.h"

class V1BLEClient;
class QuietCoordinatorModule;
class SpeedMuteModule;

struct V1VoiceObservation {
    std::array<AlertData, 16> alerts{};
    uint8_t count = 0;
    bool hasPriority = false;
    AlertData priority{};
    uint32_t episode = 0;
    bool muted = false;
    bool softMuted = false;
    bool proxyConnected = false;
    uint8_t mainVolume = 0;
};

struct VoiceSettings {
    VoiceAlertMode voiceAlertMode;
    bool voiceDirectionEnabled;
    bool announceBogeyCount;
};

AudioPlaybackResult submitVoicePlayback(const VoiceAction& action, const VoiceSettings& settings,
                                        AudioPlaybackReceipt& receipt);
struct AudioPlaybackPort {
    AudioPlaybackResult (*submit)(const VoiceAction&, const VoiceSettings&, AudioPlaybackReceipt&) = submitVoicePlayback;
};

// Application-lifetime owner: the receipt must outlive every accepted worker job.
// Selection/history stay in VoiceModule; this owner settles or retries playback.
class VoicePlaybackCoordinator {
  public:
    VoicePlaybackCoordinator(VoiceModule& voice, PacketParser& parser, V1BLEClient& ble,
                             QuietCoordinatorModule* quiet = nullptr, SpeedMuteModule* speedMute = nullptr,
                             AudioPlaybackPort playback = {})
        : voice_(&voice), parser_(&parser), ble_(&ble), quiet_(quiet), speedMute_(speedMute), playback_(playback) {}
    void begin(uint32_t currentEpisode);
    void onParsedPresentation(const V1VoiceObservation& observation, const VoiceSettings& settings, uint32_t nowMs);
    void servicePendingCancellation();
    bool hasPendingPlayback() const {
        return voicePlaybackReceipt_.state.load() == AudioPlaybackReceipt::State::Pending;
    }

  private:
    VoiceModule* voice_;
    PacketParser* parser_;
    V1BLEClient* ble_;
    QuietCoordinatorModule* quiet_;
    SpeedMuteModule* speedMute_;
    AudioPlaybackPort playback_;
    uint32_t voiceAlertLifetime_ = 0;
    VoiceAction pendingVoiceAction_{};
    uint32_t pendingVoicePriorityId_ = 0;
    bool hasPendingVoiceAction_ = false;
    AudioPlaybackReceipt voicePlaybackReceipt_;
    bool awaitingVoicePlayback_ = false;
    uint32_t voiceAcceptedAtMs_ = 0;
    bool voiceRetryScheduled_ = false;
    uint32_t nextVoiceAttemptMs_ = 0;
    static constexpr uint32_t VOICE_RETRY_INTERVAL_MS = 100;

    bool settleVoicePlayback(uint32_t nowMs);
};
