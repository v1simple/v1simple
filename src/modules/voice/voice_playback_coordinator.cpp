#include "voice_playback_coordinator.h"

#include "ble_client.h"
#include "packet_parser.h"
#include "modules/quiet/quiet_coordinator_module.h"
#include "modules/quiet/quiet_coordinator_voice_templates.h"
#include "modules/speed_mute/speed_mute_module.h"

namespace {
bool pendingVoiceActionStillCurrent(const VoiceAction& action, const V1VoiceObservation& alerts) {
    const auto matches = [&](const AlertData& alert) {
        const bool isPhoto = alert.band == BAND_K && alert.photoType != 0;
        return isPhoto == action.sourcePhoto && alert.band == action.sourceBand &&
               static_cast<uint16_t>(alert.frequency) == action.freq && alert.direction == action.sourceDirection;
    };

    if (action.type == VoiceAction::Type::ANNOUNCE_PRIORITY ||
        action.type == VoiceAction::Type::ANNOUNCE_DIRECTION) {
        return alerts.hasPriority && matches(alerts.priority);
    }
    for (int i = 0; i < alerts.count; ++i) {
        if (matches(alerts.alerts[i])) {
            return true;
        }
    }
    return false;
}

} // namespace

AudioPlaybackResult submitVoicePlayback(const VoiceAction& action, const VoiceSettings& settings,
                                        AudioPlaybackReceipt& receipt) {
    AudioPlaybackResult playbackResult = AudioPlaybackResult::Unavailable;
    switch (action.type) {
    case VoiceAction::Type::ANNOUNCE_PRIORITY:
        playbackResult = try_play_frequency_voice(action.band, action.freq, action.dir,
                                                  settings.voiceAlertMode, settings.voiceDirectionEnabled,
                                                  action.bogeyCount, &receipt);
        break;

    case VoiceAction::Type::ANNOUNCE_DIRECTION:
        playbackResult = try_play_direction_only(action.dir, action.bogeyCount, &receipt);
        break;

    case VoiceAction::Type::ANNOUNCE_SECONDARY:
        playbackResult = try_play_frequency_voice(action.band, action.freq, action.dir,
                                                  settings.voiceAlertMode, settings.voiceDirectionEnabled, 1,
                                                  &receipt);
        break;

    case VoiceAction::Type::ANNOUNCE_ESCALATION:
        playbackResult = try_play_threat_escalation(action.band, action.freq, action.dir,
                                                    action.bogeyCount, action.aheadCount,
                                                    action.behindCount, action.sideCount,
                                                    settings.voiceAlertMode, settings.voiceDirectionEnabled,
                                                    settings.announceBogeyCount, &receipt);
        break;

    case VoiceAction::Type::NONE:
    default:
        break;
    }

    return playbackResult;
}

void VoicePlaybackCoordinator::begin(uint32_t currentEpisode) {
    voiceAlertLifetime_ = currentEpisode;
    pendingVoiceAction_ = VoiceAction{};
    pendingVoicePriorityId_ = 0;
    hasPendingVoiceAction_ = false;
    awaitingVoicePlayback_ = false;
    voiceRetryScheduled_ = false;
    nextVoiceAttemptMs_ = 0;
}

// Admission only reserves the worker. Voice dedup is committed once PCM can
// have started (or a missing asset is known terminal).
// Startup failure leaves this exact action unheard; current-frame validation
// below decides whether it is still eligible for another attempt.
bool VoicePlaybackCoordinator::settleVoicePlayback(uint32_t nowMs) {
    // A clear/new episode may discard selection while the worker still owns
    // this receipt. Cancellation is not acknowledged until cleanup finishes.
    if (voicePlaybackReceipt_.isOwned()) return false;
    if (!awaitingVoicePlayback_) return true;
    const auto state = voicePlaybackReceipt_.state.load();
    awaitingVoicePlayback_ = false;
    if (state == AudioPlaybackReceipt::State::CancelledBeforeStart) {
        // Unheard, not terminally missing: select from current policy/alerts
        // when suppression lifts, without committing stale content or dedup.
        hasPendingVoiceAction_ = false;
        voiceRetryScheduled_ = false;
        nextVoiceAttemptMs_ = 0;
    } else if (state == AudioPlaybackReceipt::State::FailedBeforeStart) {
        voiceRetryScheduled_ = true;
        nextVoiceAttemptMs_ = nowMs + VOICE_RETRY_INTERVAL_MS;
    } else {
        voice_->commitAction(pendingVoiceAction_, voiceAcceptedAtMs_);
        hasPendingVoiceAction_ = false;
        voiceRetryScheduled_ = false;
        nextVoiceAttemptMs_ = 0;
    }
    return true;
}

void VoicePlaybackCoordinator::servicePendingCancellation() {
    if (!hasPendingPlayback() || !parser_ || !voice_ || !ble_) return;
    const DisplayState& state = parser_->getDisplayState();
    VoiceContext ctx;
    ctx.isMuted = state.muted;
    ctx.isSoftMuted = state.softMuted;
    ctx.mainVolume = state.mainVolume;
    ctx.isProxyConnected = ble_->isProxyClientConnected();
    if (quiet_) {
        // The accepted job itself supplies alert content, even after the live
        // table clears. Speed suppression must still apply to that job.
        quiet_->applyVoicePresentation(ctx, speedMute_, true, pendingVoiceAction_.sourceBand);
    }
    if (voice_->isContextSuppressed(ctx)) {
        (void)voicePlaybackReceipt_.requestCancellation();
    }
}

void VoicePlaybackCoordinator::onParsedPresentation(const V1VoiceObservation& observation, const VoiceSettings& settingsRef, uint32_t nowMs) {
    servicePendingCancellation();
    const V1VoiceObservation& v1Alerts = observation;
    // A queue drain can clear and reopen an encounter before we see its idle
    // frame. Re-arm voice from the parser's episode boundary, including an
    // identical frequency, without resetting cooldown on ordinary row updates.
    const uint32_t lifetime = observation.episode;
    const bool episodeChanged = lifetime != voiceAlertLifetime_;
    voiceAlertLifetime_ = lifetime;
    if (episodeChanged || v1Alerts.count == 0) {
        awaitingVoicePlayback_ = false;
        hasPendingVoiceAction_ = false;
        voiceRetryScheduled_ = false;
        nextVoiceAttemptMs_ = 0;
        voice_->clearAllState();
    }
    if (v1Alerts.count == 0 || !settleVoicePlayback(nowMs)) {
        return;
    }

    VoiceContext voiceCtx;
    voiceCtx.alerts = v1Alerts.alerts.data();
    voiceCtx.alertCount = v1Alerts.count;
    voiceCtx.priority = v1Alerts.hasPriority ? &v1Alerts.priority : nullptr;
    voiceCtx.isMuted = observation.muted;
    voiceCtx.isSoftMuted = observation.softMuted;
    voiceCtx.isProxyConnected = observation.proxyConnected;
    voiceCtx.mainVolume = observation.mainVolume;
    voiceCtx.isSuppressed = false;
    voiceCtx.now = nowMs;

    if (quiet_) {
        quiet_->applyVoicePresentation(voiceCtx, speedMute_, v1Alerts.hasPriority,
                                       v1Alerts.hasPriority ? v1Alerts.priority.band : BAND_NONE);
    }

    const uint32_t priorityId =
        v1Alerts.hasPriority ? VoiceModule::makeAlertId(
                                  v1Alerts.priority.band, static_cast<uint16_t>(v1Alerts.priority.frequency),
                                  v1Alerts.priority.band == BAND_K && v1Alerts.priority.photoType != 0)
                            : 0;
    // An unheard secondary/escalation was selected under the old priority.
    // Re-evaluate it before its retry deadline when priority changes; otherwise
    // accepting it could restart the cooldown and silence a short new threat.
    if (hasPendingVoiceAction_ &&
        (pendingVoicePriorityId_ != priorityId ||
         !pendingVoiceActionStillCurrent(pendingVoiceAction_, v1Alerts))) {
        hasPendingVoiceAction_ = false;
        voiceRetryScheduled_ = false;
        nextVoiceAttemptMs_ = 0;
    }
    // Zero is a valid retry deadline at millis() wrap, not an unarmed timer.
    if (voiceRetryScheduled_ && static_cast<int32_t>(nowMs - nextVoiceAttemptMs_) < 0) {
        return;
    }
    voiceRetryScheduled_ = false;

    if (hasPendingVoiceAction_ && !voice_->canAnnounceContext(voiceCtx)) {
        return; // Keep the unheard action eligible when current suppression lifts.
    }

    // Busy and FailedBeforeStart have not consumed voice state. Select again
    // from this admission frame so counts, direction breakdowns and eligibility
    // reflect the current table. Once Started, settleVoicePlayback commits the
    // accepted action instead, so already-heard speech is never replayed.
    hasPendingVoiceAction_ = false;
    const VoiceAction voiceAction = voice_->prepareAction(voiceCtx);

    if (!voiceAction.hasAction()) {
        return;
    }

    const AudioPlaybackResult playbackResult = playback_.submit(voiceAction, settingsRef, voicePlaybackReceipt_);

    if (playbackResult == AudioPlaybackResult::Accepted) {
        pendingVoiceAction_ = voiceAction;
        pendingVoicePriorityId_ = priorityId;
        hasPendingVoiceAction_ = true;
        awaitingVoicePlayback_ = true;
        voiceAcceptedAtMs_ = nowMs;
        (void)settleVoicePlayback(nowMs); // Also handles a worker that already finished.
    } else if (playbackResult == AudioPlaybackResult::Busy) {
        pendingVoiceAction_ = voiceAction;
        pendingVoicePriorityId_ = priorityId;
        hasPendingVoiceAction_ = true;
        voiceRetryScheduled_ = true;
        nextVoiceAttemptMs_ = nowMs + VOICE_RETRY_INTERVAL_MS;
    } else {
        // Missing/disabled audio is terminal for this attempt. Do not commit
        // dedup state, but also do not let one unavailable clip starve newer
        // current-frame decisions forever.
        hasPendingVoiceAction_ = false;
        voiceRetryScheduled_ = true;
        nextVoiceAttemptMs_ = nowMs + 1000;
    }
}
