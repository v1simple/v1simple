/**
 * AL Priority serial listener and laser-event state machine.
 *
 * Owns UART2 RX on GPIO 2 (ALP CPU TX, RJ-45 pin 2).
 * Receives ALP HiFi serial data at 19200 8N1, parses alert frames
 * for gun identification, and tracks laser-event lifecycle.
 * The listener is receive-only; no ALP TX or enable GPIO is assigned.
 *
 * Terminology (per AL Priority Software Solution Manual):
 *   The user configures one of three Advanced Options — PDC Only,
 *   PDC & DLI, or PDC & LID. Our unit runs PDC & LID. Within a drive,
 *   the ALP switches between operational states based on speed:
 *     - DLI  (Detection of Laser Interference) — below LID speed limit
 *     - LID  (Laser Interference Defense) — above LID speed limit
 *     - Warm-Up — 60s after boot or after a LID Time expires
 *   Names follow the manual's PDC, DLI, LID, and Warm-Up terminology.
 *
 * Design notes:
 *   - Receive-only: the ESP32 only listens to CPU TX (pin 2 on RJ-45).
 *   - Frames are 4 bytes: byte0 byte1 byte2 checksum.
 *     checksum = (byte0 + byte1 + byte2) & 0x7F.
 *     The native ALP suite pins validation and resynchronization behavior.
 *   - Alert detection uses two signals:
 *     1. LID-deploy trigger: 98 00 E3 7B — ALP is actively firing IR
 *        countermeasures at the source. Rare in daily driving (requires
 *        a real laser hit above LID speed with LID configured).
 *     2. Heartbeat byte1 transition: B0 01 XX = targeted, 02/03/04 = idle.
 *     The heartbeat transition is the universal indicator — works in
 *     all operational states (DLI, LID pre-deploy, LID post-deploy).
 *   - Gun identification comes from CX frames in the alert burst:
 *     - LID-deploy frame variant: CX 00 YY — fingerprint (byte0, byte2)
 *     - Detect frame variant:      CX YY 00 — fingerprint (byte0, byte1)
 *   - During laser events, UART is flooded with noise from detection
 *     circuitry crosstalk (DLI) or I2S speaker crosstalk (LID deploy).
 *     Checksum validation rejects noise; GPIO glitch filter adds HW rejection.
 *
 * begin() owns the UART lifecycle; process() runs from the main loop.
 */

#pragma once

#include <cstdint>
#include <cstddef>

#include "alp_laser_event.h"

class AlpSdLogger;
class SystemEventBus;

// ── ALP connection / protocol states ─────────────────────────────────

enum class AlpState : uint8_t {
    OFF = 0,      // Module off (alpEnabled == false)
    IDLE,         // UART open, waiting for first valid frame
    LISTENING,    // Receiving heartbeats — ALP CPU confirmed alive
    ALERT_ACTIVE, // Laser detected — heartbeat byte1=01 or 98 trigger
    NOISE_WINDOW, // Speaker alert active — UART data is glitch noise
    TEARDOWN,     // Register cleanup after alert, returning to idle
};

const char* alpStateName(AlpState s);

// ── Known gun fingerprints ───────────────────────────────────────────
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

// Display-side instrumentation should route through the ALP module's
// existing logging owner rather than reaching for its global instance.
void alpLogDisplayDecision(uint32_t nowMs, const char* event, const char* detail);

// ── Gun lookup ───────────────────────────────────────────────────────

struct AlpGunCode {
    uint8_t byte0;   // Gun family (CX frame byte0)
    uint8_t gunCode; // Gun identifier (CX frame byte2)
    AlpGunType gun;
};

AlpGunType alpLookupGun(uint8_t byte0, uint8_t gunCode);
AlpGunType alpLookupGunDetect(uint8_t byte0, uint8_t byte1);

// ── Checksum validation ─────────────────────────────────────────────
// All ALP frames: 4 bytes, checksum = (byte0 + byte1 + byte2) & 0x7F.

static inline uint8_t alpChecksum(uint8_t b0, uint8_t b1, uint8_t b2) {
    return (b0 + b1 + b2) & 0x7F;
}

static inline bool alpValidateChecksum(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t cs) {
    return cs == alpChecksum(b0, b1, b2);
}

// ── Snapshot for external consumers ──────────────────────────────────

struct AlpStatus {
    AlpState state;
    AlpGunType lastGun;
    uint32_t lastGunTimestampMs;      // millis() when gun was identified
    uint32_t lastHeartbeatMs;         // millis() of most recent valid frame
    uint32_t statusBurstCount;        // lifetime alert trigger count
    uint32_t heartbeatCount;          // lifetime heartbeat count
    uint32_t frameErrors;             // lifetime framing / checksum errors
    uint32_t noiseWindowCount;        // lifetime noise window entries
    uint8_t lastHbByte1;              // most recent B0 heartbeat byte1 (01=Targeted, 02=Warm-Up, 03=DLI, 04=LID)
    AlpLaserDirection laserDirection; // live-alert front/rear classifier projected for display/API
    uint8_t directionSampleByte1;     // raw B0 byte1 that latched laserDirection (0 when unknown)
    bool uartActive;                  // true if UART has received any data
    bool hasLaserEvent;               // mirror of AlpRuntimeModule::hasLaserEvent() — live alert indicator for display
};

// ── Alert Session ────────────────────────────────────────────────────
//
// A session is a single laser engagement from onset to final clear.
// It is an internal engagement envelope: gun and direction stay latched
// across short TEARDOWN↔ALERT_ACTIVE re-arms so the next live detect can
// resume with the same context. Display accessors intentionally surface
// only the live ALERT_ACTIVE / NOISE_WINDOW portion of that session.
//
// Lifecycle:
//   open   — LISTENING|IDLE → ALERT_ACTIVE (fresh engagement)
//   stays open — across TEARDOWN↔ALERT_ACTIVE cycling (in-engagement
//                re-arms driven by byte1 01↔02 or repeat 98 triggers).
//                The gun frame arrives once at the opening of an
//                engagement; persisting the session across re-arms
//                keeps the gun/direction context available when live
//                detection resumes without leaking it during the gap.
//   close  — TEARDOWN → LISTENING (real end), or heartbeat timeout
//            (ALP went silent) with a session still open.
//
// Warm-Up flag:
//   Set when a session opens inside the 35s preamble envelope, before the
//   first heartbeat, or while the heartbeat remains in an unconfirmed
//   detect mode. Cleared by the operational-confirmation paths in the frame
//   handlers below. While the flag is set, the V1-shape accessors
//   (hasLaserEvent, isLaserDetecting) return false so the display stays clean
//   through the ALP Warm-Up window. Per manual: Warm-Up runs ~60s at boot and
//   after a LID Time timeout.

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
    uint32_t triggerCount = 0;            // 98 frames in this session
    uint32_t rearmCount = 0;              // TEARDOWN→ALERT cycles within session
    uint8_t modeAtOpen = 0xFF;            // lastHbByte1_ when session opened:
                                          //   01 = Targeted (mid-engagement reopen)
                                          //   02 = Warm-Up
                                          //   03 = DLI active (below LID speed)
                                          //   04 = LID active (above LID speed)
                                          //   00/06 = transitional / engaged
                                          //   0xFF = unknown (default)
};

// ── Module ───────────────────────────────────────────────────────────

class AlpRuntimeModule {
  public:
    // RX only: ALP CPU TX on RJ-45 pin 2 connects to GPIO 2.
    static constexpr int ALP_RX_PIN = 2;

    // Protocol constants — all frames are 4 bytes with 7-bit checksum
    static constexpr uint32_t ALP_BAUD = 19200;
    static constexpr size_t FRAME_LEN = 4; // byte0 byte1 byte2 checksum

    // Alert trigger frame: 98 00 E3 7B
    static constexpr uint8_t ALERT_BYTE0 = 0x98;
    static constexpr uint8_t ALERT_BYTE1 = 0x00;
    static constexpr uint8_t ALERT_BYTE2 = 0xE3;

    // Heartbeat byte0 values
    static constexpr uint8_t HEARTBEAT_SINGLE_0 = 0xB0;
    static constexpr uint8_t HEARTBEAT_PAIRED_0 = 0xB8;
    static constexpr uint8_t HEARTBEAT_TRIPLE_0 = 0xE0;

    // Other known byte0 values
    static constexpr uint8_t DISCOVERY_BYTE0 = 0x91;
    static constexpr uint8_t SETUP_BYTE0_A8 = 0xA8;
    static constexpr uint8_t SETUP_BYTE0_F0 = 0xF0;

    // Timing thresholds
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

    // RESYNC log throttle: only log every Nth bad checksum outside NOISE_WINDOW
    static constexpr uint32_t RESYNC_LOG_INTERVAL = 16;

    // Warm-Up sessions are identified by an early F0/A8 preamble, a trigger
    // before the first heartbeat, or heartbeat mode 0x02. Gun identification
    // clears the flag because Warm-Up bursts do not identify a gun.
    static constexpr uint32_t WARM_UP_PREAMBLE_WINDOW_MS = 5000;
    static constexpr uint32_t WARM_UP_ENVELOPE_MS = 35000;

    /**
     * Initialize the module.
     * @param enabled  true to open UART2 and begin listening
     * @param sdLogger  optional SD logger (nullptr disables logging)
     */
    void begin(bool enabled, AlpSdLogger* sdLogger = nullptr);

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
     * NOT thread-safe. Reads module state (state_, counters, session,
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

    // ── V1-shape display projection ──────────────────────────────────
    //
    // These are the two questions the display should ask. Everything
    // else (state machine, TEARDOWN, Warm-Up windowing, re-arm
    // cycles) is parser-internal and should not leak into consumers.

    /**
     * Current laser event snapshot for display and API consumers.
     * Contains active status, gun ID, direction, and LID active flag.
     */
    const AlpLaserEvent& currentEvent() const { return currentEvent_; }

    /**
     * Is there a live laser alert that should be shown on the display?
     * True only while the ALP is actively alerting (ALERT_ACTIVE or
     * NOISE_WINDOW). False during Warm-Up, TEARDOWN/LISTENING gaps,
     * and when no engagement is active.
     */
    bool hasLaserEvent() const {
        // Mirrors updateCurrentEvent's active predicate. Display window
        // spans the full session lifetime — ALERT_ACTIVE, NOISE_WINDOW,
        // and TEARDOWN — because real laser engagements oscillate byte1
        // between Targeted (01) and IDLE (00), which bounces us through
        // TEARDOWN mid-engagement. See updateCurrentEvent() comment.
        // TEARDOWN is displayable only when a gun was identified (real
        // engagement); phantom sessions that entered ALERT_ACTIVE from
        // noise never get a gun-ID and must not surface during TEARDOWN.
        const bool teardownDisplayable = (state_ == AlpState::TEARDOWN) && (session_.gun != AlpGunType::UNKNOWN);
        return session_.active && !session_.isWarmUp &&
               (state_ == AlpState::ALERT_ACTIVE || state_ == AlpState::NOISE_WINDOW || teardownDisplayable);
    }

    /** Full current session for diagnostics / tests. */
    const AlertSession& currentSession() const { return session_; }

    /**
     * Log a display-side ALP decision change to the SD card.
     * Called by display pipeline when the ALP frequency override or
     * laser-active flag changes — keeps display decisions in the same
     * CSV timeline as protocol and session events.
     */
    void logDisplayDecision(uint32_t nowMs, const char* event, const char* detail);

    /**
     * Is the ALP connected and producing data? True when ALP is enabled
     * and in any state except OFF or IDLE.
     *
     * This reports connection, not authority. It is a precondition for
     * ALP owning the laser render, never the whole test — see
     * RenderFrameComposer::compose(), which suppresses the V1's laser
     * channel only when an ALP branch actually renders one. A connected
     * ALP with nothing on screen does not displace the V1.
     *
     * The V1 Gen2 and the ALP each drive their own speaker for laser, so
     * v1simple does not duplicate the laser channel when ALP is rendering
     * it. That also keeps V1 alert persistence from holding a duplicate
     * visual after an ALP engagement closes.
     */
    bool ownsLaserDisplay() const { return enabled_ && state_ != AlpState::OFF && state_ != AlpState::IDLE; }

#ifdef UNIT_TEST
    // ── Test instrumentation ─────────────────────────────────────────
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
        if (next.active && !currentEvent_.active) {
            next.openedAtMs = nowMs;
            next.closedAtMs = 0;
        }
        if (!next.active && currentEvent_.active) {
            next.closedAtMs = nowMs;
        }
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
    void testSetLastHeartbeat(uint32_t ms) { lastHeartbeatMs_ = ms; }
    AlpState testGetState() const { return state_; }
    uint32_t testGetHeartbeatCount() const { return heartbeatCount_; }
    uint32_t testGetStatusBurstCount() const { return statusBurstCount_; }
    uint32_t testGetFrameErrors() const { return frameErrors_; }
    uint32_t testGetNoiseWindowCount() const { return noiseWindowCount_; }
    uint8_t testGetLastHbByte1() const { return lastHbByte1_; }
    bool testGetAlertDetectedViaHb() const { return alertDetectedViaHb_; }
    const uint8_t* testGetRingBuf() const { return ringBuf_; }
    size_t testGetRingLen() const { return ringLen_; }
    // Session / Warm-Up instrumentation
    uint32_t testGetFirstFrameMs() const { return firstFrameMs_; }
    uint32_t testGetWarmUpPreambleMs() const { return warmUpPreambleMs_; }
    void testSetFirstFrameMs(uint32_t ms) { firstFrameMs_ = ms; }
    void testSetWarmUpPreambleMs(uint32_t ms) { warmUpPreambleMs_ = ms; }
    // Seam for other modules' tests (e.g. display pipeline tests that want
    // to exercise the ALP-active branch without linking alp_runtime_module.cpp).
    void testSetEnabled(bool e) {
        enabled_ = e;
        begun_ = true;
        testSyncCurrentEvent();
    }
    void testOpenSession(AlpGunType gun, bool isWarmUp = false,
                         AlpLaserDirection direction = AlpLaserDirection::UNKNOWN, uint32_t nowMs = 0) {
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
    uint32_t testGetDisplayLogSuppressedCount() const { return displayLogSuppressedCount_; }
    const char* testGetLastDisplayLogEvent() const { return lastDisplayLogEvent_; }
    const char* testGetLastDisplayLogDetail() const { return lastDisplayLogDetail_; }
#endif

  private:
    // ── State ────────────────────────────────────────────────────────
    AlpSdLogger* sdLogger_ = nullptr;
    SystemEventBus* bus_ = nullptr;
    bool enabled_ = false;
    bool begun_ = false;
    AlpState state_ = AlpState::OFF;

    // Ring buffer for incoming UART bytes
    static constexpr size_t RING_CAPACITY = 64;
    uint8_t ringBuf_[RING_CAPACITY] = {};
    size_t ringLen_ = 0;

    // Protocol tracking
    AlpGunType lastGun_ = AlpGunType::UNKNOWN;
    uint32_t lastGunTimestampMs_ = 0;
    // Boot-level latch: set the first time any gun identifies in this
    // module's lifetime, cleared only on module reconstruction. Distinct
    // from lastGun_, which is intentionally wiped at every fresh engagement
    // so a previous alert's gun can't leak into a new one. Used by the
    // freshEngagement Warm-Up gate: once any gun has been identified in this
    // boot, subsequent sessions bypass the preamble/envelope clause. Boot
    // Warm-Up never produces a gun-ID frame, so identification proves the boot
    // sequence has ended.
    bool bootGunConfirmed_ = false;
    uint32_t lastHeartbeatMs_ = 0;
    uint32_t lastFrameMs_ = 0;
    uint32_t noiseWindowEntryMs_ = 0;
    uint32_t teardownEntryMs_ = 0;
    // ALERT_ACTIVE watchdog anchor. Seeded on every state entry so alerts
    // opened by heartbeat byte1=01 cannot bypass the timeout, then refreshed
    // only by a 98 trigger rearm. Repeated Targeted heartbeats are not rearms:
    // captures show stale byte1=01 housekeeping continuing after alert close.
    uint32_t alertActiveWatchdogMs_ = 0;
    uint8_t lastHbByte1_ = 0xFF;      // most recent B0 heartbeat byte1
    bool alertDetectedViaHb_ = false; // true when byte1 transitioned to 01
    // One-process guard after TEARDOWN timeout for heartbeat-driven reopen.
    // Blocks the two known housekeeping shapes from the just-ended alert:
    //   (a) steady B0 01 resume in the same process() pass
    //   (b) cleanup bounce B0 01 -> 00 -> 01 in that same pass
    // Real reopen still works on the next tick or immediately via a 98
    // detect trigger frame.
    bool suppressHeartbeatResumeThisProcess_ = false;
    bool uartHasReceivedData_ = false;
    // Raw UART byte arrival timestamp — updated on every drainUart byte read
    // (and testInjectBytes). 0 until the first byte is observed.
    uint32_t lastUartByteMs_ = 0;

    // Session + Warm-Up tracking
    AlertSession session_;
    uint32_t firstFrameMs_ = 0;     // first valid frame after begin()
    uint32_t warmUpPreambleMs_ = 0; // F0/A8 within 5s of firstFrameMs_; 0 = not seen

    // Counters
    uint32_t statusBurstCount_ = 0;
    uint32_t heartbeatCount_ = 0;
    uint32_t frameErrors_ = 0;
    uint32_t noiseWindowCount_ = 0;
    uint32_t consecutiveBadChecksums_ = 0;

    // Display-edge logging.
    bool lastHasLaserEventLogged_ = false;
    void maybeLogDisplayWindowEdge(uint32_t nowMs);

    // ── Display-decision log dedup ───────────────────────────────────
    // logDisplayDecision() is a public SD-writer reachable from several
    // call-sites (display_indicators, display_pipeline_module, edge log).
    // Callers edge-gate independently; this final deduplication layer prevents
    // a missed gate from adding peak SD-write latency. These fields hold the
    // the body can drop an exact repeat. Cleared on every transitionTo()
    // so genuinely new state-driven events always re-emit after a state
    // change.
    char lastDisplayLogEvent_[32] = "";
    char lastDisplayLogDetail_[96] = "";
    uint32_t displayLogSuppressedCount_ = 0; // observability: total drops

    // Current display-event snapshot.
    AlpLaserEvent currentEvent_{};
    bool updateCurrentEvent(uint32_t nowMs);

    // ── Event bus publishing ──────────────────────────────────────────
    void publishDisplayEdge();

    // ── Internal methods ─────────────────────────────────────────────
    void transitionTo(AlpState newState, uint32_t nowMs);
    void drainUart(uint32_t nowMs);
    void parseRingBuffer(uint32_t nowMs);
    bool tryParseFrame(uint32_t nowMs);
    void handleAlertFrame(uint8_t b1, uint8_t b2, uint32_t nowMs);
    void handleHeartbeatFrame(uint8_t b0, uint8_t b1, uint8_t b2, uint32_t nowMs);
    void handleGunCandidate(uint8_t b0, uint8_t b1, uint8_t b2, uint32_t nowMs);
    void handleRegisterFrame(uint8_t b0, uint8_t b1, uint8_t b2, uint32_t nowMs);
    void handleDiscoveryFrame(uint8_t b1, uint8_t b2, uint32_t nowMs);
    void consumeBytes(size_t count);
    void handleNoiseWindowTimeout(uint32_t nowMs);
    void handleTeardownTimeout(uint32_t nowMs);
    void handleHeartbeatTimeout(uint32_t nowMs);
    void handleAlertActiveTimeout(uint32_t nowMs);
    void sampleSessionDirection(uint8_t heartbeatByte1, uint32_t nowMs);
};
