#include "display_pipeline_module.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "ble_client.h"
#include "display.h"
#include "display_mode.h"
#include "modules/alert_persistence/alert_persistence_module.h"
#include "modules/alp/alp_event_latch.h"
#include "modules/alp/alp_runtime_module.h"
#include "modules/alp/alp_laser_event.h"
#include "modules/quiet/quiet_coordinator_module.h"
#include "modules/speed_mute/speed_mute_module.h"
#include "modules/speed/speed_source_selector.h"
#include "modules/voice/voice_module.h"
#include "modules/quiet/quiet_coordinator_voice_templates.h"
#include "packet_parser.h"
#include "settings.h"

// Null-object ALP event used when no runtime event is available.
static const AlpLaserEvent sAlpEventEmpty{};

namespace {

struct FrameV1Alerts {
    std::array<AlertData, RenderFrame::MAX_CARDS + 1> alerts{};
    int alertCount = 0;
    bool hasPriority = false;
    AlertData priority{};
};

bool isAlpPrimaryKind(RenderFramePrimaryKind kind) {
    return kind == RenderFramePrimaryKind::ALP_LIVE || kind == RenderFramePrimaryKind::ALP_PERSISTED;
}

bool isLivePrimaryKind(RenderFramePrimaryKind kind) {
    return kind == RenderFramePrimaryKind::V1_LIVE || kind == RenderFramePrimaryKind::ALP_LIVE;
}

bool isNormalAlpListeningHeartbeat(AlpState state, uint8_t heartbeatByte1) {
    return state == AlpState::LISTENING && (heartbeatByte1 == 0x02 || heartbeatByte1 == 0x03 || heartbeatByte1 == 0x04);
}

constexpr uint32_t kAlpListeningHoldDwellMs = 1000;

bool hasDisplayableAlpAlertContext(const AlpLaserEvent& event) {
    return event.gun != AlpGunType::UNKNOWN || event.direction != AlpLaserDirection::UNKNOWN;
}

DisplayState sanitizeDisconnectedState(const DisplayState& base) {
    DisplayState state = base;
    state.activeBands = BAND_NONE;
    state.arrows = DIR_NONE;
    state.priorityArrow = DIR_NONE;
    state.signalBars = 0;
    state.flashBits = 0;
    state.bandFlashBits = 0;
    state.v1PriorityIndex = 0;
    state.bogeyCounterByte = 0;
    state.bogeyCounterChar = '0';
    state.bogeyCounterDot = false;
    state.bogeyCounterByte2 = 0;
    state.bogeyCounterChar2 = ' ';
    state.bogeyCounterDot2 = false;
    state.hasJunkAlert = false;
    state.hasPhotoAlert = false;
    return state;
}

bool isRenderableVoiceAlert(const AlertData& alert) {
    return alert.isValid && alert.band != BAND_NONE && (alert.band == BAND_LASER || alert.frequency != 0);
}

FrameV1Alerts buildFrameV1Alerts(const RenderFrame& frame) {
    FrameV1Alerts result;
    const bool frameCarriesV1Primary = frame.primaryKind == RenderFramePrimaryKind::V1_LIVE;
    bool foundFlaggedRenderablePriority = false;

    if (frameCarriesV1Primary) {
        result.alerts[result.alertCount++] = frame.v1Priority;
        result.hasPriority = true;
        result.priority = frame.v1Priority;
    }

    for (int index = 0; index < frame.cardCount; ++index) {
        const RenderFrameCard& card = frame.cards[index];
        if (card.kind != RenderFrameCard::Kind::V1) {
            continue;
        }
        if (result.alertCount >= static_cast<int>(result.alerts.size())) {
            break;
        }
        result.alerts[result.alertCount++] = card.v1Alert;
        if (!result.hasPriority && isRenderableVoiceAlert(card.v1Alert)) {
            result.hasPriority = true;
            result.priority = card.v1Alert;
        }
        // ALP owns primary presentation, but V1's flagged row still owns its
        // speaker decision. Keep card order; use the first renderable fallback
        // only when no flagged renderable row exists.
        if (!frameCarriesV1Primary && !foundFlaggedRenderablePriority && card.v1Alert.isPriority &&
            isRenderableVoiceAlert(card.v1Alert)) {
            result.hasPriority = true;
            result.priority = card.v1Alert;
            foundFlaggedRenderablePriority = true;
        }
    }

    return result;
}

bool pendingVoiceActionStillCurrent(const VoiceAction& action, const FrameV1Alerts& alerts) {
    const auto matches = [&](const AlertData& alert) {
        const bool isPhoto = alert.band == BAND_K && alert.photoType != 0;
        return isPhoto == action.sourcePhoto && alert.band == action.sourceBand &&
               static_cast<uint16_t>(alert.frequency) == action.freq && alert.direction == action.sourceDirection;
    };

    if (action.type == VoiceAction::Type::ANNOUNCE_PRIORITY ||
        action.type == VoiceAction::Type::ANNOUNCE_DIRECTION) {
        return alerts.hasPriority && matches(alerts.priority);
    }
    for (int i = 0; i < alerts.alertCount; ++i) {
        if (matches(alerts.alerts[i])) {
            return true;
        }
    }
    return false;
}

} // namespace

void DisplayPipelineModule::begin(const DisplayPipelineDependencies& dependencies) {
    displayMode_ = dependencies.displayMode;
    display_ = dependencies.display;
    parser_ = dependencies.parser;
    settings_ = dependencies.settings;
    ble_ = dependencies.ble;
    alertPersistence_ = dependencies.alertPersistence;
    voice_ = dependencies.voice;
    speedMute_ = dependencies.speedMute;
    quiet_ = dependencies.quiet;
    alp_ = dependencies.alp;
    alpLatch_ = dependencies.alpLatch;
    speedSelector_ = dependencies.speedSelector;
    scanningPresented_ = false;
    lastPersistenceSlot_ = -1;
    alpAlertPresentation_ = AlpLaserEvent{};
    lastPresentedAlpEventActive_ = false;
    alpHoldRefreshDeadlineMs_ = 0;
    alpPersistRefreshDeadlineMs_ = 0;
    v1PersistRefreshDeadlineMs_ = 0;
    lastV1ClearRevision_ = 0;
    lastAlpEndRevision_ = 0;
    voiceAlertLifetime_ = parser_ ? parser_->alertLifetime() : 0;
    pendingVoiceAction_ = VoiceAction{};
    pendingVoicePriorityId_ = 0;
    hasPendingVoiceAction_ = false;
    awaitingVoicePlayback_ = false;
    voiceRetryScheduled_ = false;
    nextVoiceAttemptMs_ = 0;
}

void DisplayPipelineModule::updateAlpLatch(const AlpLaserEvent& alpEvent, uint32_t nowMs, uint8_t persistSec) {
    if (alpEvent.active) {
        alpPersistRefreshDeadlineMs_ = 0;
        if (alpLatch_) {
            alpLatch_->setEvent(alpEvent);
        }
    } else if (alpLatch_) {
        uint32_t endedAtMs = nowMs;
        bool ended = lastPresentedAlpEventActive_;
        if (alp_ && alp_->endedEventRevision() != lastAlpEndRevision_) {
            const AlpLaserEvent& retained = alp_->endedEvent();
            if (retained.active) {
                // The whole eligible event may have opened and closed before
                // this presentation pass. Keep its source close time so a
                // delayed drain cannot renew a finished alert's lifetime.
                alpLatch_->setEvent(retained);
                endedAtMs = retained.closedAtMs;
                ended = true;
            } else {
                alpLatch_->clearLatch(); // Runtime reinitialization.
                ended = false;
            }
        }
        if (ended) {
            alpLatch_->startPersistence(endedAtMs);
            alpPersistRefreshDeadlineMs_ = persistSec > 0
                ? std::max<uint32_t>(1, endedAtMs + static_cast<uint32_t>(persistSec) * 1000UL) : 0;
        }
    }
    if (alp_) lastAlpEndRevision_ = alp_->endedEventRevision();

    lastPresentedAlpEventActive_ = alpEvent.active;

    if (!alpEvent.active && alpLatch_ && alpLatch_->isLatched()) {
        const uint32_t persistWindowMs = static_cast<uint32_t>(persistSec) * 1000UL;
        if (!alpLatch_->shouldShowPersisted(nowMs, persistWindowMs)) {
            alpLatch_->clearLatch();
            alpPersistRefreshDeadlineMs_ = 0;
        }
    }
}

AlpLaserEvent DisplayPipelineModule::buildPresentedAlpEvent(const AlpLaserEvent& rawAlpEvent, uint32_t nowMs) {
    if (rawAlpEvent.active) {
        alpHoldRefreshDeadlineMs_ = 0;
        AlpLaserEvent next = rawAlpEvent;
        const bool sameSession = next.sessionGeneration == alpAlertPresentation_.sessionGeneration &&
                                 next.sessionGeneration != 0;
        if (sameSession && next.gun == AlpGunType::UNKNOWN &&
            alpAlertPresentation_.gun != AlpGunType::UNKNOWN) {
            next.gun = alpAlertPresentation_.gun;
        }
        if (sameSession && next.direction == AlpLaserDirection::UNKNOWN &&
            alpAlertPresentation_.direction != AlpLaserDirection::UNKNOWN) {
            next.direction = alpAlertPresentation_.direction;
        }
        if (sameSession && next.openedAtMs == 0) {
            next.openedAtMs = alpAlertPresentation_.openedAtMs;
        }
        next.closedAtMs = 0;
        alpAlertPresentation_ = next;
        return alpAlertPresentation_;
    }

    const AlpState alpState = alp_ ? alp_->getState() : AlpState::OFF;
    const uint8_t heartbeatByte1 = alp_ ? alp_->lastHeartbeatByte1() : 0xFF;
    const bool normalHeartbeat = isNormalAlpListeningHeartbeat(alpState, heartbeatByte1);
    const bool recentInactiveClose =
        rawAlpEvent.closedAtMs != 0 && (nowMs - rawAlpEvent.closedAtMs) < kAlpListeningHoldDwellMs;

    const bool holdAcrossListeningGap = alpState == AlpState::LISTENING && !normalHeartbeat && recentInactiveClose;

    if (holdAcrossListeningGap && hasDisplayableAlpAlertContext(alpAlertPresentation_)) {
        alpHoldRefreshDeadlineMs_ =
            std::max<uint32_t>(1, rawAlpEvent.closedAtMs + kAlpListeningHoldDwellMs);
        alpAlertPresentation_.active = true;
        alpAlertPresentation_.lidActive = (heartbeatByte1 == 0x04);
        alpAlertPresentation_.closedAtMs = 0;
        return alpAlertPresentation_;
    }

    alpHoldRefreshDeadlineMs_ = 0;
    alpAlertPresentation_ = AlpLaserEvent{};
    return alpAlertPresentation_;
}

bool DisplayPipelineModule::consumeAlpPresentationRefreshDue(uint32_t nowMs) {
    const bool holdDue = alpHoldRefreshDeadlineMs_ != 0 &&
                         static_cast<int32_t>(nowMs - alpHoldRefreshDeadlineMs_) >= 0;
    const bool persistDue = alpPersistRefreshDeadlineMs_ != 0 &&
                            static_cast<int32_t>(nowMs - alpPersistRefreshDeadlineMs_) >= 0;
    if (holdDue) {
        alpHoldRefreshDeadlineMs_ = 0;
    }
    if (persistDue) {
        alpPersistRefreshDeadlineMs_ = 0;
    }
    return holdDue || persistDue;
}

bool DisplayPipelineModule::v1PersistenceRefreshDue(uint32_t nowMs) const {
    return v1PersistRefreshDeadlineMs_ != 0 &&
           static_cast<int32_t>(nowMs - v1PersistRefreshDeadlineMs_) >= 0;
}

RenderFrame DisplayPipelineModule::buildRenderFrame(uint32_t nowMs, const V1Settings& settingsRef) {
    if (!ble_->isConnected()) {
        return buildDisconnectedFrame(nowMs, settingsRef);
    }
    DisplayState state = parser_->getDisplayState();
    const bool hasAlerts = parser_->hasAlerts();

    AlertData priority{};
    const bool hasRenderablePriority = hasAlerts && parser_->getRenderablePriorityAlert(priority);

    if (settingsRef.activeSlot != lastPersistenceSlot_) {
        if (lastPersistenceSlot_ >= 0) {
            // Switching policies must not revive an older completed encounter.
            lastV1ClearRevision_ = parser_->clearedAlert().revision;
        }
        lastPersistenceSlot_ = settingsRef.activeSlot;
        alertPersistence_->clearPersistence();
        v1PersistRefreshDeadlineMs_ = 0;
    }

    const uint8_t persistSec = settings_->getSlotAlertPersistSec(settingsRef.activeSlot);
    const ClearedV1Alert& cleared = parser_->clearedAlert();
    if (cleared.revision != lastV1ClearRevision_) {
        lastV1ClearRevision_ = cleared.revision;
        if (!hasAlerts) {
            alertPersistence_->clearPersistence();
            v1PersistRefreshDeadlineMs_ = 0;
            if (cleared.alert.isValid && persistSec > 0) {
                alertPersistence_->setPersistedAlert(cleared.alert);
                alertPersistence_->startPersistence(cleared.clearedAtMs);
                v1PersistRefreshDeadlineMs_ = std::max<uint32_t>(
                    1, cleared.clearedAtMs + static_cast<uint32_t>(persistSec) * 1000UL);
            }
        }
    }

    // V1 owns its last-alert identity even while ALP owns the primary display.
    // Duplicate laser suppression belongs to composition, not this source latch.
    if (hasRenderablePriority) {
        alertPersistence_->setPersistedAlert(priority);
    }

    AlertData persistedAlert = alertPersistence_->getPersistedAlert();
    bool showPersistedAlert = false;
    if (!hasAlerts) {
        if (persistSec > 0 && persistedAlert.isValid) {
            alertPersistence_->startPersistence(nowMs);
            const unsigned long persistWindowMs = static_cast<unsigned long>(persistSec) * 1000UL;
            if (alertPersistence_->shouldShowPersisted(nowMs, persistWindowMs)) {
                showPersistedAlert = true;
                if (v1PersistRefreshDeadlineMs_ == 0) {
                    v1PersistRefreshDeadlineMs_ = std::max<uint32_t>(1, nowMs + persistWindowMs);
                }
            } else {
                alertPersistence_->clearPersistence();
                persistedAlert = AlertData{};
                v1PersistRefreshDeadlineMs_ = 0;
            }
        } else {
            alertPersistence_->clearPersistence();
            persistedAlert = AlertData{};
            v1PersistRefreshDeadlineMs_ = 0;
        }
    } else {
        v1PersistRefreshDeadlineMs_ = 0;
    }

    const AlpLaserEvent& rawAlpEvent = alp_ ? alp_->currentEvent() : sAlpEventEmpty;
    const AlpLaserEvent displayAlpEvent = buildPresentedAlpEvent(rawAlpEvent, nowMs);
    // ALP uses its own persist window — not V1's slot alertPersistSec. The ALP
    // has its own speaker, so post-engagement display tail is opt-in (default 0).
    const uint8_t alpPersistSec = settings_->getAlpAlertPersistSec();
    updateAlpLatch(displayAlpEvent, nowMs, alpPersistSec);

    V1Snapshot v1;
    v1.state = state;
    v1.alerts = hasAlerts ? parser_->getAllAlerts().data() : nullptr;
    v1.alertCount = hasAlerts ? static_cast<int>(parser_->getAlertCount()) : 0;
    v1.priority = priority;
    v1.hasRenderablePriority = hasRenderablePriority;
    v1.hasPersistedAlert = showPersistedAlert;
    v1.persistedAlert = showPersistedAlert ? persistedAlert : AlertData{};

    AlpSnapshot alp;
    alp.event = displayAlpEvent;
    alp.ownsLaserDisplay = alp_ && alp_->ownsLaserDisplay();
    alp.isPersistedLatch = !displayAlpEvent.active && alpLatch_ && alpLatch_->isLatched();
    alp.latchedEvent = alp.isPersistedLatch ? alpLatch_->latchedEvent() : AlpLaserEvent{};

    return composer_.compose(v1, alp, settingsRef, nowMs);
}

// Callers inject stealth fields after buildRenderFrame() returns to keep the
// composer pure.

RenderFrame DisplayPipelineModule::buildDisconnectedFrame(uint32_t nowMs, const V1Settings& settingsRef) {
    const AlpLaserEvent& rawAlpEvent = alp_ ? alp_->currentEvent() : sAlpEventEmpty;
    const AlpLaserEvent displayAlpEvent = buildPresentedAlpEvent(rawAlpEvent, nowMs);
    // ALP persist is global, not per-slot — settingsRef is forwarded to the
    // composer below for owner-resolution only.
    const uint8_t alpPersistSec = settings_->getAlpAlertPersistSec();
    updateAlpLatch(displayAlpEvent, nowMs, alpPersistSec);

    V1Snapshot v1;
    v1.state = sanitizeDisconnectedState(parser_->getDisplayState());

    AlpSnapshot alp;
    alp.event = displayAlpEvent;
    alp.ownsLaserDisplay = alp_ && alp_->ownsLaserDisplay();
    alp.isPersistedLatch = !displayAlpEvent.active && alpLatch_ && alpLatch_->isLatched();
    alp.latchedEvent = alp.isPersistedLatch ? alpLatch_->latchedEvent() : AlpLaserEvent{};

    return composer_.compose(v1, alp, settingsRef, nowMs);
}

// Admission only reserves the worker. Voice dedup is committed once PCM can
// have started (or a missing asset is known terminal).
// Startup failure leaves this exact action unheard; current-frame validation
// below decides whether it is still eligible for another attempt.
bool DisplayPipelineModule::settleVoicePlayback(uint32_t nowMs) {
    if (!awaitingVoicePlayback_) return true;
    const auto state = voicePlaybackReceipt_.state.load();
    if (state == AudioPlaybackReceipt::State::Pending) return false;
    awaitingVoicePlayback_ = false;
    if (state == AudioPlaybackReceipt::State::FailedBeforeStart) {
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

void DisplayPipelineModule::runVoice(const RenderFrame& frame, const V1Settings& settingsRef, uint32_t nowMs) {
    const FrameV1Alerts v1Alerts = buildFrameV1Alerts(frame);
    // A queue drain can clear and reopen an encounter before we see its idle
    // frame. Re-arm voice from the parser's episode boundary, including an
    // identical frequency, without resetting cooldown on ordinary row updates.
    const uint32_t lifetime = parser_->alertLifetime();
    const bool episodeChanged = lifetime != voiceAlertLifetime_;
    voiceAlertLifetime_ = lifetime;
    if (episodeChanged || v1Alerts.alertCount == 0) {
        awaitingVoicePlayback_ = false;
        hasPendingVoiceAction_ = false;
        voiceRetryScheduled_ = false;
        nextVoiceAttemptMs_ = 0;
        voice_->clearAllState();
    }
    if (v1Alerts.alertCount == 0 || !settleVoicePlayback(nowMs)) {
        return;
    }

    VoiceContext voiceCtx;
    voiceCtx.alerts = v1Alerts.alerts.data();
    voiceCtx.alertCount = v1Alerts.alertCount;
    voiceCtx.priority = v1Alerts.hasPriority ? &v1Alerts.priority : nullptr;
    voiceCtx.isMuted = frame.context.muted;
    voiceCtx.isSoftMuted = frame.context.softMuted;
    voiceCtx.isProxyConnected = ble_->isProxyClientConnected();
    voiceCtx.mainVolume = frame.context.mainVolume;
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

    AudioPlaybackResult playbackResult = AudioPlaybackResult::Unavailable;
    switch (voiceAction.type) {
    case VoiceAction::Type::ANNOUNCE_PRIORITY:
        playbackResult = try_play_frequency_voice(voiceAction.band, voiceAction.freq, voiceAction.dir,
                                                  settingsRef.voiceAlertMode, settingsRef.voiceDirectionEnabled,
                                                  voiceAction.bogeyCount, &voicePlaybackReceipt_);
        break;

    case VoiceAction::Type::ANNOUNCE_DIRECTION:
        playbackResult = try_play_direction_only(voiceAction.dir, voiceAction.bogeyCount, &voicePlaybackReceipt_);
        break;

    case VoiceAction::Type::ANNOUNCE_SECONDARY:
        playbackResult = try_play_frequency_voice(voiceAction.band, voiceAction.freq, voiceAction.dir,
                                                  settingsRef.voiceAlertMode, settingsRef.voiceDirectionEnabled, 1,
                                                  &voicePlaybackReceipt_);
        break;

    case VoiceAction::Type::ANNOUNCE_ESCALATION:
        playbackResult = try_play_threat_escalation(voiceAction.band, voiceAction.freq, voiceAction.dir,
                                                    voiceAction.bogeyCount, voiceAction.aheadCount,
                                                    voiceAction.behindCount, voiceAction.sideCount,
                                                    settingsRef.voiceAlertMode, settingsRef.voiceDirectionEnabled,
                                                    settingsRef.announceBogeyCount, &voicePlaybackReceipt_);
        break;

    case VoiceAction::Type::NONE:
    default:
        break;
    }

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

void DisplayPipelineModule::renderComposedFrame(uint32_t nowMs, const RenderFrame& frame, bool forceRedraw) {
    const bool deferSecondaryCards =
        ble_->isConnectBurstSettling() && isLivePrimaryKind(frame.primaryKind) && frame.cardCount > 0;

    // Copy the frame only when deferring cards. Snapshot counting uses
    // deferSecondaryCards and v1AlertCount independently of frame.cardCount.
    RenderFrame deferredFrame;
    const RenderFrame* renderFramePtr = &frame;
    if (deferSecondaryCards) {
        deferredFrame = frame;
        deferredFrame.cardCount = 0;
        renderFramePtr = &deferredFrame;
    }

    AlpLaserEvent displayAlpEvent = sAlpEventEmpty;
    if (frame.primaryKind == RenderFramePrimaryKind::ALP_LIVE) {
        displayAlpEvent = frame.alpPrimary;
    } else if (frame.primaryKind == RenderFramePrimaryKind::ALP_PERSISTED) {
        displayAlpEvent = frame.alpPrimary;
        displayAlpEvent.active = false;
    }

    display_->setAlpLaserEvent(displayAlpEvent);

    // Every presentation entrypoint uses the same disconnected owner. Keep
    // SCAN between ordinary heartbeat/blink ticks; explicit restoration can
    // still repaint after a preview or another temporary screen owner.
    if (!ble_->isConnected() && !isAlpPrimaryKind(frame.primaryKind)) {
        display_->showScanning(forceRedraw || !scanningPresented_);
        scanningPresented_ = true;
        *displayMode_ = DisplayMode::IDLE;
        return;
    }

    if (forceRedraw || scanningPresented_) {
        display_->forceNextRedraw();
    }
    scanningPresented_ = false;

    const bool livePrimary = frame.primaryKind == RenderFramePrimaryKind::V1_LIVE ||
                             frame.primaryKind == RenderFramePrimaryKind::ALP_LIVE ||
                             frame.primaryKind == RenderFramePrimaryKind::ALP_PERSISTED;
    *displayMode_ = livePrimary ? DisplayMode::LIVE : DisplayMode::IDLE;

    display_->renderFrame(*renderFramePtr);
}

void DisplayPipelineModule::handleParsed(uint32_t nowMs) {
    if (!display_ || !parser_ || !settings_ || !ble_ || !alertPersistence_ || !voice_ || !displayMode_) {
        return;
    }

    const V1Settings& settingsRef = settings_->get();
    RenderFrame frame = buildRenderFrame(nowMs, settingsRef);
    if (ble_->isConnected() && frame.primaryKind == RenderFramePrimaryKind::IDLE &&
        settingsRef.stealthEnabled && speedSelector_) {
        const SpeedSelection spd = speedSelector_->selectedSpeed();
        frame.stealthMode = true;
        frame.stealthSpeedMph = spd.speedMph;
        frame.stealthSpeedValid = spd.valid;
    }
    runVoice(frame, settingsRef, nowMs);
    renderComposedFrame(nowMs, frame);
}

// Recompose without voice; persistence follows the same live source and expiry
// checks. Display caches repaint only elements affected by the new frame.
void DisplayPipelineModule::refreshBlinkTick(uint32_t nowMs) {
    if (!display_ || !parser_ || !settings_ || !ble_ || !displayMode_) {
        return;
    }
    const V1Settings& settingsRef = settings_->get();
    const bool v1PersistenceExpired = v1PersistenceRefreshDue(nowMs);
    RenderFrame frame = buildRenderFrame(nowMs, settingsRef);
    if (frame.primaryKind == RenderFramePrimaryKind::NONE) {
        return;
    }
    // Resting counters can blink after the alert table clears. Stealth owns
    // the idle screen when enabled, so its hidden counter must not wake it.
    if (ble_->isConnected() && frame.primaryKind == RenderFramePrimaryKind::IDLE &&
        settingsRef.stealthEnabled && speedSelector_) {
        if (!v1PersistenceExpired) return;
        const SpeedSelection spd = speedSelector_->selectedSpeed();
        frame.stealthMode = true;
        frame.stealthSpeedMph = spd.speedMph;
        frame.stealthSpeedValid = spd.valid;
    }
    renderComposedFrame(nowMs, frame);
}

bool DisplayPipelineModule::restoreCurrentOwner(uint32_t nowMs) {
    if (!display_ || !parser_ || !settings_ || !ble_ || !alertPersistence_ || !voice_ || !displayMode_) {
        return false;
    }

    const V1Settings& settingsRef = settings_->get();
    RenderFrame frame = buildRenderFrame(nowMs, settingsRef);
    if (ble_->isConnected() && frame.primaryKind == RenderFramePrimaryKind::IDLE &&
        settingsRef.stealthEnabled && speedSelector_) {
        const SpeedSelection spd = speedSelector_->selectedSpeed();
        frame.stealthMode = true;
        frame.stealthSpeedMph = spd.speedMph;
        frame.stealthSpeedValid = spd.valid;
    }
    renderComposedFrame(nowMs, frame, true);
    return true;
}

bool DisplayPipelineModule::allowsObdPairGesture(uint32_t nowMs) const {
    if (!displayMode_ || !parser_ || !settings_ || !alertPersistence_) {
        return false;
    }

    if (*displayMode_ != DisplayMode::IDLE) {
        return false;
    }

    if (parser_->hasAlerts()) {
        return false;
    }

    const V1Settings& s = settings_->get();
    const uint8_t persistSec = settings_->getSlotAlertPersistSec(s.activeSlot);
    if (persistSec > 0 && alertPersistence_->getPersistedAlert().isValid &&
        alertPersistence_->shouldShowPersisted(nowMs, persistSec * 1000UL)) {
        return false;
    }

    return true;
}
