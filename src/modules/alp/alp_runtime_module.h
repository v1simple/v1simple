/**
 * Receive-only ALP HiFi listener: UART2 RX on GPIO 2 (RJ-45 pin 2), 19200 8N1.
 * No TX or enable GPIO. begin() owns UART lifecycle; process() runs on the main loop.
 *
 * Frames: byte0 byte1 byte2 checksum; checksum = (b0+b1+b2) & 0x7F.
 * LID deployment uses 98 00 E3 7B; targeted B0 01 heartbeats also open alerts.
 * Gun fingerprints use CX 00 YY (LID deployment) or CX YY 00 (detection).
 * Checksum admission and the RX glitch filter reject DLI/LID crosstalk.
 *
 * Manual terminology: PDC Only, PDC & DLI, PDC & LID. DLI is detection below
 * the LID speed limit; LID is countermeasure deployment above it. Manual warm-up
 * is about 60 seconds after boot or LID timeout; runtime admission is defined below.
 */

#pragma once

#include <cstdint>
#include <cstddef>

#include "alp_laser_event.h"

class SystemEventBus;

enum class AlpState : uint8_t {
    OFF = 0,      // Module off (alpEnabled == false)
    IDLE,         // UART open, waiting for first valid frame
    LISTENING,    // Receiving heartbeats — ALP CPU confirmed alive
    ALERT_ACTIVE, // Laser detected — heartbeat byte1=01 or 98 trigger
    NOISE_WINDOW, // Speaker alert active — UART data is glitch noise
    TEARDOWN,     // Register cleanup after alert, returning to idle
};

const char* alpStateName(AlpState s);

// Gun ID from CX 00 YY frames: byte0 = gun family, byte2 = gun code.

enum class AlpGunType : uint8_t {
    UNKNOWN = 0,
    PL3_PROLITE,        // byte0=c8 gunCode=d5
    DRAGONEYE_COMPACT,  // byte0=c8 gunCode=d6
    LTI_TRUSPEED_LR,    // byte0=c9 gunCode=f5
    LASER_ATLANTA_PL2,  // byte0=cb gunCode=eb  (238 pps)
    MARKSMAN_ULTRALYTE, // LID-deploy: cd/d6 | Detect: cd/0c
    STALKER_LZ1,        // byte0=cd gunCode=eb  (~130 pps)
    LASER_ALLY,         // byte0=cd gunCode=d7
    ATLANTA_STEALTH,    // byte0=ce gunCode=eb  (~68 pps "stealth mode")
};

const char* alpGunName(AlpGunType gun);
const char* alpGunAbbrev(AlpGunType gun);

enum class AlpLaserDirection : uint8_t {
    UNKNOWN = 0,
    FRONT,
    REAR,
};

const char* alpLaserDirectionName(AlpLaserDirection direction);

struct AlpGunCode {
    uint8_t byte0;   // Gun family (CX frame byte0)
    uint8_t gunCode; // Gun identifier (CX frame byte2)
    AlpGunType gun;
};

AlpGunType alpLookupGun(uint8_t byte0, uint8_t gunCode);
AlpGunType alpLookupGunDetect(uint8_t byte0, uint8_t byte1);

// All ALP frames: 4 bytes, checksum = (byte0 + byte1 + byte2) & 0x7F.

static inline uint8_t alpChecksum(uint8_t b0, uint8_t b1, uint8_t b2) {
    return (b0 + b1 + b2) & 0x7F;
}

static inline bool alpValidateChecksum(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t cs) {
    return cs == alpChecksum(b0, b1, b2);
}

struct AlpStatus {
    AlpState state;
    AlpGunType lastGun;
    uint32_t lastGunTimestampMs;      // millis() when gun was identified
    uint32_t lastHeartbeatMs;         // millis() of most recent valid frame
    uint8_t lastHbByte1;              // most recent B0 heartbeat byte1 (01=Targeted, 02=Warm-Up, 03=DLI, 04=LID)
    AlpLaserDirection laserDirection; // live-alert front/rear classifier projected for display/API
    uint8_t directionSampleByte1;     // raw B0 byte1 that latched laserDirection (0 when unknown)
    bool uartActive;                  // true if UART has received any data
    bool hasLaserEvent;               // mirror of AlpRuntimeModule::hasLaserEvent() — live alert indicator for display
    uint32_t detectGeneration;        // changes only for accepted sparse 98 detection records
    uint8_t detectRaw[3];             // most recent accepted sparse detection payload
};

// One engagement keeps gun/direction through TEARDOWN rearms and noise.
// It opens on IDLE/LISTENING -> ALERT_ACTIVE and closes on TEARDOWN ->
// LISTENING or heartbeat silence. Display eligibility is narrower: see hasLaserEvent().
//
// Warm-up hides sessions opened inside the 35-second preamble envelope, before
// the first heartbeat, or in an unconfirmed detect mode. Frame-handler
// confirmation releases it.

struct AlertSession {
    bool active = false;   // session open?
    bool isWarmUp = false; // suppressed from display (Warm-Up)
    uint32_t startMs = 0;  // session opened
    uint32_t endMs = 0;    // 0 while active
    AlpLaserDirection direction =
        AlpLaserDirection::UNKNOWN;       // latched from first qualifying B0 byte1 during ALERT_ACTIVE
    uint8_t directionSampleByte1 = 0x00;  // raw B0 byte1 that latched direction
    AlpGunType gun = AlpGunType::UNKNOWN; // session's identified gun
    uint32_t gunIdentifiedMs = 0;         // 0 if not yet identified
    uint8_t modeAtOpen = 0xFF;            // lastHbByte1_ when session opened:
                                          //   01 = Targeted (mid-engagement reopen)
                                          //   02 = Warm-Up
                                          //   03 = DLI active (below LID speed)
                                          //   04 = LID active (above LID speed)
                                          //   00/06 = transitional / engaged
                                          //   0xFF = unknown (default)
};

class AlpRuntimeModule {
  public:
    static constexpr int ALP_RX_PIN = 2;

    static constexpr uint32_t ALP_BAUD = 19200;
    static constexpr size_t FRAME_LEN = 4; // byte0 byte1 byte2 checksum

    static constexpr uint8_t ALERT_BYTE0 = 0x98;
    static constexpr uint8_t ALERT_BYTE1 = 0x00;
    static constexpr uint8_t ALERT_BYTE2 = 0xE3;

    static constexpr uint8_t HEARTBEAT_SINGLE_0 = 0xB0;
    static constexpr uint8_t HEARTBEAT_PAIRED_0 = 0xB8;
    static constexpr uint8_t HEARTBEAT_TRIPLE_0 = 0xE0;

    static constexpr uint8_t DISCOVERY_BYTE0 = 0x91;
    static constexpr uint8_t SETUP_BYTE0_A8 = 0xA8;
    static constexpr uint8_t SETUP_BYTE0_F0 = 0xF0;

    static constexpr uint32_t HEARTBEAT_TIMEOUT_MS = 3000;
    static constexpr uint32_t NOISE_WINDOW_MAX_MS = 35000; // 31s max + margin
    static constexpr uint32_t TEARDOWN_TIMEOUT_MS = 5000;
    static constexpr uint32_t ALERT_ACTIVE_TIMEOUT_MS = 15000; // no 98 trigger rearm within 15s of entry → teardown
    static constexpr size_t UART_RX_BUFFER_SIZE = 512;

    // Heartbeat byte1 alert detection: byte1=01 means Targeted (laser hit),
    // byte1=02=Warm-Up, 03=DLI active, 04=LID active. byte1=01 is the
    // universal alert indicator that fires in all operational states
    // (DLI, LID pre-deploy, LID post-deploy).
    static constexpr uint8_t HB_BYTE1_ALERT = 0x01;

    // Noise detection: consecutive bad checksums to enter NOISE_WINDOW.
    // Applies from ALERT_ACTIVE or LISTENING (DLI circuitry crosstalk
    // and LID I2S speaker crosstalk both produce UART noise).
    static constexpr uint32_t NOISE_CHECKSUM_THRESHOLD = 8;

    // Warm-Up sessions are identified by an early F0/A8 preamble, a trigger
    // before the first heartbeat, or heartbeat mode 0x02. Gun identification
    // clears the flag because Warm-Up bursts do not identify a gun.
    static constexpr uint32_t WARM_UP_PREAMBLE_WINDOW_MS = 5000;
    static constexpr uint32_t WARM_UP_ENVELOPE_MS = 35000;

    /**
     * Initialize the module.
     * @param enabled  true to open UART2 and begin listening
     */
    void begin(bool enabled);

    /**
     * Wire the event bus for state-change notifications.
     */
    void setEventBus(SystemEventBus* bus) { bus_ = bus; }

    /**
     * Called every main loop iteration.
     * Drains UART2 RX buffer, advances state machine.
     * @param nowMs  current millis()
     */
    void process(uint32_t nowMs);

    /**
     * Snapshot of current state for display / API consumers.
     *
     * NOT thread-safe. Reads module state (state_, session,
     * lastGun_) without synchronization. This is safe only because every
     * current caller runs on Core 1 alongside process() — the display
     * sync path and the main-loop pipeline are both main-loop work.
     *
     * If a consumer is ever added on another core (e.g. a WiFi API
     * handler running on Core 0), this needs to be hardened — either
     * promote the fields it reads to std::atomic, or copy them under a
     * short critical section held inside process() and snapshot().
     */
    AlpStatus snapshot() const;

    /** Current state (for wiring / guards). */
    AlpState getState() const { return state_; }

    /** Is module enabled? */
    bool isEnabled() const { return enabled_; }

    /**
     * millis() timestamp of the last raw byte read from the ALP UART.
     * 0 if no byte has ever been received since module begin(). Used by
     * PowerModule's car-mode UART-silence shutdown path to distinguish
     * "ALP never spoke" (never arm) from "ALP was alive and has now
     * gone quiet" (arm timer).
     *
     * This is raw byte reception, NOT valid-frame reception. A checksum-
     * failing byte still counts as "ALP is alive" because ignition loss
     * drops the UART line entirely, producing zero bytes — not malformed
     * frames.
     */
    uint32_t lastUartByteMs() const { return lastUartByteMs_; }

    /**
     * millis() timestamp of the last checksum-valid ALP protocol frame.
     * 0 if no valid frame has ever been parsed since module begin().
     * PowerModule's car-mode ALP silence shutdown uses this, not raw
     * bytes, so a floating/noisy RX line cannot keep the unit awake.
     */
    uint32_t lastValidFrameMs() const { return lastFrameMs_; }

    /** Is an alert currently active (laser event in progress)? */
    bool isAlertActive() const { return state_ == AlpState::ALERT_ACTIVE || state_ == AlpState::NOISE_WINDOW; }

    /** Most recently identified gun (persists across alerts). */
    AlpGunType lastIdentifiedGun() const { return lastGun_; }

    /** Timestamp of last gun identification. */
    uint32_t lastGunTimestampMs() const { return lastGunTimestampMs_; }

    /** Most recent B0 heartbeat byte1 (01=Targeted, 02/03/04=listening modes). */
    uint8_t lastHeartbeatByte1() const { return lastHbByte1_; }

    // Consumers use the event snapshot and display eligibility, not parser states.
    // Current event for display and API consumers.
    const AlpLaserEvent& currentEvent() const { return currentEvent_; }
    // Latest display-eligible event that ended, retained across a UART batch.
    // The copied identity remains active only so the presentation latch can
    // accept it; closedAtMs owns its persistence age, never live ownership.
    const AlpLaserEvent& endedEvent() const { return endedEvent_; }
    uint32_t endedEventRevision() const { return endedEventRevision_; }

    // Live ALERT_ACTIVE/NOISE_WINDOW or identified TEARDOWN; never warm-up.
    bool hasLaserEvent() const {
        // Keep this predicate aligned with updateCurrentEvent().
        const bool teardownDisplayable = (state_ == AlpState::TEARDOWN) && (session_.gun != AlpGunType::UNKNOWN);
        return session_.active && !session_.isWarmUp &&
               (state_ == AlpState::ALERT_ACTIVE || state_ == AlpState::NOISE_WINDOW || teardownDisplayable);
    }

    /** Full current session for product state consumers and tests. */
    const AlertSession& currentSession() const { return session_; }

    // Connection is a prerequisite for laser ownership. The composer suppresses
    // V1 laser only when an ALP branch actually renders; a silent ALP cannot hide it.
    bool ownsLaserDisplay() const { return enabled_ && state_ != AlpState::OFF && state_ != AlpState::IDLE; }

#ifdef UNIT_TEST
    void testSyncCurrentEvent(uint32_t nowMs = 0) {
        AlpLaserEvent next;
        const bool teardownDisplayable = (state_ == AlpState::TEARDOWN) && (session_.gun != AlpGunType::UNKNOWN);
        next.active = session_.active && !session_.isWarmUp &&
                      (state_ == AlpState::ALERT_ACTIVE || state_ == AlpState::NOISE_WINDOW || teardownDisplayable);
        next.gun = next.active ? session_.gun : AlpGunType::UNKNOWN;
        next.direction = next.active ? session_.direction : AlpLaserDirection::UNKNOWN;
        next.lidActive = (lastHbByte1_ == 0x04);
        next.openedAtMs = currentEvent_.openedAtMs;
        next.closedAtMs = currentEvent_.closedAtMs;
        next.sessionGeneration = sessionGeneration_;
        if (next.active && !currentEvent_.active) {
            next.openedAtMs = nowMs;
            next.closedAtMs = 0;
        }
        if (!next.active && currentEvent_.active) {
            next.closedAtMs = nowMs;
        }
        retainEndedEvent(next, nowMs);
        currentEvent_ = next;
    }
    void testInjectBytes(const uint8_t* data, size_t len);
    void testSetLastUartByteMs(uint32_t ms);
    uint32_t testGetLastUartByteMs() const { return lastUartByteMs_; }
    uint32_t testGetLastFrameMs() const { return lastFrameMs_; }
    void testSetState(AlpState s, uint32_t nowMs = 0) {
        state_ = s;
        testSyncCurrentEvent(nowMs);
    }
    void testSetLastHbByte1(uint8_t byte1) { lastHbByte1_ = byte1; }
    void testSetLastHeartbeat(uint32_t ms) {
        lastHeartbeatMs_ = ms;
        heartbeatTimeoutArmed_ = true;
    }
    AlpState testGetState() const { return state_; }
    uint8_t testGetLastHbByte1() const { return lastHbByte1_; }
    bool testGetAlertDetectedViaHb() const { return alertDetectedViaHb_; }
    const uint8_t* testGetRingBuf() const { return ringBuf_; }
    size_t testGetRingLen() const { return ringLen_; }
    uint32_t testGetFirstFrameMs() const { return firstFrameMs_; }
    uint32_t testGetWarmUpPreambleMs() const { return warmUpPreambleMs_; }
    void testSetFirstFrameMs(uint32_t ms) { firstFrameMs_ = ms; }
    void testSetWarmUpPreambleMs(uint32_t ms) { warmUpPreambleMs_ = ms; }
    // Display-test seam for consumers that do not link the runtime implementation.
    void testSetEnabled(bool e) {
        enabled_ = e;
        begun_ = true;
        testSyncCurrentEvent();
    }
    void testOpenSession(AlpGunType gun, bool isWarmUp = false,
                         AlpLaserDirection direction = AlpLaserDirection::UNKNOWN, uint32_t nowMs = 0) {
        if (!session_.active) {
            ++sessionGeneration_;
        }
        session_.active = true;
        session_.isWarmUp = isWarmUp;
        session_.direction = direction;
        session_.gun = gun;
        testSyncCurrentEvent(nowMs);
    }
    void testCloseSession(uint32_t nowMs = 0) {
        session_.active = false;
        testSyncCurrentEvent(nowMs);
    }
    void testTransitionTo(AlpState state, uint32_t nowMs) { transitionTo(state, nowMs); }
#endif

  private:
    SystemEventBus* bus_ = nullptr;
    bool enabled_ = false;
    bool begun_ = false;
    AlpState state_ = AlpState::OFF;

    static constexpr size_t RING_CAPACITY = 64;
    uint8_t ringBuf_[RING_CAPACITY] = {};
    size_t ringLen_ = 0;

    AlpGunType lastGun_ = AlpGunType::UNKNOWN;
    uint32_t lastGunTimestampMs_ = 0;
    // Persists across sessions while UART remains live; silence timeout clears it.
    // A confirmed gun lets later sessions bypass the boot preamble/envelope gate.
    bool bootGunConfirmed_ = false;
    uint32_t lastHeartbeatMs_ = 0;
    // Separate timeout admission from timestamp 0 (valid at boot/rollover).
    bool heartbeatTimeoutArmed_ = false;
    uint32_t lastFrameMs_ = 0;
    uint32_t noiseWindowEntryMs_ = 0;
    uint32_t teardownEntryMs_ = 0;
    // Seed on ALERT_ACTIVE entry; only 98 detection triggers refresh it.
    // Repeated targeted heartbeats must not extend an alert indefinitely.
    uint32_t alertActiveWatchdogMs_ = 0;
    uint8_t lastHbByte1_ = 0xFF;      // most recent B0 heartbeat byte1
    bool alertDetectedViaHb_ = false; // true when byte1 transitioned to 01
    // Teardown expiry blocks steady 01 and 01->00->01 reopening for one pass.
    // A 98 trigger can reopen immediately; heartbeats can reopen next tick.
    bool suppressHeartbeatResumeThisProcess_ = false;
    bool uartHasReceivedData_ = false;
    // Raw UART byte arrival timestamp — updated on every drainUart byte read.
    // The separate armed bit makes timestamp 0 usable at boot/rollover.
    uint32_t lastUartByteMs_ = 0;
    bool uartSilenceTimeoutArmed_ = false;

    AlertSession session_;
    uint32_t sessionGeneration_ = 0;
    uint32_t firstFrameMs_ = 0;     // first valid frame after begin()
    uint32_t warmUpPreambleMs_ = 0; // F0/A8 within 5s of firstFrameMs_; 0 = not seen

    uint32_t consecutiveBadChecksums_ = 0;

    // Sparse product-domain detection identity. This is not a frame trace:
    // only the two accepted 98 detection forms advance the generation.
    uint32_t detectGeneration_ = 0;
    uint8_t detectRaw_[3] = {};

    AlpLaserEvent currentEvent_{};
    AlpLaserEvent endedEvent_{};
    uint32_t endedEventRevision_ = 0;
    void retainEndedEvent(const AlpLaserEvent& next, uint32_t nowMs) {
        if (!next.active && currentEvent_.active) {
            endedEvent_ = currentEvent_;
            endedEvent_.closedAtMs = nowMs;
            ++endedEventRevision_;
        }
    }
    void updateCurrentEvent(uint32_t nowMs);

    void publishDisplayEdge();

    void transitionTo(AlpState newState, uint32_t nowMs);
    void drainUart(uint32_t nowMs);
    void parseRingBuffer(uint32_t nowMs);
    bool tryParseFrame(uint32_t nowMs);
    void handleAlertFrame(uint8_t b1, uint8_t b2, uint32_t nowMs);
    void handleHeartbeatFrame(uint8_t b0, uint8_t b1, uint32_t nowMs);
    void handleGunCandidate(uint8_t b0, uint8_t b1, uint8_t b2, uint32_t nowMs);
    void handleRegisterFrame(uint8_t b2, uint32_t nowMs);
    void handleDiscoveryFrame(uint32_t nowMs);
    void consumeBytes(size_t count);
    void handleNoiseWindowTimeout(uint32_t nowMs);
    void handleTeardownTimeout(uint32_t nowMs);
    void handleHeartbeatTimeout(uint32_t nowMs);
    void handleAlertActiveTimeout(uint32_t nowMs);
    void sampleSessionDirection(uint8_t heartbeatByte1, uint32_t nowMs);
};
