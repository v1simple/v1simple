#pragma once

/**
 * VoiceModule - Voice announcement decision logic
 *
 * Responsibilities:
 * - Decide WHEN to announce (cooldowns, mute state, settings)
 * - Decide WHAT to announce (priority, secondary, escalation)
 * - Track announcement state (last announced, throttling)
 *
 * Does NOT:
 * - Play audio (returns action for main to execute)
 * - Parse BLE data (receives processed alert data)
 */

#include <Arduino.h>
#include "packet_parser.h"  // For AlertData, Band, Direction
#include "audio_beep.h"     // For AlertBand, AlertDirection

// Forward declarations
class SettingsManager;
struct V1Settings;
class V1BLEClient;

// ============================================================================
// Voice Alert Processing - Input/Output Structs
// ============================================================================

/**
 * VoiceContext - All inputs needed for voice alert decisions
 */
struct VoiceContext {
    // Alert data
    const AlertData* alerts;          // Array of all current alerts
    int alertCount;                   // Number of alerts in array
    const AlertData* priority;        // Priority alert (can be nullptr)

    // V1 state
    bool isMuted;                     // V1 mute LED is lit (UI/quiet-coordinator semantics)
    bool isSoftMuted;                 // V1 audio is suppressed (aux0 bit 0 "isSoft").
                                      // VR's spec-correct gate for "should I make noise?"
                                      // (audit B2 — voice was previously gating on isMuted).
    bool isProxyConnected;            // Phone app connected via BLE proxy
    uint8_t mainVolume;               // Current V1 main volume (0-9)

    // Environment
    bool isSuppressed;                // Priority alert is software-suppressed

    // Time
    unsigned long now;                // Current millis() timestamp

    // Default constructor
    VoiceContext() :
        alerts(nullptr), alertCount(0), priority(nullptr),
        isMuted(false), isSoftMuted(false), isProxyConnected(false), mainVolume(0),
        isSuppressed(false), now(0) {}
};

/**
 * VoiceAction - Output from process()
 *
 * Describes what voice action to take (if any).
 * Main.cpp executes this action by calling the appropriate audio function.
 */
struct VoiceAction {
    enum class Type : uint8_t {
        NONE = 0,              // No announcement needed
        ANNOUNCE_PRIORITY,     // Full: band + freq + direction + bogeys
        ANNOUNCE_DIRECTION,    // Direction only: dir + bogeys (for same alert)
        ANNOUNCE_SECONDARY,    // Full announcement for secondary alert
        ANNOUNCE_ESCALATION    // Threat escalation: band + freq + dir + breakdown
    };

    Type type = Type::NONE;

    // Payload - interpretation depends on type
    AlertBand band;                   // Band to announce
    uint16_t freq;                    // Frequency to announce
    AlertDirection dir;               // Direction to announce
    uint8_t bogeyCount;               // Total bogey count (0 = don't announce count)

    // Escalation-specific breakdown
    uint8_t aheadCount;               // Bogeys ahead (ESCALATION only)
    uint8_t behindCount;              // Bogeys behind (ESCALATION only)
    uint8_t sideCount;                // Bogeys to side (ESCALATION only)

    // Default constructor - NONE action
    VoiceAction() :
        type(Type::NONE), band(AlertBand::KA), freq(0), dir(AlertDirection::AHEAD),
        bogeyCount(0), aheadCount(0), behindCount(0), sideCount(0) {}

    // Convenience: check if action requires audio
    bool hasAction() const { return type != Type::NONE; }
};

/**
 * VoiceModule - Voice announcement decision engine
 */
class VoiceModule {
public:
    VoiceModule();

    // Initialize with dependencies
    void begin(SettingsManager* settings, V1BLEClient* ble = nullptr);

    // Main decision method - returns what to announce (if anything)
    VoiceAction process(const VoiceContext& ctx);

    // State management - call when all alerts clear
    void reset() { clearAllState(); }  // Alias for consistency with other modules
    void clearAllState();

    // ============================================================================
    // Static Utilities
    // ============================================================================

    // Get signal bars for alert based on direction
    static uint8_t getAlertBars(const AlertData& alert);

    // Create unique alert ID from band and frequency
    static uint32_t makeAlertId(Band band, uint16_t freq);

    // Check if band is enabled for secondary alert announcements
    static bool isBandEnabledForSecondary(Band band, const V1Settings& settings);

    // Convert V1 Direction bitmask to AlertDirection for audio
    static AlertDirection toAudioDirection(Direction dir);

    // Speed utility
    float getCurrentSpeedMph(unsigned long now);
    bool getCurrentSpeedSample(unsigned long now, float& speedMphOut) const;
    void updateSpeedSample(float speedMph, unsigned long timestampMs);
    void clearSpeedSample();
    bool hasValidSpeedSource(unsigned long now) const;

private:
    // Dependencies
    SettingsManager* settings_ = nullptr;
    V1BLEClient* bleClient_ = nullptr;

    // ============================================================================
    // Tracking State
    // ============================================================================

    // Announced alert tracking
    static constexpr int MAX_ANNOUNCED_ALERTS = 10;
    uint32_t announcedAlertIds_[MAX_ANNOUNCED_ALERTS] = {0};
    uint8_t announcedAlertCount_ = 0;

    // Smart threat escalation tracking
    static constexpr int WEAK_THRESHOLD = 2;
    static constexpr int STRONG_THRESHOLD = 4;
    static constexpr unsigned long SUSTAINED_MS = 500;
    static constexpr unsigned long HISTORY_STALE_MS = 5000;
    static constexpr int MAX_BOGEYS_FOR_ESCALATION = 4;

    struct AlertHistory {
        uint32_t alertId;
        uint8_t currentBars;
        uint32_t lastUpdateMs;
        uint32_t strongSinceMs;
        bool wasWeak;
        bool escalationAnnounced;
    };

    static constexpr int MAX_ALERT_HISTORIES = 10;
    AlertHistory alertHistories_[MAX_ALERT_HISTORIES] = {};
    uint8_t alertHistoryCount_ = 0;

    AlertHistory* findAlertHistory(uint32_t alertId);
    AlertHistory* getOrCreateAlertHistory(uint32_t alertId, unsigned long now);

    // Announced alert tracking helpers
    bool isAlertAnnounced(Band band, uint16_t freq);
    void markAlertAnnounced(Band band, uint16_t freq);
    void clearAnnouncedAlerts();

    // Direction change throttling
    static constexpr unsigned long DIRECTION_THROTTLE_WINDOW_MS = 10000;
    static constexpr uint8_t DIRECTION_CHANGE_LIMIT = 3;
    uint8_t directionChangeCount_ = 0;
    unsigned long directionChangeWindowStart_ = 0;

    void resetDirectionThrottle(unsigned long now);
    bool shouldThrottleDirectionChange(unsigned long now);

    // Priority stability tracking
    static constexpr unsigned long PRIORITY_STABILITY_MS = 1000;
    static constexpr unsigned long POST_PRIORITY_GAP_MS = 1500;
    unsigned long lastPriorityAnnouncementTime_ = 0;
    unsigned long priorityStableSince_ = 0;
    uint32_t lastPriorityAlertId_ = 0xFFFFFFFF;
    void updatePriorityStability(uint32_t currentAlertId, unsigned long now);
    void markPriorityAnnounced(unsigned long now);
    void resetPriorityStability();
    bool canAnnounceSecondary(unsigned long now) const;

    // Voice alert "last announced" tracking
    static constexpr unsigned long VOICE_ALERT_COOLDOWN_MS = 2000;
    static constexpr unsigned long BOGEY_COUNT_COOLDOWN_MS = 500;
    static constexpr unsigned long STABLE_COUNT_MS = 1500;
    Band lastVoiceAlertBand_ = BAND_NONE;
    Direction lastVoiceAlertDirection_ = DIR_NONE;
    uint16_t lastVoiceAlertFrequency_ = 0xFFFF;
    uint8_t lastVoiceAlertBogeyCount_ = 0;
    unsigned long lastVoiceAlertTime_ = 0;
    // Explicit first-announcement flag: lastVoiceAlertTime_ == 0 is a real
    // timestamp, not a sentinel — without this flag the cooldown silently
    // suppresses announcements for the first COOLDOWN_MS of uptime.
    bool hasAnnouncedOnce_ = false;
    uint8_t prevAlertCount_ = 0;
    unsigned long lastCountStableSinceMs_ = 0;
    bool countStableTracked_ = false;

    bool hasAlertChanged(Band band, uint16_t freq) const;
    bool hasDirectionChanged(Direction dir) const;
    bool hasCooldownPassed(unsigned long now) const;
    bool hasBogeyCountCooldownPassed(unsigned long now) const;
    void updateLastAnnounced(Band band, Direction dir, uint16_t freq, uint8_t bogeyCount, unsigned long now);
    void updateLastAnnouncedDirection(Direction dir, uint8_t bogeyCount);
    void updateLastAnnouncedTime(unsigned long now);
    void resetLastAnnounced();

    // Speed helpers (getCurrentSpeedMph is public)
    float cachedSpeedMph_ = 0.0f;
    unsigned long cachedSpeedTimestamp_ = 0;
    static constexpr unsigned long SPEED_CACHE_MAX_AGE_MS = 5000;

    // Alert history helpers
    void updateAlertHistory(Band band, uint16_t freq, uint8_t bars, unsigned long now);
    void cleanupStaleHistories(unsigned long now);
    bool shouldAnnounceThreatEscalation(Band band, uint16_t freq, uint8_t totalBogeys, unsigned long now);
    void markThreatEscalationAnnounced(Band band, uint16_t freq);
    void clearAlertHistories();

#ifdef UNIT_TEST
public:
    using AlertHistoryArray = decltype(alertHistories_);
    AlertHistoryArray& getAlertHistories() { return alertHistories_; }
    uint8_t& getAlertHistoryCount() { return alertHistoryCount_; }
    static constexpr int TEST_MAX_ALERT_HISTORIES = MAX_ALERT_HISTORIES;
    void testUpdateAlertHistory(Band band, uint16_t freq, uint8_t bars, unsigned long now) {
        updateAlertHistory(band, freq, bars, now);
    }
#endif
};
