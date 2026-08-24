#pragma once

#include "quiet_coordinator_module.h"

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
    syncCommittedState();

    const auto& smSettings = speedMute.getSettings();
    const auto& smState = speedMute.getState();

    const bool wantsActive = smState.muteActive;

    if (wantsActive && !speedVolActive_) {
        const DisplayState& ds = parser_->getDisplayState();
        // Baseline guard: defer DROP until V1 has delivered real volume data.
        // DisplayState::mainVolume/muteVolume default to 0 and are only valid
        // once hasVolumeData is set (packet_parser.cpp sets it on the first
        // volume-bearing display packet). Capturing ds.mainVolume before that
        // saves 0 as the "original", so the eventual RESTORE sends 0 back and
        // orphans V1 at mainVolume=0 — firing the VOL 0 warning later at rest.
        // test_speed_volume_drop_deferred_until_volume_data_received pins it.
        if (!ds.hasVolumeData) {
            updateSpeedVolPresentation(&speedMute);
            return false;
        }
        pendingSpeedVolRestoreVol_ = 0xFF;
        speedVolSavedOriginal_ = ds.mainVolume;
        speedVolSavedMuteVol_ = ds.muteVolume;
        speedVolActive_ = true;
        speedVolLastRetryMs_ = nowMs;
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
        pendingSpeedVolRestoreSetMs_ = nowMs;
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
    syncCommittedState();
    if (!volumeFade || !parser_) {
        return false;
    }

    const bool hasAlerts = parser_->hasAlerts();
    AlertData priority;
    const bool hasRenderablePriority = hasAlerts && parser_->getRenderablePriorityAlert(priority);

    VolumeFadeContext fadeCtx;
    fadeCtx.hasAlert = hasAlerts;
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

    const VolumeFadeAction fadeAction = volumeFade->process(fadeCtx);
    if (!fadeAction.hasAction()) {
        return false;
    }

    if (fadeAction.type == VolumeFadeAction::Type::FADE_DOWN) {
        sendVolume(QuietOwner::VolumeFade, fadeAction.targetVolume, fadeAction.targetMuteVolume);
        return true;
    }
    if (fadeAction.type == VolumeFadeAction::Type::RESTORE) {
        sendVolume(QuietOwner::VolumeFade, fadeAction.restoreVolume, fadeAction.restoreMuteVolume);
        return true;
    }
    return false;
}
