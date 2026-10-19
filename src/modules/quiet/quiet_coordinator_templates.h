#pragma once

#include "quiet_coordinator_module.h"

#include <cstring>

template <typename VolumeFadeLike>
void QuietCoordinatorModule::beginDetectorVolumeSession(const char* canonicalAddress, VolumeFadeLike* volumeFade) {
    const bool known = canonicalAddress && std::strlen(canonicalAddress) == 17;
    // Same-detector reconnects retain recovery. A different/unknown target may
    // never receive the old detector's baseline or deferred volume command.
    if (!known || std::strcmp(volumeDetectorAddress_, canonicalAddress) != 0) {
        resetVolumeState();
        if (volumeFade) volumeFade->reset();
    }
    if (known) std::memcpy(volumeDetectorAddress_, canonicalAddress, sizeof(volumeDetectorAddress_));
    else volumeDetectorAddress_[0] = '\0';
}

template <typename VolumeFadeLike>
bool QuietCoordinatorModule::syncProxyVolumeOwnership(VolumeFadeLike* volumeFade) {
    if (!ble_ || !ble_->isProxyClientConnected()) return false;
    // The phone owns volume, including any later restoration. Do not retain a
    // rejected local drop or an older override to replay after it disconnects.
    resetVolumeState();
    if (volumeFade) volumeFade->reset();
    return true;
}

template <typename SpeedMuteLike>
void QuietCoordinatorModule::updateSpeedVolPresentation(const SpeedMuteLike* speedMute) {
    presentation_.speedVolZeroActive = speedVolActive_ && speedMute && speedMute->getSettings().v1Volume == 0;
    if (speedVolActive_ || pendingSpeedVolRestoreVol_ != 0xFF) {
        presentation_.activeVolumeOwner = QuietOwner::SpeedVolume;
    } else if (presentation_.activeVolumeOwner == QuietOwner::SpeedVolume) {
        presentation_.activeVolumeOwner = QuietOwner::None;
    }
}

template <typename SpeedMuteLike, typename VolumeFadeLike>
bool QuietCoordinatorModule::processSpeedVolume(const uint32_t nowMs, const SpeedMuteLike& speedMute,
                                                VolumeFadeLike* volumeFade) {
    if (syncProxyVolumeOwnership(volumeFade)) return false;
    syncCommittedState();

    const auto& smSettings = speedMute.getSettings();
    const auto& smState = speedMute.getState();

    const bool wantsActive = smState.muteActive;
    bool startingSpeedVolume = false;

    if (wantsActive && !speedVolActive_) {
        const DisplayState& ds = parser_->getDisplayState();
        // Wait for hasVolumeData before DROP. Default zeroes are not a valid
        // restore baseline; saving them would later restore the detector to VOL 0.
        if (!ds.hasVolumeData) {
            updateSpeedVolPresentation(&speedMute);
            return false;
        }
        const bool restoring = pendingSpeedVolRestoreVol_ != 0xFF;
        if (!restoring) speedVolBaselineUpdated_ = desired_.volumeOwner == QuietOwner::AutoPush;
        // An accepted AutoPush may precede its detector echo. Carry that
        // pending pair; after confirmation, later observed volume is authoritative.
        const bool pendingAutoPush = speedVolBaselineUpdated_ && desired_.volumePending;
        // Re-entering low speed before restoration completes must not capture
        // the still-reduced detector pair as the user's new baseline.
        speedVolSavedOriginal_ = restoring ? pendingSpeedVolRestoreVol_ :
                                  pendingAutoPush ? desired_.volume : ds.mainVolume;
        speedVolSavedMuteVol_ = restoring ? pendingSpeedVolRestoreMuteVol_ :
                                 pendingAutoPush ? desired_.muteVolume : ds.muteVolume;
        pendingSpeedVolRestoreVol_ = 0xFF;
        speedVolActive_ = true;
        speedVolLastRetryMs_ = nowMs;
        startingSpeedVolume = true;
    }

    // Speed owns all volume writes, but must not hide the end of an alert
    // episode from fade. Carry the underlying restore pair without lifting
    // the speed override, and discard any unsent fade from the old episode.
    if (volumeFade && parser_ && (speedVolActive_ || pendingSpeedVolRestoreVol_ != 0xFF)) {
        const bool alertEnded = consumeFadeAlertEnd();
        if (!parser_->hasAlerts() || alertEnded) {
            const VolumeFadeAction cleared = volumeFade->releaseClearedAlert();
            pendingFadeAction_ = false;
            if (cleared.type == VolumeFadeAction::Type::RESTORE && !speedVolBaselineUpdated_) {
                if (speedVolActive_) {
                    speedVolSavedOriginal_ = cleared.restoreVolume;
                    speedVolSavedMuteVol_ = cleared.restoreMuteVolume;
                } else {
                    pendingSpeedVolRestoreVol_ = cleared.restoreVolume;
                    pendingSpeedVolRestoreMuteVol_ = cleared.restoreMuteVolume;
                    volumeFade->setBaselineHint(cleared.restoreVolume, cleared.restoreMuteVolume, nowMs);
                }
            }
        }
    }

    if (startingSpeedVolume) {
        sendVolume(QuietOwner::SpeedVolume, smSettings.v1Volume, speedVolSavedMuteVol_);
        Serial.printf("[SpeedVol] DROP: %d -> %d\n", speedVolSavedOriginal_, smSettings.v1Volume);
        updateSpeedVolPresentation(&speedMute);
        return true;
    }

    if (!wantsActive && speedVolActive_) {
        sendVolume(QuietOwner::SpeedVolume, speedVolSavedOriginal_, speedVolSavedMuteVol_);
        if (volumeFade) {
            volumeFade->setBaselineHint(speedVolSavedOriginal_, speedVolSavedMuteVol_, nowMs);
        }
        pendingSpeedVolRestoreVol_ = speedVolSavedOriginal_;
        pendingSpeedVolRestoreMuteVol_ = speedVolSavedMuteVol_;
        pendingSpeedVolRestoreLastRetryMs_ = nowMs;
        Serial.printf("[SpeedVol] RESTORE: -> %d\n", speedVolSavedOriginal_);
        speedVolActive_ = false;
        speedVolSavedOriginal_ = 0xFF;
        updateSpeedVolPresentation(&speedMute);
        return retryPendingSpeedVolRestore(nowMs);
    }

    if (speedVolActive_) {
        if (committed_.mainVolume == smSettings.v1Volume && committed_.muteVolume == speedVolSavedMuteVol_) {
            updateSpeedVolPresentation(&speedMute);
            return true;
        }
        if ((nowMs - speedVolLastRetryMs_) >= SPEED_VOL_RETRY_INTERVAL_MS) {
            speedVolLastRetryMs_ = nowMs;
            sendVolume(QuietOwner::SpeedVolume, smSettings.v1Volume, speedVolSavedMuteVol_);
        }
        updateSpeedVolPresentation(&speedMute);
        return true;
    }

    updateSpeedVolPresentation(&speedMute);
    return retryPendingSpeedVolRestore(nowMs);
}

template <typename VolumeFadeLike>
bool QuietCoordinatorModule::executeVolumeFade(const uint32_t nowMs, VolumeFadeLike* volumeFade) {
    if (syncProxyVolumeOwnership(volumeFade)) return false;
    syncCommittedState();
    if (!volumeFade || !parser_) {
        return false;
    }
    // Do not even advance fade's state machine while Apply is waiting for the
    // focused 0x38 readback. Processing here could capture the old pair as a
    // restore baseline and undo the verified profile immediately afterward.
    if (autoPushVolumeTransactionActive_) {
        return false;
    }
    // An alert may arrive before the first volume-bearing packet. Do not
    // capture the default 0/0 pair as a restore baseline; a received zero
    // remains a valid user volume, as it does for the speed-volume owner.
    if (!parser_->getDisplayState().hasVolumeData) {
        return false;
    }

    const bool hasAlerts = parser_->hasAlerts();
    AlertData priority;
    const bool hasRenderablePriority = hasAlerts && parser_->getRenderablePriorityAlert(priority);

    VolumeFadeContext fadeCtx;
    fadeCtx.hasAlert = hasAlerts;
    fadeCtx.alertEpisodeEnded = consumeFadeAlertEnd();
    fadeCtx.currentVolume = committed_.mainVolume;
    fadeCtx.currentMuteVolume = committed_.muteVolume;
    fadeCtx.now = nowMs;
    if (hasAlerts) {
        fadeCtx.alertMuted = committed_.muted;
        fadeCtx.alertSuppressed = false;
        fadeCtx.currentFrequency = hasRenderablePriority ? static_cast<uint16_t>(priority.frequency) : 0;
        // Laser has no frequency; flag it so a laser arriving mid-fade counts
        // as a new distinct alert and releases the fade (screen/speaker
        // contract: new threat, new sound).
        fadeCtx.priorityIsLaser = hasRenderablePriority && priority.band == BAND_LASER;
    }

    if (fadeCtx.alertEpisodeEnded && hasAlerts) {
        // Re-evaluate the preceding episode's deferred command against its
        // retained baseline before any retry can lower the new encounter.
        pendingFadeAction_ = false;
    }
    if (pendingFadeAction_) {
        const bool staleFadeDown = !fadeCtx.hasAlert || fadeCtx.alertMuted || fadeCtx.alertSuppressed ||
                                   fadeCtx.currentFrequency != pendingFadeFrequency_ ||
                                   fadeCtx.priorityIsLaser != pendingFadeLaser_;
        if (staleFadeDown) {
            pendingFadeAction_ = false;
        } else {
            if (static_cast<uint32_t>(nowMs - pendingFadeLastAttemptMs_) < FADE_RETRY_INTERVAL_MS) {
                return true;
            }
            pendingFadeLastAttemptMs_ = nowMs;
            const SendResult retry =
                sendVolumeResult(QuietOwner::VolumeFade, pendingFadeVolume_, pendingFadeMuteVolume_);
            if (retry == SendResult::NOT_YET) {
                return true;
            }
            pendingFadeAction_ = false;
            return retry == SendResult::SENT;
        }
    }

    const VolumeFadeAction fadeAction = volumeFade->process(fadeCtx);
    // VolumeFade owns the detector pair for as long as it retains a captured
    // baseline or a pending restore. Mirror that internal state even when the
    // attempted fade-down did not reach the transport: a later restore could
    // otherwise overwrite a profile Apply that was admitted in between.
    if (volumeFade->hasActiveVolumeOverride()) {
        presentation_.activeVolumeOwner = QuietOwner::VolumeFade;
    } else if (presentation_.activeVolumeOwner == QuietOwner::VolumeFade) {
        presentation_.activeVolumeOwner = QuietOwner::None;
    }
    if (!fadeAction.hasAction()) {
        return false;
    }

    if (fadeAction.type == VolumeFadeAction::Type::FADE_DOWN) {
        const SendResult result =
            sendVolumeResult(QuietOwner::VolumeFade, fadeAction.targetVolume, fadeAction.targetMuteVolume);
        if (result == SendResult::NOT_YET) {
            pendingFadeAction_ = true;
            pendingFadeVolume_ = fadeAction.targetVolume;
            pendingFadeMuteVolume_ = fadeAction.targetMuteVolume;
            pendingFadeFrequency_ = fadeCtx.currentFrequency;
            pendingFadeLaser_ = fadeCtx.priorityIsLaser;
            pendingFadeLastAttemptMs_ = nowMs;
        }
        return true;
    }
    if (fadeAction.type == VolumeFadeAction::Type::RESTORE) {
        // Fade owns restore retries through canonical convergence, including
        // SENT-but-unapplied writes. Do not create a second retry clock here.
        sendVolumeResult(QuietOwner::VolumeFade, fadeAction.restoreVolume, fadeAction.restoreMuteVolume);
        return true;
    }
    return false;
}
