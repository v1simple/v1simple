#include "quiet_coordinator_module.h"

#ifndef UNIT_TEST
#include "../../ble_client.h"
#include "../../packet_parser.h"
#else
#include "../../../test/mocks/ble_client.h"
#include "../../../test/mocks/packet_parser.h"
#endif

const char* quietOwnerName(const QuietOwner owner) {
    switch (owner) {
    case QuietOwner::None:
        return "none";
    case QuietOwner::SpeedVolume:
        return "speed_volume";
    case QuietOwner::VolumeFade:
        return "volume_fade";
    case QuietOwner::TapGesture:
        return "tap_gesture";
    case QuietOwner::WifiCommand:
        return "wifi_command";
    case QuietOwner::AutoPush:
        return "auto_push";
    case QuietOwner::InTheBox:
        return "in_the_box";
    default:
        return "unknown";
    }
}

void QuietCoordinatorModule::begin(V1BLEClient* bleClient, PacketParser* parser) {
    ble_ = bleClient;
    parser_ = parser;
    volumeDetectorAddress_[0] = '\0';
    reset();
}

void QuietCoordinatorModule::reset() {
    desired_ = QuietDesiredState{};
    committed_ = QuietCommittedState{};
    presentation_ = QuietPresentationState{};

    resetVolumeState();
    resetInTheBoxSession();

    syncCommittedState();
}

void QuietCoordinatorModule::resetVolumeState() {
    desired_.volumeOwner = QuietOwner::None;
    desired_.volumePending = false;
    desired_.volume = 0xFF;
    desired_.muteVolume = 0;
    presentation_.activeVolumeOwner = QuietOwner::None;
    presentation_.speedVolZeroActive = false;

    speedVolActive_ = false;
    speedVolSavedOriginal_ = 0xFF;
    speedVolSavedMuteVol_ = 0;
    speedVolBaselineUpdated_ = false;
    pendingSpeedVolRestoreVol_ = 0xFF;
    pendingSpeedVolRestoreMuteVol_ = 0;
    pendingSpeedVolRestoreLastRetryMs_ = 0;
    speedRestoreSent_ = false;
    speedRestoreHintInvalidated_ = false;
    speedVolLastRetryMs_ = 0;
    pendingFadeAction_ = false;
    pendingFadeVolume_ = 0;
    pendingFadeMuteVolume_ = 0;
    pendingFadeFrequency_ = 0;
    pendingFadeLaser_ = false;
    pendingFadeLastAttemptMs_ = 0;
    pendingFadeSession_ = 0;
    fadeWriteRevision_ = 0;
    fadeWriteIngress_ = 0;
    fadeAuthorityWriteRevision_ = 0;
    fadeAuthoritySession_ = 0;
    fadeAlertLifetimeKnown_ = false;
    autoPushVolumeTransactionActive_ = false;
}

bool QuietCoordinatorModule::consumeFadeAlertEnd() {
    const uint32_t lifetime = parser_->alertLifetime();
    const uint32_t session = ble_ ? ble_->sessionGeneration() : 0;
    const uint32_t transitions = lifetime - fadeAlertLifetime_;
    // A single transition ending in alerts is only a start. Two or more
    // transitions include a clear even when the queue's final state is live.
    // Session resets are separate: same-detector reconnects retain recovery.
    const bool ended = fadeAlertLifetimeKnown_ && session == fadeAlertSession_ &&
                       (transitions > 1 || (transitions == 1 && !parser_->hasAlerts()));
    fadeAlertLifetime_ = lifetime;
    fadeAlertSession_ = session;
    fadeAlertLifetimeKnown_ = true;
    return ended;
}

void QuietCoordinatorModule::syncCommittedState() {
    committed_.connected = ble_ ? ble_->isConnected() : false;
    committed_.hasDisplayState = false;
    committed_.muted = false;
    committed_.mainVolume = 0;
    committed_.muteVolume = 0;

    if (parser_) {
        const DisplayState& state = parser_->getDisplayState();
        committed_.hasDisplayState = true;
        committed_.muted = state.muted;
        committed_.mainVolume = state.mainVolume;
        committed_.muteVolume = state.muteVolume;
        presentation_.effectiveMuted = state.muted;
    } else {
        presentation_.effectiveMuted = false;
    }

    refreshPendingState();
}

void QuietCoordinatorModule::refreshPendingState() {
    // Command convergence follows the audio gate; the debounced mute LED
    // remains a separate presentation input.
    if (desired_.mutePending && parser_ && parser_->getDisplayState().softMuted == desired_.mute) {
        desired_.mutePending = false;
    }
    if (desired_.volumePending && committed_.hasDisplayState && committed_.mainVolume == desired_.volume &&
        committed_.muteVolume == desired_.muteVolume) {
        desired_.volumePending = false;
    }
}

QuietCommittedState QuietCoordinatorModule::getCommittedState() {
    syncCommittedState();
    return committed_;
}

bool QuietCoordinatorModule::sendMute(QuietOwner owner, bool muted) {
    return sendMuteResult(owner, muted) == SendResult::SENT;
}

SendResult QuietCoordinatorModule::sendMuteResult(QuietOwner owner, bool muted) {
    syncCommittedState();

    desired_.muteOwner = owner;
    desired_.mute = muted;
    desired_.mutePending = parser_ ? (parser_->getDisplayState().softMuted != muted) : true;

    if (!ble_) {
        return SendResult::FAILED;
    }

    const SendResult result = ble_->setMuteResult(muted);
    if (result == SendResult::SENT) {
        presentation_.activeMuteOwner = muted ? owner : QuietOwner::None;
        if (owner != QuietOwner::InTheBox) {
            // An explicit local mute/unmute supersedes our pending command.
            // Never release a later manual mute as though it were our own.
            inTheBoxOwnsMute_ = false;
            inTheBoxMuteConfirmed_ = false;
            inTheBoxManualOverride_ = true;
            inTheBoxInsidePending_ = false;
            inTheBoxCommandPending_ = false;
            inTheBoxAttempted_ = false;
            inTheBoxSuppressVoice_ = false;
        }
    }
    return result;
}

void QuietCoordinatorModule::resetInTheBoxSession() {
    inTheBoxSettings_ = V1InTheBoxSettings{};
    inTheBox_.reset();
    inTheBoxSession_ = 0;
    inTheBoxLifetimeKnown_ = false;
    inTheBoxOwnsMute_ = false;
    inTheBoxManualOverride_ = false;
    inTheBoxInsidePending_ = false;
    inTheBoxCommandPending_ = false;
    inTheBoxCommandSent_ = false;
    inTheBoxMuteConfirmed_ = false;
    inTheBoxAttempted_ = false;
    inTheBoxSuppressVoice_ = false;
    if (presentation_.activeMuteOwner == QuietOwner::InTheBox) {
        presentation_.activeMuteOwner = QuietOwner::None;
    }
}

void QuietCoordinatorModule::setInTheBoxSettings(const V1InTheBoxSettings& settings,
                                                uint32_t sessionGeneration) {
    if (sessionGeneration != inTheBoxSession_) resetInTheBoxSession();
    inTheBoxSettings_ = isValidV1InTheBoxSettings(settings) ? settings : V1InTheBoxSettings{};
    inTheBoxSession_ = sessionGeneration;
    inTheBoxLifetimeKnown_ = false;
    inTheBox_.reset();
    inTheBoxManualOverride_ = false;
    inTheBoxInsidePending_ = false;
    inTheBoxCommandPending_ = false;
    inTheBoxAttempted_ = false;
    // Preserve same-session mute ownership so disabling/changing this policy
    // can release the mute we already sent to that detector.
}

void QuietCoordinatorModule::processInTheBox(uint32_t nowMs) {
    inTheBoxSuppressVoice_ = false;
    if (!ble_ || !parser_) return;
    if (!ble_->isConnected() || ble_->sessionGeneration() != inTheBoxSession_) {
        resetInTheBoxSession();
        return;
    }
    if (ble_->isProxyClientConnected()) {
        // The connected phone owns the detector. Do not queue a command that
        // could unexpectedly take effect when the phone disconnects.
        inTheBoxOwnsMute_ = false;
        inTheBoxInsidePending_ = false;
        inTheBoxCommandPending_ = false;
        inTheBoxManualOverride_ = true;
        inTheBoxAttempted_ = false;
        if (presentation_.activeMuteOwner == QuietOwner::InTheBox)
            presentation_.activeMuteOwner = QuietOwner::None;
        return;
    }

    const auto& state = parser_->getDisplayState();
    const auto& observation = parser_->displayOnObservation();
    if (!observation.available) return;

    const uint32_t lifetime = parser_->alertLifetime();
    const bool lifetimeChanged = inTheBoxLifetimeKnown_ && lifetime != inTheBoxLifetime_;
    if (lifetimeChanged) {
        // The queue can deliver a clear and a new encounter in one drain.
        // Do not carry an old manual override or inside-seen identity across it.
        inTheBox_.reset();
        inTheBoxManualOverride_ = false;
        inTheBoxInsidePending_ = false;
    }
    inTheBoxLifetime_ = lifetime;
    inTheBoxLifetimeKnown_ = true;

    // Confirm only from a later canonical display packet, using the actual
    // audio-mute bit (isSoft), rather than the debounced screen icon.
    if (inTheBoxCommandPending_ && inTheBoxCommandSent_ &&
        observation.revision != inTheBoxCommandRevision_ &&
        static_cast<int32_t>(observation.ingressSequence - inTheBoxCommandIngress_) > 0 &&
        state.softMuted == inTheBoxCommandMute_) {
        inTheBoxCommandPending_ = false;
        inTheBoxCommandSent_ = false;
        inTheBoxMuteConfirmed_ = inTheBoxCommandMute_;
        if (!inTheBoxCommandMute_) {
            inTheBoxOwnsMute_ = false;
            inTheBoxInsidePending_ = false;
        }
    }
    if (inTheBoxOwnsMute_ && inTheBoxMuteConfirmed_ && !state.softMuted &&
        !inTheBoxCommandPending_ && !lifetimeChanged) {
        // A later detector-side unmute wins too. The protocol cannot tell us
        // whether it came from its button or its own alert logic; re-muting
        // either would override newly observed detector behavior.
        inTheBoxOwnsMute_ = false;
        inTheBoxMuteConfirmed_ = false;
        inTheBoxManualOverride_ = true;
        if (presentation_.activeMuteOwner == QuietOwner::InTheBox)
            presentation_.activeMuteOwner = QuietOwner::None;
    }

    InTheBoxDecision decision;
    const bool fresh = !inTheBoxSuspended_ && parser_->hasFreshAlertTable(nowMs);
    if (fresh) {
        const auto& alerts = parser_->getAllAlerts();
        decision = inTheBox_.process(inTheBoxSettings_, alerts.data(),
                                     static_cast<size_t>(parser_->getAlertCount()),
                                     (state.activeBands & BAND_LASER) != 0);
        if (!parser_->hasAlerts()) inTheBoxManualOverride_ = false;
        inTheBoxInsidePending_ = decision.anyInsideUnmuteEligible &&
                                (inTheBoxInsidePending_ || decision.newInsideUnmute);
    } else {
        // Missing/stale data may release our mute, never create a new mute or
        // an explicit inside-unmute. Keep encounter history through gaps.
        inTheBoxInsidePending_ = false;
    }

    bool haveTarget = false;
    bool targetMute = false;
    if (inTheBoxOwnsMute_ && !decision.allOutsideMuteEligible) {
        haveTarget = true;
    } else if (inTheBoxInsidePending_) {
        haveTarget = true;
    } else if (decision.allOutsideMuteEligible && !inTheBoxManualOverride_ &&
               (inTheBoxOwnsMute_ || !state.softMuted)) {
        haveTarget = true;
        targetMute = true;
    }
    inTheBoxSuppressVoice_ = haveTarget && targetMute;

    if (!haveTarget) {
        inTheBoxCommandPending_ = false;
        inTheBoxAttempted_ = false;
        return;
    }
    if (!inTheBoxCommandPending_ && targetMute && state.softMuted) return;
    if (!inTheBoxCommandPending_ || targetMute != inTheBoxCommandMute_) {
        // A reversal is immediate, even inside the retry interval. A queued
        // outside-mute must not outlive the arrival of an inside/laser alert.
        inTheBoxCommandPending_ = true;
        inTheBoxCommandMute_ = targetMute;
        inTheBoxCommandSent_ = false;
        inTheBoxAttempted_ = false;
    }
    if (!inTheBoxCommandSent_ && state.softMuted == targetMute && !inTheBoxOwnsMute_) {
        inTheBoxCommandPending_ = false;
        inTheBoxInsidePending_ = false;
        return;
    }
    if (inTheBoxAttempted_ && static_cast<uint32_t>(nowMs - inTheBoxLastAttemptMs_) < IN_THE_BOX_RETRY_MS)
        return;
    inTheBoxAttempted_ = true;
    inTheBoxLastAttemptMs_ = nowMs;
    const uint32_t beforeRevision = observation.revision;
    const uint32_t beforeIngress = ble_->latestV1NotificationIngressSequence();
    const SendResult result = sendMuteResult(QuietOwner::InTheBox, targetMute);
    if (result == SendResult::SENT) {
        inTheBoxCommandSent_ = true;
        inTheBoxCommandRevision_ = beforeRevision;
        inTheBoxCommandIngress_ = beforeIngress;
        if (targetMute) inTheBoxOwnsMute_ = true;
    }
}

bool QuietCoordinatorModule::sendVolume(QuietOwner owner, uint8_t volume, uint8_t muteVolume) {
    return sendVolumeResult(owner, volume, muteVolume) == SendResult::SENT;
}

SendResult QuietCoordinatorModule::sendVolumeResult(QuietOwner owner, uint8_t volume, uint8_t muteVolume) {
    syncCommittedState();

    if (autoPushVolumeTransactionActive_ && owner != QuietOwner::AutoPush) {
        return SendResult::NOT_YET;
    }

    desired_.volumeOwner = owner;
    desired_.volume = volume;
    desired_.muteVolume = muteVolume;
    desired_.volumePending =
        committed_.hasDisplayState ? (committed_.mainVolume != volume || committed_.muteVolume != muteVolume) : true;

    if (!ble_) {
        return SendResult::FAILED;
    }

    const SendResult result = ble_->setVolumeResult(volume, muteVolume);
    if (result == SendResult::SENT) {
        presentation_.activeVolumeOwner = owner;
    }
    return result;
}

bool QuietCoordinatorModule::beginAutoPushVolumeTransaction() {
    if (autoPushVolumeTransactionActive_ || !canApplyAutoPushVolumeExactly()) return false;
    autoPushVolumeTransactionActive_ = true;
    return true;
}

bool QuietCoordinatorModule::sendAutoPushVolume(uint8_t volume, uint8_t muteVolume) {
    return sendAutoPushVolume(volume, muteVolume, 0);
}

bool QuietCoordinatorModule::sendAutoPushVolume(uint8_t volume, uint8_t muteVolume, uint8_t aux0) {
    return sendAutoPushVolumeResult(volume, muteVolume, aux0) == SendResult::SENT;
}

SendResult QuietCoordinatorModule::sendAutoPushVolumeResult(uint8_t volume, uint8_t muteVolume, uint8_t aux0) {
    if (volume > 9 || muteVolume > 9) {
        return SendResult::FAILED;
    }

    syncCommittedState();
    if (!speedVolActive_) {
        if (!ble_) return SendResult::FAILED;
        const uint32_t session = ble_->sessionGeneration();
        const uint32_t ingress = ble_->latestV1NotificationIngressSequence();
        const SendResult result = ble_->setVolumeResult(volume, muteVolume, aux0);
        if (result != SendResult::SENT) return result;
        // A deferred or failed Apply did not change the detector. Preserve
        // its baseline so later speed-volume restoration cannot apply a pair
        // that this transaction never transmitted.
        if (pendingSpeedVolRestoreVol_ != 0xFF) {
            setPendingSpeedRestore(volume, muteVolume);
            // This successful app write is newer than any already queued
            // canonical observation. Only subsequent choices may retire it.
            speedRestoreObservationSession_ = session;
            speedRestoreObservationIngress_ = ingress;
            noteSpeedRestoreSent(session, ingress);
            speedVolBaselineUpdated_ = true;
        }
        desired_.volumeOwner = QuietOwner::AutoPush;
        desired_.volume = volume;
        desired_.muteVolume = muteVolume;
        desired_.volumePending = true;
        presentation_.activeVolumeOwner = QuietOwner::AutoPush;
        return SendResult::SENT;
    }

    if (!ble_) {
        return SendResult::FAILED;
    }

    // REQWRITEVOLUME is atomic. Keep the temporary speed-mute main volume on
    // the detector while replacing the baseline pair that will be restored.
    const uint8_t temporaryVolume = desired_.volume <= 9 ? desired_.volume : committed_.mainVolume;
    const SendResult result = ble_->setVolumeResult(temporaryVolume, muteVolume, aux0);
    if (result != SendResult::SENT) {
        return result;
    }

    speedVolSavedOriginal_ = volume;
    speedVolSavedMuteVol_ = muteVolume;
    speedVolBaselineUpdated_ = true;
    desired_.volumeOwner = QuietOwner::SpeedVolume;
    desired_.volume = temporaryVolume;
    desired_.muteVolume = muteVolume;
    desired_.volumePending =
        !committed_.hasDisplayState || committed_.mainVolume != temporaryVolume || committed_.muteVolume != muteVolume;
    presentation_.activeVolumeOwner = QuietOwner::SpeedVolume;
    return SendResult::SENT;
}

bool QuietCoordinatorModule::retryPendingSpeedVolRestore(const uint32_t nowMs) {
    syncCommittedState();
    reconcilePendingSpeedRestore();
    if (pendingSpeedVolRestoreVol_ == 0xFF || !ble_ || !parser_) {
        return false;
    }

    // A deferred or accepted write is still owed until the canonical pair
    // converges. Time spent in protocol holdoff cannot release that ownership.
    if (!parser_->getDisplayState().hasVolumeData || !committed_.connected) return true;
    if ((nowMs - pendingSpeedVolRestoreLastRetryMs_) < SPEED_VOL_RETRY_INTERVAL_MS) {
        return true;
    }

    pendingSpeedVolRestoreLastRetryMs_ = nowMs;
    const uint32_t session = ble_->sessionGeneration();
    const uint32_t ingressBoundary = ble_->latestV1NotificationIngressSequence();
    if (sendVolume(QuietOwner::SpeedVolume, pendingSpeedVolRestoreVol_, pendingSpeedVolRestoreMuteVol_))
        noteSpeedRestoreSent(session, ingressBoundary);
    return true;
}

void QuietCoordinatorModule::setPendingSpeedRestore(uint8_t volume, uint8_t muteVolume) {
    if (pendingSpeedVolRestoreVol_ == 0xFF && ble_) {
        // Preserve the temporary pair separately from the baseline. Delayed
        // reduction feedback still permits a retry; a later different atomic
        // pair belongs to the driver, even when restoration was deferred.
        speedRestoreReducedVolume_ = desired_.volume;
        speedRestoreReducedMuteVolume_ = desired_.muteVolume;
        speedRestorePreviousVolume_ = 0xFF;
        speedRestoreObservationSession_ = ble_->sessionGeneration();
        speedRestoreObservationIngress_ = ble_->latestV1NotificationIngressSequence();
    }
    if (ble_) speedRestoreWriteRevision_ = ble_->volumeWriteRevision();
    if (pendingSpeedVolRestoreVol_ != volume || pendingSpeedVolRestoreMuteVol_ != muteVolume) {
        if (speedRestoreSent_) {
            // Fade clear or a newer app pair can replace an accepted restore.
            // Its late echo is still our own write, not a manual choice.
            speedRestorePreviousVolume_ = pendingSpeedVolRestoreVol_;
            speedRestorePreviousMuteVolume_ = pendingSpeedVolRestoreMuteVol_;
        }
        speedRestoreSent_ = false;
    }
    pendingSpeedVolRestoreVol_ = volume;
    pendingSpeedVolRestoreMuteVol_ = muteVolume;
}

void QuietCoordinatorModule::noteSpeedRestoreSent(uint32_t sessionGeneration, uint32_t ingressBoundary) {
    speedRestoreWriteRevision_ = ble_->volumeWriteRevision();
    if (!speedRestoreSent_ || speedRestoreSentSession_ != sessionGeneration) {
        speedRestoreSent_ = true;
        speedRestoreSentSession_ = sessionGeneration;
        speedRestoreSentIngress_ = ingressBoundary;
    }
}

void QuietCoordinatorModule::reconcilePendingSpeedRestore(bool allowConfirmation) {
    if (pendingSpeedVolRestoreVol_ == 0xFF || !ble_ || !parser_ || !committed_.connected) return;

    const uint32_t session = ble_->sessionGeneration();
    // Same-detector reconnects retain the obligation but require observations
    // from their new session. The parser invalidates its pair at session open.
    const bool newSession = session != speedRestoreObservationSession_;
    const bool newerWriter = !newSession && ble_->volumeWriteRevision() != speedRestoreWriteRevision_;
    uint8_t mainVolume = 0, muteVolume = 0;
    uint32_t ingress = 0;
    const bool fresh = parser_->copyLatestCanonicalCurrentVolume(mainVolume, muteVolume, &ingress) &&
        (newSession || static_cast<int32_t>(ingress - speedRestoreObservationIngress_) > 0);
    const bool atTarget = mainVolume == pendingSpeedVolRestoreVol_ && muteVolume == pendingSpeedVolRestoreMuteVol_;
    const bool atReduction = mainVolume == speedRestoreReducedVolume_ && muteVolume == speedRestoreReducedMuteVolume_;
    const bool atPrevious = mainVolume == speedRestorePreviousVolume_ && muteVolume == speedRestorePreviousMuteVolume_;
    const bool manualPair = fresh && !atTarget && !atReduction && !atPrevious;
    const bool confirmed = allowConfirmation && fresh && atTarget && speedRestoreSent_ && speedRestoreSentSession_ == session &&
        static_cast<int32_t>(ingress - speedRestoreSentIngress_) > 0;
    if (!newerWriter && !manualPair && !confirmed) {
        if (newSession && fresh) {
            speedRestoreObservationSession_ = session;
            speedRestoreObservationIngress_ = ingress;
            speedRestoreWriteRevision_ = ble_->volumeWriteRevision();
        }
        return;
    }
    pendingSpeedVolRestoreVol_ = 0xFF;
    speedRestoreSent_ = false;
    speedRestoreHintInvalidated_ = newerWriter || manualPair;
    if ((newerWriter || manualPair) && (desired_.volumeOwner == QuietOwner::SpeedVolume ||
        (!newerWriter && desired_.volumeOwner == QuietOwner::AutoPush))) {
        desired_.volumeOwner = QuietOwner::None;
        desired_.volumePending = false;
    }
    if (presentation_.activeVolumeOwner == QuietOwner::SpeedVolume)
        presentation_.activeVolumeOwner = QuietOwner::None;
    presentation_.speedVolZeroActive = false;
}
