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

DisplayState sanitizeDisconnectedRestoreState(const DisplayState& base) {
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

AlertData alpEventToSyntheticAlert(const AlpLaserEvent& event) {
    AlertData alert;
    alert.isValid = true;
    alert.band = BAND_LASER;
    alert.frequency = 0;
    switch (event.direction) {
    case AlpLaserDirection::FRONT:
        alert.direction = DIR_FRONT;
        break;

    case AlpLaserDirection::REAR:
        alert.direction = DIR_REAR;
        break;

    case AlpLaserDirection::UNKNOWN:
    default:
        alert.direction = DIR_NONE;
        break;
    }
    alert.frontStrength = 6;
    return alert;
}

FrameV1Alerts buildFrameV1Alerts(const RenderFrame& frame) {
    FrameV1Alerts result;

    if (frame.primaryKind == RenderFramePrimaryKind::V1_LIVE) {
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
        if (!result.hasPriority) {
            result.hasPriority = true;
            result.priority = card.v1Alert;
        }
    }

    return result;
}

bool pendingVoiceActionStillCurrent(const VoiceAction& action, const FrameV1Alerts& alerts) {
    const auto matches = [&](const AlertData& alert) {
        return alert.band == action.sourceBand && static_cast<uint16_t>(alert.frequency) == action.freq &&
               alert.direction == action.sourceDirection;
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
    lastPersistenceSlot_ = -1;
    alpAlertPresentation_ = AlpLaserEvent{};
    lastPresentedAlpEventActive_ = false;
    alpHoldRefreshDeadlineMs_ = 0;
    alpPersistRefreshDeadlineMs_ = 0;
    pendingVoiceAction_ = VoiceAction{};
    hasPendingVoiceAction_ = false;
    nextVoiceAttemptMs_ = 0;
}

void DisplayPipelineModule::updateAlpLatch(const AlpLaserEvent& alpEvent, uint32_t nowMs, uint8_t persistSec) {
    if (alpEvent.active) {
        alpPersistRefreshDeadlineMs_ = 0;
        if (alpLatch_) {
            alpLatch_->setEvent(alpEvent);
        }
    } else if (alpLatch_ && lastPresentedAlpEventActive_) {
        alpLatch_->startPersistence(nowMs);
        alpPersistRefreshDeadlineMs_ =
            persistSec > 0 ? std::max<uint32_t>(1, nowMs + static_cast<uint32_t>(persistSec) * 1000UL) : 0;
    }

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
        if (next.gun == AlpGunType::UNKNOWN && alpAlertPresentation_.gun != AlpGunType::UNKNOWN) {
            next.gun = alpAlertPresentation_.gun;
        }
        if (next.direction == AlpLaserDirection::UNKNOWN &&
            alpAlertPresentation_.direction != AlpLaserDirection::UNKNOWN) {
            next.direction = alpAlertPresentation_.direction;
        }
        if (next.openedAtMs == 0) {
            next.openedAtMs = alpAlertPresentation_.openedAtMs;
        }
        next.closedAtMs = 0;
        alpAlertPresentation_ = next;
        return alpAlertPresentation_;
    }

    const AlpState alpState = alp_ ? alp_->getState() : AlpState::OFF;
    const uint8_t heartbeatByte1 = alp_ ? alp_->lastHeartbeatByte1() : 0xFF;
    const bool sessionActive = alp_ && alp_->currentSession().active;
    const bool normalHeartbeat = isNormalAlpListeningHeartbeat(alpState, heartbeatByte1);
    const bool recentInactiveClose =
        rawAlpEvent.closedAtMs != 0 && (nowMs - rawAlpEvent.closedAtMs) < kAlpListeningHoldDwellMs;

    const bool holdAcrossListeningGap = alpState == AlpState::LISTENING && !normalHeartbeat && recentInactiveClose;
    const bool holdAcrossTeardownGap = alpState == AlpState::TEARDOWN && sessionActive;

    if ((holdAcrossListeningGap || holdAcrossTeardownGap) && hasDisplayableAlpAlertContext(alpAlertPresentation_)) {
        alpHoldRefreshDeadlineMs_ =
            holdAcrossListeningGap ? std::max<uint32_t>(1, rawAlpEvent.closedAtMs + kAlpListeningHoldDwellMs) : 0;
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

RenderFrame DisplayPipelineModule::buildRenderFrame(uint32_t nowMs, const V1Settings& settingsRef) {
    DisplayState state = parser_->getDisplayState();
    const bool hasAlerts = parser_->hasAlerts();

    AlertData priority{};
    const bool hasRenderablePriority = hasAlerts && parser_->getRenderablePriorityAlert(priority);

    if (settingsRef.activeSlot != lastPersistenceSlot_) {
        lastPersistenceSlot_ = settingsRef.activeSlot;
        alertPersistence_->clearPersistence();
    }

    const uint8_t persistSec = settings_->getSlotAlertPersistSec(settingsRef.activeSlot);
    AlertData persistedAlert = alertPersistence_->getPersistedAlert();
    bool showPersistedAlert = false;
    if (!hasAlerts) {
        if (persistSec > 0 && persistedAlert.isValid) {
            alertPersistence_->startPersistence(nowMs);
            const unsigned long persistWindowMs = static_cast<unsigned long>(persistSec) * 1000UL;
            if (alertPersistence_->shouldShowPersisted(nowMs, persistWindowMs)) {
                showPersistedAlert = true;
            } else {
                alertPersistence_->clearPersistence();
                persistedAlert = AlertData{};
            }
        } else {
            alertPersistence_->clearPersistence();
            persistedAlert = AlertData{};
        }
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

RenderFrame DisplayPipelineModule::buildDisconnectedRestoreFrame(uint32_t nowMs, const V1Settings& settingsRef) {
    const AlpLaserEvent& rawAlpEvent = alp_ ? alp_->currentEvent() : sAlpEventEmpty;
    const AlpLaserEvent displayAlpEvent = buildPresentedAlpEvent(rawAlpEvent, nowMs);
    // ALP persist is global, not per-slot — settingsRef is forwarded to the
    // composer below for owner-resolution only.
    const uint8_t alpPersistSec = settings_->getAlpAlertPersistSec();
    updateAlpLatch(displayAlpEvent, nowMs, alpPersistSec);

    V1Snapshot v1;
    v1.state = sanitizeDisconnectedRestoreState(parser_->getDisplayState());

    AlpSnapshot alp;
    alp.event = displayAlpEvent;
    alp.ownsLaserDisplay = alp_ && alp_->ownsLaserDisplay();
    alp.isPersistedLatch = !displayAlpEvent.active && alpLatch_ && alpLatch_->isLatched();
    alp.latchedEvent = alp.isPersistedLatch ? alpLatch_->latchedEvent() : AlpLaserEvent{};

    return composer_.compose(v1, alp, settingsRef, nowMs);
}

void DisplayPipelineModule::runVoice(const RenderFrame& frame, const V1Settings& settingsRef, uint32_t nowMs) {
    const FrameV1Alerts v1Alerts = buildFrameV1Alerts(frame);
    if (v1Alerts.alertCount == 0) {
        hasPendingVoiceAction_ = false;
        nextVoiceAttemptMs_ = 0;
        voice_->clearAllState();
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

    if (hasPendingVoiceAction_ && !pendingVoiceActionStillCurrent(pendingVoiceAction_, v1Alerts)) {
        hasPendingVoiceAction_ = false;
        nextVoiceAttemptMs_ = 0;
    }
    if (static_cast<int32_t>(nowMs - nextVoiceAttemptMs_) < 0) {
        return;
    }

    const VoiceAction voiceAction = hasPendingVoiceAction_ ? pendingVoiceAction_ : voice_->prepareAction(voiceCtx);

    if (!voiceAction.hasAction()) {
        return;
    }

    AudioPlaybackResult playbackResult = AudioPlaybackResult::Unavailable;
    switch (voiceAction.type) {
    case VoiceAction::Type::ANNOUNCE_PRIORITY:
        playbackResult = try_play_frequency_voice(voiceAction.band, voiceAction.freq, voiceAction.dir,
                                                  settingsRef.voiceAlertMode, settingsRef.voiceDirectionEnabled,
                                                  voiceAction.bogeyCount);
        break;

    case VoiceAction::Type::ANNOUNCE_DIRECTION:
        playbackResult = try_play_direction_only(voiceAction.dir, voiceAction.bogeyCount);
        break;

    case VoiceAction::Type::ANNOUNCE_SECONDARY:
        playbackResult = try_play_frequency_voice(voiceAction.band, voiceAction.freq, voiceAction.dir,
                                                  settingsRef.voiceAlertMode, settingsRef.voiceDirectionEnabled, 1);
        break;

    case VoiceAction::Type::ANNOUNCE_ESCALATION:
        playbackResult = try_play_threat_escalation(voiceAction.band, voiceAction.freq, voiceAction.dir,
                                                    voiceAction.bogeyCount, voiceAction.aheadCount,
                                                    voiceAction.behindCount, voiceAction.sideCount);
        break;

    case VoiceAction::Type::NONE:
    default:
        break;
    }

    if (playbackResult == AudioPlaybackResult::Accepted) {
        voice_->commitAction(voiceAction, nowMs);
        hasPendingVoiceAction_ = false;
        nextVoiceAttemptMs_ = 0;
    } else if (playbackResult == AudioPlaybackResult::Busy) {
        pendingVoiceAction_ = voiceAction;
        hasPendingVoiceAction_ = true;
        nextVoiceAttemptMs_ = nowMs + VOICE_RETRY_INTERVAL_MS;
    } else {
        // Missing/disabled audio is terminal for this attempt. Do not commit
        // dedup state, but also do not let one unavailable clip starve newer
        // current-frame decisions forever.
        hasPendingVoiceAction_ = false;
        nextVoiceAttemptMs_ = nowMs + 1000;
    }
}

void DisplayPipelineModule::renderComposedFrame(uint32_t nowMs, const RenderFrame& frame, bool forceRedraw) {
    const bool deferSecondaryCards =
        ble_->isConnectBurstSettling() && isLivePrimaryKind(frame.primaryKind) && frame.cardCount > 0;

    // Avoid copying the full RenderFrame (std::array<RenderFrameCard, 15>
    // + scalars, a few hundred bytes) on every pipeline tick just to override one
    // field. Only copy when we're actually deferring cards; otherwise pass the
    // caller's const ref straight through. The snapshot formatter reads
    // deferSecondaryCards as a separate flag and does not consult frame.cardCount
    // for its own counting (renderAlertCountForSnapshot uses the flag + the
    // pre-built v1AlertCount), so the log output is identical whether we hand it
    // `frame` or the deferred clone.
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

    if (forceRedraw) {
        display_->forceNextRedraw();
    }

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
    if (frame.primaryKind == RenderFramePrimaryKind::IDLE && settingsRef.stealthEnabled && speedSelector_) {
        const SpeedSelection spd = speedSelector_->selectedSpeed();
        frame.stealthMode = true;
        frame.stealthSpeedMph = spd.speedMph;
        frame.stealthSpeedValid = spd.valid;
    }
    runVoice(frame, settingsRef, nowMs);
    if (frame.primaryKind == RenderFramePrimaryKind::V1_LIVE) {
        alertPersistence_->setPersistedAlert(frame.v1Priority);
    }
    renderComposedFrame(nowMs, frame);
}

// Narrow re-render path for blink refreshes. It omits voice and persistence
// side effects; display caches repaint only elements affected by the phase.
void DisplayPipelineModule::refreshBlinkTick(uint32_t nowMs) {
    if (!display_ || !parser_ || !settings_ || !ble_ || !displayMode_) {
        return;
    }
    const V1Settings& settingsRef = settings_->get();
    RenderFrame frame = buildRenderFrame(nowMs, settingsRef);
    // Idle frames contain no blink sources.
    if (frame.primaryKind == RenderFramePrimaryKind::IDLE || frame.primaryKind == RenderFramePrimaryKind::NONE) {
        return;
    }
    renderComposedFrame(nowMs, frame);
}

bool DisplayPipelineModule::restoreCurrentOwner(uint32_t nowMs) {
    if (!display_ || !parser_ || !settings_ || !ble_ || !alertPersistence_ || !voice_ || !displayMode_) {
        return false;
    }

    const bool v1Connected = ble_->isConnected();
    const V1Settings& settingsRef = settings_->get();

    if (!v1Connected) {
        const RenderFrame frame = buildDisconnectedRestoreFrame(nowMs, settingsRef);
        if (isAlpPrimaryKind(frame.primaryKind)) {
            renderComposedFrame(nowMs, frame, true);
            return true;
        }

        display_->showScanning();
        *displayMode_ = DisplayMode::IDLE;
        return true;
    }

    RenderFrame frame = buildRenderFrame(nowMs, settingsRef);
    if (frame.primaryKind == RenderFramePrimaryKind::IDLE && settingsRef.stealthEnabled && speedSelector_) {
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
