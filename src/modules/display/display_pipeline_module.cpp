#include "display_pipeline_module.h"
#include "runtime_presentation_source.h"

#include <algorithm>
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
#include "packet_parser.h"
#include "settings.h"

// Null-object ALP event used when no runtime event is available.
static const AlpLaserEvent sAlpEventEmpty{};

namespace {

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

} // namespace

void DisplayPipelineModule::begin(const DisplayPipelineDependencies& dependencies) {
    displayMode_ = dependencies.displayMode;
    display_ = dependencies.display;
    presentation_ = dependencies.presentation;
    parser_ = dependencies.parser;
    settings_ = dependencies.settings;
    ble_ = dependencies.ble;
    alertPersistence_ = dependencies.alertPersistence;
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
        return buildDisconnectedFrame(nowMs);
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

    return composer_.compose(v1, alp);
}

// Callers inject stealth fields after buildRenderFrame() returns to keep the
// composer pure.

RenderFrame DisplayPipelineModule::buildDisconnectedFrame(uint32_t nowMs) {
    const AlpLaserEvent& rawAlpEvent = alp_ ? alp_->currentEvent() : sAlpEventEmpty;
    const AlpLaserEvent displayAlpEvent = buildPresentedAlpEvent(rawAlpEvent, nowMs);
    // ALP persistence uses its global setting, independent of the V1 slot.
    const uint8_t alpPersistSec = settings_->getAlpAlertPersistSec();
    updateAlpLatch(displayAlpEvent, nowMs, alpPersistSec);

    V1Snapshot v1;
    v1.state = sanitizeDisconnectedState(parser_->getDisplayState());

    AlpSnapshot alp;
    alp.event = displayAlpEvent;
    alp.ownsLaserDisplay = alp_ && alp_->ownsLaserDisplay();
    alp.isPersistedLatch = !displayAlpEvent.active && alpLatch_ && alpLatch_->isLatched();
    alp.latchedEvent = alp.isPersistedLatch ? alpLatch_->latchedEvent() : AlpLaserEvent{};

    return composer_.compose(v1, alp);
}

void DisplayPipelineModule::renderComposedFrame(uint32_t nowMs, const RenderFrame& frame, bool forceRedraw) {
    const bool deferSecondaryCards =
        ble_->isConnectBurstSettling() && isLivePrimaryKind(frame.primaryKind) && frame.cardCount > 0;

    const auto inputs = presentation_->captureForPaint();

    // Every presentation entrypoint uses the same disconnected owner. Keep
    // SCAN between ordinary heartbeat/blink ticks; explicit restoration can
    // still repaint after a preview or another temporary screen owner.
    if (!ble_->isConnected() && !isAlpPrimaryKind(frame.primaryKind)) {
        display_->showScanning(inputs.context(), forceRedraw || !scanningPresented_);
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

    display_->renderFrame(frame, inputs.context(), deferSecondaryCards ? 0 : frame.cardCount);
}

bool DisplayPipelineModule::prepareParsed(uint32_t nowMs, RenderFrame& frame) {
    if (!display_ || !presentation_ || !parser_ || !settings_ || !ble_ || !alertPersistence_ || !displayMode_) {
        return false;
    }

    const V1Settings& settingsRef = settings_->get();
    frame = buildRenderFrame(nowMs, settingsRef);
    if (ble_->isConnected() && frame.primaryKind == RenderFramePrimaryKind::IDLE &&
        settingsRef.stealthEnabled && speedSelector_) {
        const SpeedSelection spd = speedSelector_->selectedSpeed();
        frame.stealthMode = true;
        frame.stealthSpeedMph = spd.speedMph;
        frame.stealthSpeedValid = spd.valid;
    }
    return true;
}

void DisplayPipelineModule::presentPrepared(const RenderFrame& frame, uint32_t paintNowMs) {
    renderComposedFrame(paintNowMs, frame);
}

// Recompose without voice; persistence follows the same live source and expiry
// checks. Display caches repaint only elements affected by the new frame.
void DisplayPipelineModule::refreshBlinkTick(uint32_t nowMs) {
    if (!display_ || !presentation_ || !parser_ || !settings_ || !ble_ || !displayMode_) {
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
    if (!display_ || !presentation_ || !parser_ || !settings_ || !ble_ || !alertPersistence_ || !displayMode_) {
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
