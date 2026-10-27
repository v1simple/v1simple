#include "volume_fade_module.h"

#include <cstring>

#include <Arduino.h>

#include "settings.h"

VolumeFadeModule::VolumeFadeModule()
    : settings_(nullptr), alertStartMs_(0), originalVolume_(0xFF), originalMuteVolume_(0), fadeActive_(false),
      commandSent_(false), restoreLogEmitted_(false), seenCount_(0), pendingRestoreVolume_(0xFF),
      pendingRestoreMuteVolume_(0), lastRestoreAttemptMs_(0) {
    memset(seenFreqs_, 0, sizeof(seenFreqs_));
}

void VolumeFadeModule::begin(SettingsManager* settings) {
    this->settings_ = settings;
}

bool VolumeFadeModule::observeVolumeOwnership(const VolumeFadeContext& ctx) {
    if (ctx.volumeOwnerChanged) {
        reset();
        manualChoiceActive_ = ctx.hasAlert;
        manualChoiceFrequency_ = ctx.currentFrequency;
        manualChoiceLaser_ = ctx.priorityIsLaser;
        return true;
    }

    // A canonical pair is atomic. A fresh pair outside both our baseline and
    // requested reduction belongs to the driver or another volume owner, even
    // after a fade was confirmed or while its restoration is still pending.
    // Retire the old obligation before clear/new-priority handling can send it.
    if (ctx.hasVolumeObservation && ctx.volumeObservationIngress != 0 &&
        (!volumeObservationKnown_ || ctx.sessionGeneration != volumeObservationSession_ ||
         static_cast<int32_t>(ctx.volumeObservationIngress - volumeObservationIngress_) > 0)) {
        const bool atBaseline = ctx.currentVolume == originalVolume_ &&
                                ctx.currentMuteVolume == originalMuteVolume_;
        const bool atFade = ctx.currentVolume == fadeTargetVolume_ &&
                            ctx.currentMuteVolume == fadeTargetMuteVolume_;
        if (originalVolume_ != 0xFF && !atBaseline && !atFade) {
            reset();
            manualChoiceActive_ = ctx.hasAlert;
            manualChoiceFrequency_ = ctx.currentFrequency;
            manualChoiceLaser_ = ctx.priorityIsLaser;
            return true;
        }
        volumeObservationKnown_ = true;
        volumeObservationSession_ = ctx.sessionGeneration;
        volumeObservationIngress_ = ctx.volumeObservationIngress;
    }
    return false;
}

VolumeFadeAction VolumeFadeModule::process(const VolumeFadeContext& ctx) {
    VolumeFadeAction action;

    if (!settings_)
        return action;
    const V1Settings& s = settings_->get();
    if (observeVolumeOwnership(ctx)) return action;

    // A manual choice ends this automatic override. Do not immediately acquire
    // it again and fade the same threat a second time. A distinct next threat
    // may start a fresh delay using the driver's now-current pair.
    if (manualChoiceActive_) {
        if (ctx.hasAlert && !ctx.alertEpisodeEnded &&
            ctx.currentFrequency == manualChoiceFrequency_ && ctx.priorityIsLaser == manualChoiceLaser_)
            return action;
        reset();
    }

    // BLE acceptance is not detector application. Retain the complete restore
    // pair across holdoff and live alerts until canonical observation agrees.
    if (pendingRestoreVolume_ != 0xFF) {
        if (!ctx.restoreCommandInFlight && restoreSent_ && ctx.hasVolumeObservation &&
            ctx.sessionGeneration == restoreSentSession_ &&
            static_cast<int32_t>(ctx.volumeObservationIngress - restoreSentIngress_) > 0 &&
            ctx.currentVolume == pendingRestoreVolume_ && ctx.currentMuteVolume == pendingRestoreMuteVolume_) {
            pendingRestoreVolume_ = 0xFF;
            restoreSent_ = false;
            pendingRestoreMuteVolume_ = 0;
            fadeActive_ = false;
            // A delayed restoration must still let the new threat sound before
            // fading again; do not immediately consume an already elapsed delay.
            const unsigned long delay = static_cast<unsigned long>(s.alertVolumeFadeDelaySec) * 1000UL;
            if (ctx.hasAlert && (ctx.now - alertStartMs_) >= delay) alertStartMs_ = ctx.now;
        }
    }

    // External baseline hint expiry.
    if (hintBaselineVolume_ != 0xFF) {
        if ((ctx.currentVolume == hintBaselineVolume_ && ctx.currentMuteVolume == hintBaselineMuteVolume_) ||
            (ctx.now - hintSetMs_) > HINT_WINDOW_MS) {
            hintBaselineVolume_ = 0xFF;
            hintBaselineMuteVolume_ = 0;
            hintSetMs_ = 0;
        }
    }

    // If feature disabled, clear any tracking so we don't block speed boost
    if (!s.alertVolumeFadeEnabled) {
        reset();
        return action;
    }

    // No active alerts -> restore if needed, retry until V1 confirms
    if (!ctx.hasAlert) {
        const bool restoreInFlight = (pendingRestoreVolume_ != 0xFF);
        const bool firstRestoreAttempt = fadeActive_ && !restoreInFlight;
        const bool retryRestore = restoreInFlight;
        const bool shouldRestore = (originalVolume_ != 0xFF) && (firstRestoreAttempt || retryRestore);
        if (shouldRestore) {
            const bool retryWindowOpen =
                (lastRestoreAttemptMs_ == 0) || ((ctx.now - lastRestoreAttemptMs_) >= RESTORE_RETRY_MIN_INTERVAL_MS);
            if (!retryWindowOpen) {
                return action;
            }
            lastRestoreAttemptMs_ = ctx.now;
            action.type = VolumeFadeAction::Type::RESTORE;
            action.restoreVolume = originalVolume_;
            action.restoreMuteVolume = originalMuteVolume_;
            if (pendingRestoreVolume_ == 0xFF) {
                restoreSent_ = false;
                pendingRestoreVolume_ = originalVolume_;
                pendingRestoreMuteVolume_ = originalMuteVolume_;
            }
            if (!restoreLogEmitted_) {
                Serial.printf("[VolumeFade] RESTORE: current=%d -> original=%d\n", ctx.currentVolume, originalVolume_);
                restoreLogEmitted_ = true;
            }
            // Keep state — retry until V1 confirms volume restored.
            // setVolume() can fail silently due to 5ms BLE pacing gate.
            return action;
        }
        if (originalVolume_ != 0xFF && ctx.currentVolume == originalVolume_ &&
            ctx.currentMuteVolume == originalMuteVolume_) {
            Serial.printf("[VolumeFade] Restore confirmed: current=%d == original=%d\n", ctx.currentVolume,
                          originalVolume_);
        }
        resetSessionState();
        return action;
    }

    // Restore for external mute/suppression, not our own fade-induced mute
    // indicator; retry restoration until confirmed.
    if ((ctx.alertMuted && !fadeActive_) || ctx.alertSuppressed) {
        const bool restoreInFlight = (pendingRestoreVolume_ != 0xFF);
        const bool firstRestoreAttempt = !restoreInFlight &&
                                         (ctx.currentVolume != originalVolume_ ||
                                          ctx.currentMuteVolume != originalMuteVolume_);
        const bool retryRestore = restoreInFlight;
        if (originalVolume_ != 0xFF && (firstRestoreAttempt || retryRestore)) {
            const bool retryWindowOpen =
                (lastRestoreAttemptMs_ == 0) || ((ctx.now - lastRestoreAttemptMs_) >= RESTORE_RETRY_MIN_INTERVAL_MS);
            if (!retryWindowOpen) {
                return action;
            }
            lastRestoreAttemptMs_ = ctx.now;
            action.type = VolumeFadeAction::Type::RESTORE;
            action.restoreVolume = originalVolume_;
            action.restoreMuteVolume = originalMuteVolume_;
            if (pendingRestoreVolume_ == 0xFF) {
                restoreSent_ = false;
                pendingRestoreVolume_ = originalVolume_;
                pendingRestoreMuteVolume_ = originalMuteVolume_;
            }
            if (!restoreLogEmitted_) {
                restoreLogEmitted_ = true;
            }
            // Keep state — retry until V1 confirms volume restored.
            return action;
        }
        resetSessionState();
        return action;
    }

    unsigned long now = ctx.now;
    uint16_t freq = ctx.currentFrequency;

    if (ctx.alertEpisodeEnded) {
        // A clear and reappearance can share one policy pass. Start a new
        // delay even at the same frequency, retaining any unconfirmed restore.
        if (!fadeActive_ && pendingRestoreVolume_ == 0xFF) resetSessionState();
        seenCount_ = 0;
        nextSeenFrequency_ = 0;
        seenLaser_ = false;
    }

    // A new distinct threat releases fade (Valentine's Law). Radar uses
    // frequency identity; laser is its own distinct class.
    bool isNewFrequency = freq != 0;
    if (isNewFrequency) {
        for (int i = 0; i < seenCount_; i++) {
            if (seenFreqs_[i] == freq) {
                isNewFrequency = false;
                break;
            }
        }
    }
    const bool isNewLaser = ctx.priorityIsLaser && !seenLaser_;

    // If we're currently faded and a new distinct alert shows up, restore and
    // restart the fade timer.
    if ((fadeActive_ || pendingRestoreVolume_ != 0xFF) &&
        (isNewFrequency || isNewLaser || ctx.alertEpisodeEnded)) {
        if (originalVolume_ != 0xFF) {
            action.type = VolumeFadeAction::Type::RESTORE;
            action.restoreVolume = originalVolume_;
            action.restoreMuteVolume = originalMuteVolume_;
            if (pendingRestoreVolume_ == 0xFF) restoreSent_ = false;
            pendingRestoreVolume_ = originalVolume_;
            pendingRestoreMuteVolume_ = originalMuteVolume_;
            lastRestoreAttemptMs_ = ctx.now;
        }
        alertStartMs_ = now;
        fadeActive_ = false;
        commandSent_ = false;
        if (isNewFrequency) rememberFrequency(freq);
        if (isNewLaser) {
            seenLaser_ = true;
        }
        return action;
    }

    // A new threat can remain live while its accepted restore is rejected or
    // deferred by the detector. Service that obligation before any later fade.
    if (pendingRestoreVolume_ != 0xFF) {
        if ((ctx.now - lastRestoreAttemptMs_) >= RESTORE_RETRY_MIN_INTERVAL_MS) {
            lastRestoreAttemptMs_ = ctx.now;
            action.type = VolumeFadeAction::Type::RESTORE;
            action.restoreVolume = pendingRestoreVolume_;
            action.restoreMuteVolume = pendingRestoreMuteVolume_;
        }
        return action;
    }

    // First alert in session - capture baseline volumes and start timer
    if (alertStartMs_ == 0) {
        alertStartMs_ = now;
        if (hintBaselineVolume_ != 0xFF && ctx.currentVolume < hintBaselineVolume_) {
            // Pre-quiet just restored but V1 hasn't echoed back yet.
            originalVolume_ = hintBaselineVolume_;
            originalMuteVolume_ = hintBaselineMuteVolume_;
            hintBaselineVolume_ = 0xFF;
            hintBaselineMuteVolume_ = 0;
            hintSetMs_ = 0;
        } else {
            originalVolume_ = ctx.currentVolume;
            originalMuteVolume_ = ctx.currentMuteVolume;
        }
        fadeActive_ = false;
        commandSent_ = false;
        seenCount_ = 0;
        nextSeenFrequency_ = 0;
        seenLaser_ = ctx.priorityIsLaser;
        rememberFrequency(freq);
    }

    // Check if it's time to fade down
    unsigned long fadeDelayMs = static_cast<unsigned long>(s.alertVolumeFadeDelaySec) * 1000UL;
    if (!commandSent_ && (now - alertStartMs_) >= fadeDelayMs) {
        // Clamp fade to at least 1, including configured 0. At zero, V1
        // image1 mute feedback would trigger a restore/reset/re-fade cycle.
        uint8_t fadeVol = std::max<uint8_t>(1, s.alertVolumeFadeVolume);
        if (ctx.currentVolume > fadeVol) {
            action.type = VolumeFadeAction::Type::FADE_DOWN;
            action.targetVolume = fadeVol;
            action.targetMuteVolume = originalMuteVolume_;
            fadeTargetVolume_ = action.targetVolume;
            fadeTargetMuteVolume_ = action.targetMuteVolume;
            // Priority can change during the initial delay, before there was
            // an active fade to release. The priority we actually reduce must
            // still be remembered on the next unchanged frame.
            rememberFrequency(freq);
            if (ctx.priorityIsLaser) seenLaser_ = true;
            fadeActive_ = true;
        }
        // The coordinator retains transport deferral and explicitly rejected
        // writes; this module emits only one new fade intent per encounter.
        commandSent_ = true;
        return action;
    }

    return action;
}

VolumeFadeAction VolumeFadeModule::releaseClearedAlert() {
    VolumeFadeAction action;
    if (originalVolume_ != 0xFF && (fadeActive_ || pendingRestoreVolume_ != 0xFF)) {
        action.type = VolumeFadeAction::Type::RESTORE;
        action.restoreVolume = originalVolume_;
        action.restoreMuteVolume = originalMuteVolume_;
    }
    reset();
    return action;
}

void VolumeFadeModule::resetSessionState() {
    alertStartMs_ = 0;
    originalVolume_ = 0xFF;
    originalMuteVolume_ = 0;
    fadeActive_ = false;
    commandSent_ = false;
    restoreLogEmitted_ = false;
    lastRestoreAttemptMs_ = 0;
    seenCount_ = 0;
    nextSeenFrequency_ = 0;
    seenLaser_ = false;
    fadeTargetVolume_ = 0xFF;
    fadeTargetMuteVolume_ = 0;
    volumeObservationKnown_ = false;
    manualChoiceActive_ = false;
    memset(seenFreqs_, 0, sizeof(seenFreqs_));
}

void VolumeFadeModule::rememberFrequency(uint16_t frequency) {
    if (frequency == 0) return;
    for (int i = 0; i < seenCount_; ++i) {
        if (seenFreqs_[i] == frequency) return;
    }
    seenFreqs_[nextSeenFrequency_] = frequency;
    nextSeenFrequency_ = (nextSeenFrequency_ + 1) % MAX_FADE_SEEN_FREQS;
    if (seenCount_ < MAX_FADE_SEEN_FREQS) ++seenCount_;
}

void VolumeFadeModule::reset() {
    resetSessionState();
    pendingRestoreVolume_ = 0xFF;
    restoreSent_ = false;
    pendingRestoreMuteVolume_ = 0;
    lastRestoreAttemptMs_ = 0;
    hintBaselineVolume_ = 0xFF;
    hintBaselineMuteVolume_ = 0;
    hintSetMs_ = 0;
}

void VolumeFadeModule::setBaselineHint(uint8_t mainVol, uint8_t muteVol, uint32_t nowMs) {
    hintBaselineVolume_ = mainVol;
    hintBaselineMuteVolume_ = muteVol;
    hintSetMs_ = nowMs;
}

void VolumeFadeModule::noteRestoreSent(uint32_t sessionGeneration, uint32_t ingressBoundary) {
    if (pendingRestoreVolume_ == 0xFF) return;
    // Retain the first successful write's boundary across retries, so a later
    // matching observation can still acknowledge it after another retry.
    if (!restoreSent_ || restoreSentSession_ != sessionGeneration) {
        restoreSent_ = true;
        restoreSentSession_ = sessionGeneration;
        restoreSentIngress_ = ingressBoundary;
    }
}
