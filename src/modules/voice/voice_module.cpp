// Voice Module - Voice announcement decision logic

#include "voice_module.h"

#include "settings.h"
#include "../perf/debug_macros.h"

#ifndef UNIT_TEST
#include "perf_metrics.h"
#define VOICE_PERF_INC(counter) PERF_INC(counter)
#else
#define VOICE_PERF_INC(counter)                                                                                        \
    do {                                                                                                               \
    } while (0)
#endif

// ============================================================================
// Constructor and Initialization
// ============================================================================

VoiceModule::VoiceModule() {
    // Dependencies set in begin()
}

void VoiceModule::begin(SettingsManager* settings, V1BLEClient* ble) {
    settings_ = settings;
    bleClient_ = ble;

    DBG_PRINTLN("[VoiceModule] Initialized");
}

// ============================================================================
// Static Utilities
// ============================================================================

uint8_t VoiceModule::getAlertBars(const AlertData& a) {
    if (a.direction & DIR_FRONT)
        return a.frontStrength;
    if (a.direction & DIR_REAR)
        return a.rearStrength;
    return (a.frontStrength > a.rearStrength) ? a.frontStrength : a.rearStrength;
}

uint32_t VoiceModule::makeAlertId(Band band, uint16_t freq) {
    return ((uint32_t)band << 16) | freq;
}

bool VoiceModule::isBandEnabledForSecondary(Band band, const V1Settings& settings) {
    switch (band) {
    case BAND_LASER:
        return settings.secondaryLaser;
    case BAND_KA:
        return settings.secondaryKa;
    case BAND_K:
        return settings.secondaryK;
    case BAND_KU:
        return settings.secondaryK; // Ku reuses K's secondary toggle.
    case BAND_X:
        return settings.secondaryX;
    default:
        return false;
    }
}

AlertDirection VoiceModule::toAudioDirection(Direction dir) {
    if (dir & DIR_FRONT) {
        return AlertDirection::AHEAD;
    } else if (dir & DIR_REAR) {
        return AlertDirection::BEHIND;
    } else {
        return AlertDirection::SIDE;
    }
}

// ============================================================================
// Local Helpers
// ============================================================================

static AlertBand toAudioBand(Band band) {
    switch (band) {
    case BAND_LASER:
        return AlertBand::LASER;
    case BAND_KA:
        return AlertBand::KA;
    case BAND_K:
        return AlertBand::K;
    case BAND_KU:
        return AlertBand::K; // No Ku audio asset; speak as K.
    case BAND_X:
        return AlertBand::X;
    default:
        return AlertBand::KA;
    }
}

static bool isValidAnnounceBand(Band band) {
    return band == BAND_LASER || band == BAND_KA || band == BAND_K || band == BAND_KU || band == BAND_X;
}

// ============================================================================
// Main Decision Method
// ============================================================================

VoiceAction VoiceModule::process(const VoiceContext& ctx) {
    VoiceAction action; // Default = NONE

    // --- Early Exit Checks ---

    if (!settings_)
        return action;
    const V1Settings& s = settings_->get();

    // Voice alerts disabled
    if (s.voiceAlertMode == VOICE_MODE_DISABLED)
        return action;

    // Mute voice if V1 volume is zero (optional setting)
    if (s.muteVoiceIfVolZero && ctx.mainVolume == 0)
        return action;

    // V1 audio is soft-muted (aux0 bit 0 — spec-correct "should I make noise?" gate).
    // Was previously ctx.isMuted (LED bit), which is debounced and lags / can disagree
    // with V1's own audio gate.  See V1 secondary audit B2.
    if (ctx.isSoftMuted)
        return action;

    // Alert is in a suppression zone
    if (ctx.isSuppressed)
        return action;

    // Phone app is connected - let app handle voice
    if (ctx.isProxyConnected)
        return action;

    // No valid priority alert
    if (!ctx.priority || ctx.priority->band == BAND_NONE)
        return action;

    // V1 audit R1: photo-radar alerts ride on the K band on the wire (V1
    // synthesizes AlertBand.Photo = 0xFE only when band==K AND photoType!=0;
    // see VR data/AlertData.java:323-336). We have no `band_photo.mul` audio
    // asset, and announcing photo-radar enforcement cameras as plain "K" is
    // misleading — the user already sees the Photo card and 'P' on the bogey
    // counter. Suppress voice for any priority alert flagged as photo-type.
    if (ctx.priority->photoType != 0)
        return action;

    // --- Priority Alert Logic ---

    const AlertData& priority = *ctx.priority;
    uint16_t currentFreq = (uint16_t)priority.frequency;

    // Query current state
    bool alertChanged = hasAlertChanged(priority.band, currentFreq);
    bool directionChanged = hasDirectionChanged(priority.direction);
    bool directionKnown = (priority.direction != DIR_NONE);
    bool cooldownPassed = hasCooldownPassed(ctx.now);

    // F: track alert-count stability — direction re-announces wait until V1's
    // count stops fluttering, so we don't blurt stale "left/right" while
    // priority is still shuffling.
    if (!countStableTracked_) {
        prevAlertCount_ = (uint8_t)ctx.alertCount;
        lastCountStableSinceMs_ = ctx.now;
        countStableTracked_ = true;
    } else if ((uint8_t)ctx.alertCount != prevAlertCount_) {
        prevAlertCount_ = (uint8_t)ctx.alertCount;
        lastCountStableSinceMs_ = ctx.now;
    }
    bool countStable = (ctx.now - lastCountStableSinceMs_) >= STABLE_COUNT_MS;

    // Track priority stability for secondary alerts
    uint32_t currentAlertId = makeAlertId(priority.band, currentFreq);
    updatePriorityStability(currentAlertId, ctx.now);

    // Convert direction for audio
    AlertDirection audioDir = toAudioDirection(priority.direction);

    // Case 1: New Alert (band or frequency changed)
    if (alertChanged && cooldownPassed) {
        if (!isValidAnnounceBand(priority.band))
            return action;

        resetDirectionThrottle(ctx.now);

        action.type = VoiceAction::Type::ANNOUNCE_PRIORITY;
        action.band = toAudioBand(priority.band);
        action.freq = currentFreq;
        action.dir = audioDir;
        action.bogeyCount = s.announceBogeyCount ? (uint8_t)ctx.alertCount : 1;

        updateLastAnnounced(priority.band, priority.direction, currentFreq, (uint8_t)ctx.alertCount, ctx.now);
        markPriorityAnnounced(ctx.now);
        markAlertAnnounced(priority.band, currentFreq);
        VOICE_PERF_INC(voiceAnnouncePriority);

        return action;
    }

    // Case 2: Direction Changed (same alert)
    // Ignore transient DIR_NONE direction drops to avoid noisy "side" chatter.
    // F: also gate on countStable so we don't announce direction while V1 is
    // still shuffling priority during a count flutter.
    if (!alertChanged && directionChanged && cooldownPassed && s.voiceDirectionEnabled && directionKnown &&
        countStable) {
        bool throttled = shouldThrottleDirectionChange(ctx.now);
        updateLastAnnouncedDirection(priority.direction, (uint8_t)ctx.alertCount);

        if (throttled) {
            VOICE_PERF_INC(voiceDirectionThrottled);
            return action;
        }

        action.type = VoiceAction::Type::ANNOUNCE_DIRECTION;
        action.dir = audioDir;
        // C: never speak bogey-count on direction-only updates — display owns
        // the count; voice is just the directional cue.
        action.bogeyCount = 0;

        updateLastAnnouncedTime(ctx.now);
        markPriorityAnnounced(ctx.now);
        VOICE_PERF_INC(voiceAnnounceDirection);

        return action;
    }

    // Case 3 (count-only re-announce) intentionally removed: voice no longer
    // speaks count outside the initial Case 1 announcement. The display is
    // the source of truth for current bogey count; redundant audio of a
    // potentially-stale count caused mismatch perception under heavy V1 churn.

    // --- Secondary Alert Logic ---

    if (s.announceSecondaryAlerts && ctx.alertCount > 1 && canAnnounceSecondary(ctx.now)) {
        for (int i = 0; i < ctx.alertCount; i++) {
            const AlertData& alert = ctx.alerts[i];
            if (!alert.isValid || alert.band == BAND_NONE)
                continue;

            uint16_t alertFreq = (uint16_t)alert.frequency;

            // Skip priority alert
            if (alert.band == priority.band && alertFreq == currentFreq)
                continue;

            // Skip if already announced
            if (isAlertAnnounced(alert.band, alertFreq))
                continue;

            // Check band filter
            if (!isBandEnabledForSecondary(alert.band, s))
                continue;

            if (!isValidAnnounceBand(alert.band))
                continue;

            action.type = VoiceAction::Type::ANNOUNCE_SECONDARY;
            action.band = toAudioBand(alert.band);
            action.freq = alertFreq;
            action.dir = toAudioDirection(alert.direction);
            action.bogeyCount = 1;

            markAlertAnnounced(alert.band, alertFreq);
            updateLastAnnouncedTime(ctx.now);
            VOICE_PERF_INC(voiceAnnounceSecondary);

            return action;
        }
    }

    // --- Smart Threat Escalation ---

    if (s.announceSecondaryAlerts && ctx.alertCount > 1) {
        // Update all alert histories
        for (int i = 0; i < ctx.alertCount; i++) {
            const AlertData& alert = ctx.alerts[i];
            if (!alert.isValid || alert.band == BAND_NONE)
                continue;
            if (alert.band == BAND_LASER)
                continue;

            uint16_t alertFreq = (uint16_t)alert.frequency;
            uint8_t bars = getAlertBars(alert);
            updateAlertHistory(alert.band, alertFreq, bars, ctx.now);
        }

        cleanupStaleHistories(ctx.now);

        // Check for escalation triggers
        if (hasBogeyCountCooldownPassed(ctx.now)) {
            for (int i = 0; i < ctx.alertCount; i++) {
                const AlertData& alert = ctx.alerts[i];
                if (!alert.isValid || alert.band == BAND_NONE)
                    continue;
                if (alert.band == BAND_LASER)
                    continue;

                uint16_t alertFreq = (uint16_t)alert.frequency;

                // Skip priority alert
                if (alert.band == priority.band && alertFreq == currentFreq)
                    continue;
                if (ctx.isSoftMuted)
                    continue; // V1 audio mute (audit B2)
                if (alert.photoType != 0)
                    continue; // V1 audit R1: never speak photo-radar
                if (!isBandEnabledForSecondary(alert.band, s))
                    continue;

                if (shouldAnnounceThreatEscalation(alert.band, alertFreq, (uint8_t)ctx.alertCount, ctx.now)) {
                    markThreatEscalationAnnounced(alert.band, alertFreq);

                    if (!isValidAnnounceBand(alert.band))
                        continue;

                    // Count direction breakdown
                    uint8_t aheadCount = 0, behindCount = 0, sideCount = 0;
                    for (int j = 0; j < ctx.alertCount; j++) {
                        const AlertData& a = ctx.alerts[j];
                        if (!a.isValid || a.band == BAND_NONE)
                            continue;

                        if (a.direction & DIR_FRONT)
                            aheadCount++;
                        else if (a.direction & DIR_REAR)
                            behindCount++;
                        else
                            sideCount++;
                    }

                    uint8_t total = aheadCount + behindCount + sideCount;

                    action.type = VoiceAction::Type::ANNOUNCE_ESCALATION;
                    action.band = toAudioBand(alert.band);
                    action.freq = alertFreq;
                    action.dir = toAudioDirection(alert.direction);
                    action.bogeyCount = total;
                    action.aheadCount = aheadCount;
                    action.behindCount = behindCount;
                    action.sideCount = sideCount;

                    updateLastAnnouncedTime(ctx.now);
                    VOICE_PERF_INC(voiceAnnounceEscalation);

                    return action;
                }
            }
        }
    }

    return action; // NONE
}

// ============================================================================
// State Management
// ============================================================================

void VoiceModule::clearAllState() {
    clearAnnouncedAlerts();
    clearAlertHistories();
    resetLastAnnounced();
    resetPriorityStability();
}

// ============================================================================
// Announced Alert Tracking
// ============================================================================

bool VoiceModule::isAlertAnnounced(Band band, uint16_t freq) {
    uint32_t id = makeAlertId(band, freq);
    for (int i = 0; i < announcedAlertCount_; i++) {
        if (announcedAlertIds_[i] == id)
            return true;
    }
    return false;
}

void VoiceModule::markAlertAnnounced(Band band, uint16_t freq) {
    uint32_t id = makeAlertId(band, freq);
    if (isAlertAnnounced(band, freq)) {
        return;
    }

    // The set is a FIFO window over the most recent MAX_ANNOUNCED_ALERTS ids, not
    // a fixed-capacity set that stops accepting. Silently refusing to record once
    // full saturates the set during a long multi-alert stretch: every later alert
    // then reads as un-announced in the secondary-alert loop, so the same bogeys
    // get announced over and over. Evict the oldest id instead, which keeps the
    // window useful under sustained load and re-arms only the alert that has been
    // quiet the longest. MAX_ANNOUNCED_ALERTS is 10 and this runs at most once per
    // announcement, so the shift is cheaper than carrying a wrap index.
    if (announcedAlertCount_ >= MAX_ANNOUNCED_ALERTS) {
        memmove(&announcedAlertIds_[0], &announcedAlertIds_[1],
                sizeof(announcedAlertIds_[0]) * (MAX_ANNOUNCED_ALERTS - 1));
        announcedAlertCount_ = MAX_ANNOUNCED_ALERTS - 1;
    }
    announcedAlertIds_[announcedAlertCount_++] = id;
}

void VoiceModule::clearAnnouncedAlerts() {
    announcedAlertCount_ = 0;
    memset(announcedAlertIds_, 0, sizeof(announcedAlertIds_));
}

// ============================================================================
// Alert History Tracking - Smart Threat Escalation
// ============================================================================

VoiceModule::AlertHistory* VoiceModule::findAlertHistory(uint32_t alertId) {
    for (int i = 0; i < alertHistoryCount_; i++) {
        if (alertHistories_[i].alertId == alertId) {
            return &alertHistories_[i];
        }
    }
    return nullptr;
}

VoiceModule::AlertHistory* VoiceModule::getOrCreateAlertHistory(uint32_t alertId, unsigned long now) {
    AlertHistory* h = findAlertHistory(alertId);
    if (h)
        return h;

    if (alertHistoryCount_ < MAX_ALERT_HISTORIES) {
        h = &alertHistories_[alertHistoryCount_++];
        h->alertId = alertId;
        h->currentBars = 0;
        h->lastUpdateMs = now;
        h->strongSinceMs = 0;
        h->wasWeak = false;
        h->escalationAnnounced = false;
        return h;
    }

    // Recycle oldest (by elapsed time, handles millis() wraparound)
    unsigned long oldestElapsed = 0;
    int oldestIdx = -1;
    for (int i = 0; i < alertHistoryCount_; i++) {
        unsigned long elapsed = now - alertHistories_[i].lastUpdateMs;
        if (elapsed > oldestElapsed) {
            oldestElapsed = elapsed;
            oldestIdx = i;
        }
    }
    if (oldestIdx >= 0) {
        h = &alertHistories_[oldestIdx];
        h->alertId = alertId;
        h->currentBars = 0;
        h->lastUpdateMs = now;
        h->strongSinceMs = 0;
        h->wasWeak = false;
        h->escalationAnnounced = false;
        return h;
    }

    return nullptr;
}

void VoiceModule::updateAlertHistory(Band band, uint16_t freq, uint8_t bars, unsigned long now) {
    if (band == BAND_LASER)
        return;

    uint32_t alertId = makeAlertId(band, freq);
    AlertHistory* h = getOrCreateAlertHistory(alertId, now);
    if (!h)
        return;

    if (bars <= WEAK_THRESHOLD)
        h->wasWeak = true;

    if (bars >= STRONG_THRESHOLD) {
        if (h->strongSinceMs == 0)
            h->strongSinceMs = now;
    } else {
        h->strongSinceMs = 0;
    }

    h->currentBars = bars;
    h->lastUpdateMs = now;
}

void VoiceModule::cleanupStaleHistories(unsigned long now) {
    for (int i = alertHistoryCount_ - 1; i >= 0; i--) {
        if (now - alertHistories_[i].lastUpdateMs > HISTORY_STALE_MS) {
            for (int j = i; j < alertHistoryCount_ - 1; j++) {
                alertHistories_[j] = alertHistories_[j + 1];
            }
            alertHistoryCount_--;
        }
    }
}

bool VoiceModule::shouldAnnounceThreatEscalation(Band band, uint16_t freq, uint8_t totalBogeys, unsigned long now) {
    if (band == BAND_LASER)
        return false;

    uint32_t alertId = makeAlertId(band, freq);
    AlertHistory* h = findAlertHistory(alertId);
    if (!h)
        return false;

    bool wasWeak = h->wasWeak;
    bool nowStrong = (h->currentBars >= STRONG_THRESHOLD);
    bool sustained = (h->strongSinceMs > 0) && (now - h->strongSinceMs >= SUSTAINED_MS);
    bool notNoisy = (totalBogeys <= MAX_BOGEYS_FOR_ESCALATION);
    bool notAnnounced = !h->escalationAnnounced;

    if (wasWeak && nowStrong && sustained && notNoisy && notAnnounced) {
        return true;
    }
    return false;
}

void VoiceModule::markThreatEscalationAnnounced(Band band, uint16_t freq) {
    uint32_t alertId = makeAlertId(band, freq);
    AlertHistory* h = findAlertHistory(alertId);
    if (h)
        h->escalationAnnounced = true;
}

void VoiceModule::clearAlertHistories() {
    alertHistoryCount_ = 0;
    memset(alertHistories_, 0, sizeof(alertHistories_));
}

// ============================================================================
// Direction Change Throttling
// ============================================================================

void VoiceModule::resetDirectionThrottle(unsigned long now) {
    directionChangeCount_ = 0;
    directionChangeWindowStart_ = now;
}

bool VoiceModule::shouldThrottleDirectionChange(unsigned long now) {
    if (now - directionChangeWindowStart_ > DIRECTION_THROTTLE_WINDOW_MS) {
        directionChangeCount_ = 0;
        directionChangeWindowStart_ = now;
    }
    directionChangeCount_++;
    return directionChangeCount_ > DIRECTION_CHANGE_LIMIT;
}

// ============================================================================
// Priority Stability Tracking
// ============================================================================

void VoiceModule::updatePriorityStability(uint32_t currentAlertId, unsigned long now) {
    if (currentAlertId != lastPriorityAlertId_) {
        lastPriorityAlertId_ = currentAlertId;
        priorityStableSince_ = now;
    }
}

void VoiceModule::markPriorityAnnounced(unsigned long now) {
    lastPriorityAnnouncementTime_ = now;
}

void VoiceModule::resetPriorityStability() {
    priorityStableSince_ = 0;
    lastPriorityAlertId_ = 0xFFFFFFFF;
}

bool VoiceModule::canAnnounceSecondary(unsigned long now) const {
    return (priorityStableSince_ > 0) && (now - priorityStableSince_ >= PRIORITY_STABILITY_MS) &&
           (now - lastPriorityAnnouncementTime_ >= POST_PRIORITY_GAP_MS);
}

// ============================================================================
// Last Announced Tracking
// ============================================================================

bool VoiceModule::hasAlertChanged(Band band, uint16_t freq) const {
    return (band != lastVoiceAlertBand_) || (freq != lastVoiceAlertFrequency_);
}

bool VoiceModule::hasDirectionChanged(Direction dir) const {
    return dir != lastVoiceAlertDirection_;
}

bool VoiceModule::hasCooldownPassed(unsigned long now) const {
    // Before the first announcement there is nothing to cool down from — the
    // 0-initialized timestamp must not suppress the boot-window announcement.
    if (!hasAnnouncedOnce_)
        return true;
    return (now - lastVoiceAlertTime_ >= VOICE_ALERT_COOLDOWN_MS);
}

bool VoiceModule::hasBogeyCountCooldownPassed(unsigned long now) const {
    if (!hasAnnouncedOnce_)
        return true;
    return (now - lastVoiceAlertTime_ >= BOGEY_COUNT_COOLDOWN_MS);
}

void VoiceModule::updateLastAnnounced(Band band, Direction dir, uint16_t freq, uint8_t bogeyCount, unsigned long now) {
    lastVoiceAlertBand_ = band;
    lastVoiceAlertDirection_ = dir;
    lastVoiceAlertFrequency_ = freq;
    lastVoiceAlertBogeyCount_ = bogeyCount;
    lastVoiceAlertTime_ = now;
    hasAnnouncedOnce_ = true;
}

void VoiceModule::updateLastAnnouncedDirection(Direction dir, uint8_t bogeyCount) {
    lastVoiceAlertDirection_ = dir;
    lastVoiceAlertBogeyCount_ = bogeyCount;
}

void VoiceModule::updateLastAnnouncedTime(unsigned long now) {
    lastVoiceAlertTime_ = now;
    hasAnnouncedOnce_ = true;
}

void VoiceModule::resetLastAnnounced() {
    lastVoiceAlertBand_ = BAND_NONE;
    lastVoiceAlertDirection_ = DIR_NONE;
    lastVoiceAlertFrequency_ = 0xFFFF;
    lastVoiceAlertBogeyCount_ = 0;
    lastVoiceAlertTime_ = 0;
    prevAlertCount_ = 0;
    lastCountStableSinceMs_ = 0;
    countStableTracked_ = false;
}

// ============================================================================
// Speed Helpers - Low Speed Muting
// ============================================================================

bool VoiceModule::getCurrentSpeedSample(unsigned long now, float& speedMphOut) const {
    if (cachedSpeedTimestamp_ == 0) {
        return false;
    }
    if ((now - cachedSpeedTimestamp_) >= SPEED_CACHE_MAX_AGE_MS) {
        return false;
    }
    speedMphOut = cachedSpeedMph_;
    return true;
}

float VoiceModule::getCurrentSpeedMph(unsigned long now) {
    float speedMph = 0.0f;
    if (getCurrentSpeedSample(now, speedMph)) {
        return speedMph;
    }

    return 0.0f;
}

void VoiceModule::updateSpeedSample(float speedMph, unsigned long timestampMs) {
    // Ignore invalid/negative input so stale-good data is preserved.
    if (!(speedMph >= 0.0f)) {
        return;
    }
    cachedSpeedMph_ = speedMph;
    cachedSpeedTimestamp_ = timestampMs;
}

void VoiceModule::clearSpeedSample() {
    cachedSpeedMph_ = 0.0f;
    cachedSpeedTimestamp_ = 0;
}

bool VoiceModule::hasValidSpeedSource(unsigned long now) const {
    float speedMph = 0.0f;
    return getCurrentSpeedSample(now, speedMph);
}
