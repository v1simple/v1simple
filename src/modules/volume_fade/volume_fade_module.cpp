#include "volume_fade_module.h"

#include <cstring>

#include <Arduino.h>

#include "perf_metrics.h"
#include "settings.h"

VolumeFadeModule::VolumeFadeModule()
    : settings_(nullptr)
    , alertStartMs_(0)
    , originalVolume_(0xFF)
    , originalMuteVolume_(0)
    , fadeActive_(false)
    , commandSent_(false)
    , restoreLogEmitted_(false)
    , seenCount_(0)
    , pendingRestoreVolume_(0xFF)
    , pendingRestoreMuteVolume_(0)
    , pendingRestoreSetMs_(0)
    , lastRestoreAttemptMs_(0) {
    memset(seenFreqs_, 0, sizeof(seenFreqs_));
}

void VolumeFadeModule::begin(SettingsManager* settings) {
    this->settings_ = settings;
}

VolumeFadeAction VolumeFadeModule::process(const VolumeFadeContext& ctx) {
    VolumeFadeAction action;

    if (!settings_) return action;
    const V1Settings& s = settings_->get();

    // Pending-restore window: keep a short baseline carry-over if a new alert starts
    // before V1 applies the previous restore command.
    if (pendingRestoreVolume_ != 0xFF) {
        if (ctx.currentVolume == pendingRestoreVolume_ ||
            (ctx.now - pendingRestoreSetMs_) > PENDING_RESTORE_WINDOW_MS) {
            pendingRestoreVolume_ = 0xFF;
            pendingRestoreMuteVolume_ = 0;
            pendingRestoreSetMs_ = 0;
            // Restore has either converged or timed out; stop forcing restore.
            fadeActive_ = false;
        }
    }

    // External baseline hint expiry.
    if (hintBaselineVolume_ != 0xFF) {
        if (ctx.currentVolume == hintBaselineVolume_ ||
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
        const bool retryRestore = restoreInFlight && (ctx.currentVolume != originalVolume_);
        const bool shouldRestore =
            (originalVolume_ != 0xFF) &&
            (firstRestoreAttempt || retryRestore);
        if (shouldRestore) {
            const bool retryWindowOpen =
                (lastRestoreAttemptMs_ == 0) ||
                ((ctx.now - lastRestoreAttemptMs_) >= RESTORE_RETRY_MIN_INTERVAL_MS);
            if (!retryWindowOpen) {
                return action;
            }
            lastRestoreAttemptMs_ = ctx.now;
            action.type = VolumeFadeAction::Type::RESTORE;
            action.restoreVolume = originalVolume_;
            action.restoreMuteVolume = originalMuteVolume_;
            if (pendingRestoreVolume_ == 0xFF) {
                pendingRestoreVolume_ = originalVolume_;
                pendingRestoreMuteVolume_ = originalMuteVolume_;
                pendingRestoreSetMs_ = ctx.now;
            }
            if (!restoreLogEmitted_) {
                perfRecordVolumeFadeDecision(
                    PerfFadeDecision::RestoreApplied,
                    ctx.currentVolume,
                    originalVolume_,
                    ctx.now);
                Serial.printf("[VolumeFade] RESTORE: current=%d -> original=%d\n",
                              ctx.currentVolume, originalVolume_);
                restoreLogEmitted_ = true;
            }
            // Keep state — retry until V1 confirms volume restored.
            // setVolume() can fail silently due to 5ms BLE pacing gate.
            return action;
        }
        if (originalVolume_ != 0xFF) {
            if (ctx.currentVolume == originalVolume_) {
                perfRecordVolumeFadeDecision(
                    PerfFadeDecision::RestoreSkippedEqual,
                    ctx.currentVolume,
                    originalVolume_,
                    ctx.now);
                Serial.printf("[VolumeFade] Restore confirmed: current=%d == original=%d\n",
                              ctx.currentVolume, originalVolume_);
            } else if (!fadeActive_ && !restoreInFlight) {
                // No fade was active and no restore was pending; treat this as external/manual
                // ownership and drop our baseline instead of forcing a stale restore.
                perfRecordVolumeFadeDecision(
                    PerfFadeDecision::RestoreSkippedNotFaded,
                    ctx.currentVolume,
                    originalVolume_,
                    ctx.now);
            }
        }
        resetSessionState();
        return action;
    }

    // Alert muted or suppressed -> restore if we had faded, retry until confirmed.
    // BUT: if we are the ones that faded the volume (fadeActive_), the V1 may
    // report its mute indicator at low/zero volume.  Don't fight with ourselves
    // — only restore when the mute came from an external source (user mute
    // button) or the alert was suppressed by another module.
    if ((ctx.alertMuted && !fadeActive_) || ctx.alertSuppressed) {
        const bool restoreInFlight = (pendingRestoreVolume_ != 0xFF);
        const bool firstRestoreAttempt = !restoreInFlight;
        const bool retryRestore = restoreInFlight && (ctx.currentVolume != originalVolume_);
        if (originalVolume_ != 0xFF && (firstRestoreAttempt || retryRestore)) {
            const bool retryWindowOpen =
                (lastRestoreAttemptMs_ == 0) ||
                ((ctx.now - lastRestoreAttemptMs_) >= RESTORE_RETRY_MIN_INTERVAL_MS);
            if (!retryWindowOpen) {
                return action;
            }
            lastRestoreAttemptMs_ = ctx.now;
            action.type = VolumeFadeAction::Type::RESTORE;
            action.restoreVolume = originalVolume_;
            action.restoreMuteVolume = originalMuteVolume_;
            if (pendingRestoreVolume_ == 0xFF) {
                pendingRestoreVolume_ = originalVolume_;
                pendingRestoreMuteVolume_ = originalMuteVolume_;
                pendingRestoreSetMs_ = ctx.now;
            }
            if (!restoreLogEmitted_) {
                perfRecordVolumeFadeDecision(
                    PerfFadeDecision::RestoreApplied,
                    ctx.currentVolume,
                    originalVolume_,
                    ctx.now);
                restoreLogEmitted_ = true;
            }
            // Keep state — retry until V1 confirms volume restored.
            return action;
        }
        if (originalVolume_ == 0xFF) {
            perfRecordVolumeFadeDecision(
                PerfFadeDecision::RestoreSkippedNoBaseline,
                ctx.currentVolume,
                originalVolume_,
                ctx.now);
        } else {
            perfRecordVolumeFadeDecision(
                PerfFadeDecision::RestoreSkippedNotFaded,
                ctx.currentVolume,
                originalVolume_,
                ctx.now);
        }
        resetSessionState();
        return action;
    }

    unsigned long now = ctx.now;
    uint16_t freq = ctx.currentFrequency;

    // Determine if this is a new frequency during the same alert session
    bool isNewFrequency = freq != 0;
    if (isNewFrequency) {
        for (int i = 0; i < seenCount_; i++) {
            if (seenFreqs_[i] == freq) {
                isNewFrequency = false;
                break;
            }
        }
    }

    // If we're currently faded and a new frequency shows up, restore and restart timer
    if (fadeActive_ && isNewFrequency) {
        if (originalVolume_ != 0xFF) {
            action.type = VolumeFadeAction::Type::RESTORE;
            action.restoreVolume = originalVolume_;
            action.restoreMuteVolume = originalMuteVolume_;
            pendingRestoreVolume_ = originalVolume_;
            pendingRestoreMuteVolume_ = originalMuteVolume_;
            pendingRestoreSetMs_ = ctx.now;
            perfRecordVolumeFadeDecision(
                PerfFadeDecision::RestoreApplied,
                ctx.currentVolume,
                originalVolume_,
                ctx.now);
        } else {
            perfRecordVolumeFadeDecision(
                PerfFadeDecision::RestoreSkippedNoBaseline,
                ctx.currentVolume,
                originalVolume_,
                ctx.now);
        }
        alertStartMs_ = now;
        fadeActive_ = false;
        commandSent_ = false;
        if (seenCount_ < MAX_FADE_SEEN_FREQS && freq != 0) {
            seenFreqs_[seenCount_++] = freq;
        }
        return action;
    }

    // First alert in session - capture baseline volumes and start timer
    if (alertStartMs_ == 0) {
        alertStartMs_ = now;
        if (pendingRestoreVolume_ != 0xFF && ctx.currentVolume < pendingRestoreVolume_) {
            originalVolume_ = pendingRestoreVolume_;
            originalMuteVolume_ = pendingRestoreMuteVolume_;
            pendingRestoreVolume_ = 0xFF;
            pendingRestoreMuteVolume_ = 0;
            pendingRestoreSetMs_ = 0;
        } else if (hintBaselineVolume_ != 0xFF && ctx.currentVolume < hintBaselineVolume_) {
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
        if (seenCount_ < MAX_FADE_SEEN_FREQS && freq != 0) {
            seenFreqs_[seenCount_++] = freq;
        }
    }

    // Check if it's time to fade down
    unsigned long fadeDelayMs = static_cast<unsigned long>(s.alertVolumeFadeDelaySec) * 1000UL;
    if (!commandSent_ && (now - alertStartMs_) >= fadeDelayMs) {
        // Clamp minimum fade volume to 1.  Volume 0 causes the V1 to set its
        // mute indicator in image1, which feeds back through alertMuted and
        // triggers a restore→reset→re-fade cycle every fadeDelaySec.
        uint8_t fadeVol = std::max<uint8_t>(1, s.alertVolumeFadeVolume);
        if (ctx.currentVolume > fadeVol) {
            action.type = VolumeFadeAction::Type::FADE_DOWN;
            action.targetVolume = fadeVol;
            action.targetMuteVolume = originalMuteVolume_;
            fadeActive_ = true;
            perfRecordVolumeFadeDecision(
                PerfFadeDecision::FadeDown,
                ctx.currentVolume,
                originalVolume_,
                ctx.now);
        }
        commandSent_ = true;  // Do not retry if it fails; mirrors prior behavior
        return action;
    }

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
    memset(seenFreqs_, 0, sizeof(seenFreqs_));
}

void VolumeFadeModule::reset() {
    resetSessionState();
    pendingRestoreVolume_ = 0xFF;
    pendingRestoreMuteVolume_ = 0;
    pendingRestoreSetMs_ = 0;
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
