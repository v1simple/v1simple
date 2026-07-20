#include "display_pipeline_module.h"

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
#include "perf_metrics.h"
#include "settings.h"

// ── Phase 2: Static empty ALP event for null-check fallback ──────────
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

bool isHighRateDisplaySnapshotEvent(const char* event) {
    if (!event) {
        return false;
    }

    return strcmp(event, "DISP_LIVE") == 0 || strcmp(event, "DISP_IDLE") == 0 || strcmp(event, "DISP_RESTORE") == 0 ||
           strcmp(event, "DISP_SCAN") == 0;
}

constexpr uint32_t kAlpDisplaySnapshotLogIntervalMs = 1000;

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

int renderAlertCountForSnapshot(const RenderFrame& frame, int v1AlertCount, bool deferSecondaryCards) {
    switch (frame.primaryKind) {
    case RenderFramePrimaryKind::V1_LIVE:
        return deferSecondaryCards ? 1 : v1AlertCount;

    case RenderFramePrimaryKind::ALP_LIVE:
    case RenderFramePrimaryKind::ALP_PERSISTED:
        return (v1AlertCount == 0 || deferSecondaryCards) ? 1 : v1AlertCount;

    default:
        return 0;
    }
}

PerfDisplayRenderScenario scenarioForFrame(const RenderFrame& frame, bool restoreContext) {
    if (restoreContext) {
        return PerfDisplayRenderScenario::Restore;
    }
    if (frame.primaryKind == RenderFramePrimaryKind::V1_PERSISTED) {
        return PerfDisplayRenderScenario::Persisted;
    }
    return frame.primaryKind == RenderFramePrimaryKind::IDLE ? PerfDisplayRenderScenario::Resting
                                                             : PerfDisplayRenderScenario::Live;
}

const char* bandLogName(Band band) {
    switch (band) {
    case BAND_X:
        return "X";
    case BAND_K:
        return "K";
    case BAND_KU:
        return "KU";
    case BAND_KA:
        return "KA";
    case BAND_LASER:
        return "LASER";
    case BAND_NONE:
    default:
        return "NONE";
    }
}

const char* directionLogName(Direction dir) {
    switch (dir) {
    case DIR_FRONT:
        return "FRONT";
    case DIR_SIDE:
        return "SIDE";
    case DIR_REAR:
        return "REAR";
    case DIR_NONE:
    default:
        return "NONE";
    }
}

void formatAlertSummary(char* dest, size_t destSize, bool hasAlert, const AlertData& alert) {
    if (!hasAlert || !alert.isValid || alert.band == BAND_NONE) {
        snprintf(dest, destSize, "NONE");
        return;
    }

    snprintf(dest, destSize, "%s/%u/%s", bandLogName(alert.band), static_cast<unsigned>(alert.frequency),
             directionLogName(alert.direction));
}

void formatDisplaySnapshotDetail(char* dest, size_t destSize, bool alpHasLaser, bool alpOwnsLaser, bool alpLidActive,
                                 const char* alpGunAbbr, AlpLaserDirection alpDir, int alertCount,
                                 bool hasRenderablePriority, const AlertData& renderablePriority,
                                 bool hasDisplayPriority, const AlertData& displayPriority, int renderAlertCount,
                                 bool deferSecondaryCards, const DisplayState& displayState, uint8_t rawV1SignalBars) {
    char renderableBuf[32];
    char displayBuf[32];
    formatAlertSummary(renderableBuf, sizeof(renderableBuf), hasRenderablePriority, renderablePriority);
    formatAlertSummary(displayBuf, sizeof(displayBuf), hasDisplayPriority, displayPriority);

    snprintf(
        dest, destSize,
        "has=%u own=%u lid=%u g=%s d=%s v1c=%d rp=%s dp=%s rc=%d def=%u ab=0x%02X ar=0x%02X pa=0x%02X sb=%u v1sb=%u",
        alpHasLaser ? 1u : 0u, alpOwnsLaser ? 1u : 0u, alpLidActive ? 1u : 0u, alpGunAbbr ? alpGunAbbr : "-",
        alpLaserDirectionName(alpDir), alertCount, renderableBuf, displayBuf, renderAlertCount,
        deferSecondaryCards ? 1u : 0u, static_cast<unsigned>(displayState.activeBands),
        static_cast<unsigned>(displayState.arrows), static_cast<unsigned>(displayState.priorityArrow),
        static_cast<unsigned>(displayState.signalBars), static_cast<unsigned>(rawV1SignalBars));
}

void formatRenderFrameSnapshotDetail(char* dest, size_t destSize, const RenderFrame& frame, bool alpOwnsLaser,
                                     int v1AlertCount, bool deferSecondaryCards) {
    const bool alpPrimary = isAlpPrimaryKind(frame.primaryKind);
    const AlpLaserEvent& alpEvent = alpPrimary ? frame.alpPrimary : sAlpEventEmpty;
    const char* alpGunAbbr = (alpPrimary && alpEvent.gun != AlpGunType::UNKNOWN) ? alpGunAbbrev(alpEvent.gun) : nullptr;
    const FrameV1Alerts v1Alerts = buildFrameV1Alerts(frame);

    AlertData displayPriority{};
    bool hasDisplayPriority = false;
    switch (frame.primaryKind) {
    case RenderFramePrimaryKind::V1_LIVE:
    case RenderFramePrimaryKind::V1_PERSISTED:
        displayPriority = frame.v1Priority;
        hasDisplayPriority = true;
        break;

    case RenderFramePrimaryKind::ALP_LIVE:
    case RenderFramePrimaryKind::ALP_PERSISTED:
        displayPriority = alpEventToSyntheticAlert(frame.alpPrimary);
        hasDisplayPriority = true;
        break;

    case RenderFramePrimaryKind::NONE:
    case RenderFramePrimaryKind::IDLE:
    default:
        break;
    }

    formatDisplaySnapshotDetail(dest, destSize, alpPrimary, alpOwnsLaser, alpPrimary ? alpEvent.lidActive : false,
                                alpGunAbbr, alpPrimary ? alpEvent.direction : AlpLaserDirection::UNKNOWN, v1AlertCount,
                                v1Alerts.hasPriority, v1Alerts.priority, hasDisplayPriority, displayPriority,
                                renderAlertCountForSnapshot(frame, v1AlertCount, deferSecondaryCards),
                                deferSecondaryCards, frame.primaryState, frame.context.signalBars);
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
    lastAlpDisplayTraceRelevant_ = false;
    lastAlpDisplayLogEvent_[0] = '\0';
    lastAlpDisplayLogDetail_[0] = '\0';
    lastAlpDisplaySnapshotLogMs_ = 0;
}

void DisplayPipelineModule::logAlpDisplaySnapshot(uint32_t nowMs, const char* event, const char* detail,
                                                  bool traceRelevant) {
    const bool shouldTrace = traceRelevant || lastAlpDisplayTraceRelevant_;
    lastAlpDisplayTraceRelevant_ = traceRelevant;
    if (!alp_ || !shouldTrace) {
        return;
    }

    if (strcmp(lastAlpDisplayLogEvent_, event) == 0 && strcmp(lastAlpDisplayLogDetail_, detail) == 0) {
        return;
    }

    if (isHighRateDisplaySnapshotEvent(event) && strcmp(lastAlpDisplayLogEvent_, event) == 0 &&
        (nowMs - lastAlpDisplaySnapshotLogMs_) < kAlpDisplaySnapshotLogIntervalMs) {
        return;
    }

    snprintf(lastAlpDisplayLogEvent_, sizeof(lastAlpDisplayLogEvent_), "%s", event);
    snprintf(lastAlpDisplayLogDetail_, sizeof(lastAlpDisplayLogDetail_), "%s", detail);
    lastAlpDisplaySnapshotLogMs_ = nowMs;
    alp_->logDisplayDecision(nowMs, event, detail);
}

void DisplayPipelineModule::updateAlpLatch(const AlpLaserEvent& alpEvent, uint32_t nowMs, uint8_t persistSec) {
    if (alpEvent.active) {
        if (alpLatch_) {
            alpLatch_->setEvent(alpEvent);
        }
    } else if (alpLatch_ && lastPresentedAlpEventActive_) {
        alpLatch_->startPersistence(nowMs);
    }

    lastPresentedAlpEventActive_ = alpEvent.active;

    if (!alpEvent.active && alpLatch_ && alpLatch_->isLatched()) {
        const uint32_t persistWindowMs = static_cast<uint32_t>(persistSec) * 1000UL;
        if (!alpLatch_->shouldShowPersisted(nowMs, persistWindowMs)) {
            alpLatch_->clearLatch();
        }
    }
}

AlpLaserEvent DisplayPipelineModule::buildPresentedAlpEvent(const AlpLaserEvent& rawAlpEvent, uint32_t nowMs) {
    if (rawAlpEvent.active) {
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
        alpAlertPresentation_.active = true;
        alpAlertPresentation_.lidActive = (heartbeatByte1 == 0x04);
        alpAlertPresentation_.closedAtMs = 0;
        return alpAlertPresentation_;
    }

    alpAlertPresentation_ = AlpLaserEvent{};
    return alpAlertPresentation_;
}

RenderFrame DisplayPipelineModule::buildRenderFrame(uint32_t nowMs, const V1Settings& settingsRef) {
    DisplayState state = parser_->getDisplayState();
    const bool hasAlerts = parser_->hasAlerts();

    AlertData priority{};
    const bool hasRenderablePriority = hasAlerts && parser_->getRenderablePriorityAlert(priority);
    if (hasRenderablePriority) {
        const AlertData rawPriority = parser_->getPriorityAlert();
        const bool rawRenderable = rawPriority.isValid && rawPriority.band != BAND_NONE &&
                                   ((rawPriority.band == BAND_LASER) || (rawPriority.frequency != 0));
        if (!rawRenderable) {
            PERF_INC(displayLiveFallbackToUsable);
        }
    }

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
                PERF_INC(alertPersistExpires);
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

// NOTE: stealth fields are injected by the caller (handleParsed / restoreCurrentOwner)
// after buildRenderFrame() returns, not here, to keep composer_ pure.

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

    const unsigned long voiceStartUs = micros();
    const VoiceAction voiceAction = voice_->process(voiceCtx);
    perfRecordDisplayVoiceUs(micros() - voiceStartUs);

    if (!voiceAction.hasAction()) {
        return;
    }

    switch (voiceAction.type) {
    case VoiceAction::Type::ANNOUNCE_PRIORITY:
        play_frequency_voice(voiceAction.band, voiceAction.freq, voiceAction.dir, settingsRef.voiceAlertMode,
                             settingsRef.voiceDirectionEnabled, voiceAction.bogeyCount);
        break;

    case VoiceAction::Type::ANNOUNCE_DIRECTION:
        play_direction_only(voiceAction.dir, voiceAction.bogeyCount);
        break;

    case VoiceAction::Type::ANNOUNCE_SECONDARY:
        play_frequency_voice(voiceAction.band, voiceAction.freq, voiceAction.dir, settingsRef.voiceAlertMode,
                             settingsRef.voiceDirectionEnabled, 1);
        break;

    case VoiceAction::Type::ANNOUNCE_ESCALATION:
        play_threat_escalation(voiceAction.band, voiceAction.freq, voiceAction.dir, voiceAction.bogeyCount,
                               voiceAction.aheadCount, voiceAction.behindCount, voiceAction.sideCount);
        break;

    case VoiceAction::Type::NONE:
    default:
        break;
    }
}

void DisplayPipelineModule::renderComposedFrame(uint32_t nowMs, const RenderFrame& frame, bool restoreContext,
                                                const char* logEvent, bool forceRedraw) {
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

    // Defer the 192-byte snprintf until we know the log will be
    // kept. `logAlpDisplaySnapshot` drops non-trace-relevant snapshots whose
    // event+detail match the last emission, so on idle frames the format work
    // was wasted. Compute relevance first and only format when it will be
    // consumed. Fallback: when we just transitioned out of a trace-relevant
    // state we still format one final frame so the de-dup logger can print the
    // closing snapshot (matches the prior `lastAlpDisplayTraceRelevant_` carry).
    const bool traceRelevant = isAlpPrimaryKind(frame.primaryKind) || (alp_ && alp_->ownsLaserDisplay()) ||
                               (alpLatch_ && alpLatch_->isLatched());
    if (traceRelevant || lastAlpDisplayTraceRelevant_) {
        const FrameV1Alerts v1Alerts = buildFrameV1Alerts(frame);
        char displayLogDetail[224];
        formatRenderFrameSnapshotDetail(displayLogDetail, sizeof(displayLogDetail), frame,
                                        alp_ && alp_->ownsLaserDisplay(), v1Alerts.alertCount, deferSecondaryCards);
        logAlpDisplaySnapshot(nowMs, logEvent, displayLogDetail, traceRelevant);
    }

    const bool livePrimary = frame.primaryKind == RenderFramePrimaryKind::V1_LIVE ||
                             frame.primaryKind == RenderFramePrimaryKind::ALP_LIVE ||
                             frame.primaryKind == RenderFramePrimaryKind::ALP_PERSISTED;
    *displayMode_ = livePrimary ? DisplayMode::LIVE : DisplayMode::IDLE;

    perfSetDisplayRenderScenario(scenarioForFrame(frame, restoreContext));
    const unsigned long startUs = micros();
    display_->renderFrame(*renderFramePtr);
    const unsigned long endUs = micros();
    recordPerfTiming(startUs, endUs);
    perfClearDisplayRenderScenario();
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
    renderComposedFrame(nowMs, frame, false,
                        frame.primaryKind == RenderFramePrimaryKind::IDLE ? "DISP_IDLE" : "DISP_LIVE");
}

// D2 fix: narrow re-render path used by the orchestrator's blink-refresh
// tick. Skips runVoice() and alertPersistence_->setPersistedAlert() so it
// can fire at ~80 ms cadence without re-emitting voice or shifting the
// persistence anchor. Element caches in V1Display handle the per-leaf
// short-circuit so an unchanged frame is essentially free; the arrow and
// band caches will MISS during a blink-phase toggle and repaint exactly the
// pixels that changed.
void DisplayPipelineModule::refreshBlinkTick(uint32_t nowMs) {
    if (!display_ || !parser_ || !settings_ || !ble_ || !displayMode_) {
        return;
    }
    const V1Settings& settingsRef = settings_->get();
    RenderFrame frame = buildRenderFrame(nowMs, settingsRef);
    // No blink sources on an IDLE frame — don't waste a render cycle.
    if (frame.primaryKind == RenderFramePrimaryKind::IDLE || frame.primaryKind == RenderFramePrimaryKind::NONE) {
        return;
    }
    renderComposedFrame(nowMs, frame, false, "DISP_BLINK");
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
            renderComposedFrame(nowMs, frame, true, "DISP_RESTORE", true);
            return true;
        }

        const AlpLaserEvent& alpEvent = alp_ ? alp_->currentEvent() : sAlpEventEmpty;
        const bool alpHasLaser = alpEvent.active;
        const bool alpOwnsLaser = alp_ && alp_->ownsLaserDisplay();
        const bool alpLidActive = alpEvent.lidActive;
        const AlpGunType alpGun = alpEvent.gun;
        const AlpLaserDirection alpDir = alpEvent.direction;
        const char* alpGunAbbr = (alpHasLaser && alpGun != AlpGunType::UNKNOWN) ? alpGunAbbrev(alpGun) : nullptr;
        AlertData emptyPriority{};
        char displayLogDetail[224];
        const DisplayState rawScanState = parser_->getDisplayState();
        const DisplayState scanState = sanitizeDisconnectedRestoreState(rawScanState);
        formatDisplaySnapshotDetail(displayLogDetail, sizeof(displayLogDetail), alpHasLaser, alpOwnsLaser, alpLidActive,
                                    alpGunAbbr, alpDir, 0, false, emptyPriority, false, emptyPriority, 0, false,
                                    scanState, rawScanState.signalBars);
        logAlpDisplaySnapshot(nowMs, "DISP_SCAN", displayLogDetail, alpHasLaser || alpOwnsLaser);
        perfSetDisplayRenderScenario(PerfDisplayRenderScenario::Restore);
        display_->showScanning();
        perfClearDisplayRenderScenario();
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
    renderComposedFrame(nowMs, frame, true, "DISP_RESTORE", true);
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

void DisplayPipelineModule::recordPerfTiming(unsigned long startUs, unsigned long endUs) {
    const unsigned long dur = endUs - startUs;
    perfRecordDisplayRenderUs(dur);
    perfRecordDisplayScenarioRenderUs(dur);
    PERF_INC(displayUpdates);
}
