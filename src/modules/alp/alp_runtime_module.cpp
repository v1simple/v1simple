/**
 * ALP Runtime Module — implementation.
 *
 * Rewritten April 2026 for the 4-byte frame protocol with 7-bit checksum.
 * All frames: byte0 byte1 byte2 checksum, where checksum = (b0+b1+b2) & 0x7F.
 *
 * Heavy logging throughout: this is early protocol integration and we need
 * visibility into every frame, state transition, and anomaly to validate
 * decoding against live captures. Logging will be dialed back once
 * field testing confirms reliability.
 */

#include "alp_runtime_module.h"
#include "alp_laser_event.h"

#include <cstdio>
#include <cstring>

#ifndef UNIT_TEST
#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/gpio_filter.h>
#include <soc/gpio_struct.h>
#endif

#include "alp_sd_logger.h"
#include "../system/system_event_bus.h"

// ── Global instance ──────────────────────────────────────────────────
// Follows the same pattern as ObdRuntimeModule in obd_runtime_module.cpp.
AlpRuntimeModule alpRuntimeModule;

// ── String helpers ───────────────────────────────────────────────────

const char* alpStateName(AlpState s) {
    switch (s) {
    case AlpState::OFF:
        return "OFF";
    case AlpState::IDLE:
        return "IDLE";
    case AlpState::LISTENING:
        return "LISTENING";
    case AlpState::ALERT_ACTIVE:
        return "ALERT_ACTIVE";
    case AlpState::NOISE_WINDOW:
        return "NOISE_WINDOW";
    case AlpState::TEARDOWN:
        return "TEARDOWN";
    default:
        return "UNKNOWN_STATE";
    }
}

const char* alpGunName(AlpGunType gun) {
    switch (gun) {
    case AlpGunType::UNKNOWN:
        return "Unknown";
    case AlpGunType::PL3_PROLITE:
        return "PL3 ProLite";
    case AlpGunType::DRAGONEYE_COMPACT:
        return "DragonEye Compact";
    case AlpGunType::LTI_TRUSPEED_LR:
        return "LTI TruSpeed LR";
    case AlpGunType::LASER_ATLANTA_PL2:
        return "Laser Atlanta PL2";
    case AlpGunType::MARKSMAN_ULTRALYTE:
        return "Marksman Ultralyte";
    case AlpGunType::STALKER_LZ1:
        return "Stalker LZ1";
    case AlpGunType::LASER_ALLY:
        return "Laser Ally";
    case AlpGunType::ATLANTA_STEALTH:
        return "Atlanta Stealth";
    default:
        return "INVALID_GUN";
    }
}

// 7-segment friendly abbreviations (~6 chars) for display frequency area.
// Only uses glyphs that render well on 7-seg: A b C d E F G H I J L n O P r S t U Y
const char* alpGunAbbrev(AlpGunType gun) {
    switch (gun) {
    case AlpGunType::PL3_PROLITE:
        return "PL3";
    case AlpGunType::DRAGONEYE_COMPACT:
        return "drgEYE";
    case AlpGunType::LTI_TRUSPEED_LR:
        return "truSPd";
    case AlpGunType::LASER_ATLANTA_PL2:
        return "PL2";
    case AlpGunType::MARKSMAN_ULTRALYTE:
        return "ULtrLt";
    case AlpGunType::STALKER_LZ1:
        return "StLr";
    case AlpGunType::LASER_ALLY:
        return "LALLY";
    case AlpGunType::ATLANTA_STEALTH:
        return "StLtH";
    default:
        return "LASER";
    }
}

const char* alpLaserDirectionName(AlpLaserDirection direction) {
    switch (direction) {
    case AlpLaserDirection::UNKNOWN:
        return "UNKNOWN";
    case AlpLaserDirection::FRONT:
        return "FRONT";
    case AlpLaserDirection::REAR:
        return "REAR";
    default:
        return "UNKNOWN";
    }
}

void alpLogDisplayDecision(uint32_t nowMs, const char* event, const char* detail) {
    alpRuntimeModule.logDisplayDecision(nowMs, event, detail);
}

namespace {

bool isUnconfirmedDetectOpenMode(uint8_t byte1) {
    return byte1 == 0x00 || byte1 == 0x02 || byte1 == 0x06;
}

AlpLaserDirection classifyLaserDirectionFromHeartbeatByte1(uint8_t byte1) {
    switch (byte1) {
    case 0x01:
        return AlpLaserDirection::FRONT;
    case 0x03:
    case 0x04:
        return AlpLaserDirection::REAR;
    default:
        return AlpLaserDirection::UNKNOWN;
    }
}

const char* sessionDirectionName(const AlertSession& session) {
    return alpLaserDirectionName(session.active ? session.direction : AlpLaserDirection::UNKNOWN);
}

} // namespace

// ── Gun code lookup table ────────────────────────────────────────────
// Sources: docs/ALP_PROTOCOL_EVIDENCE.md (gun-code fingerprints) + live captures.
//
// LID-deploy frame: CX 00 YY — fingerprint is (byte0, byte2). Fires only when
//                   LID is actively deploying countermeasures (above LID speed
//                   limit, real laser hit). Rare in daily driving.
// Detect frame:     CX YY 00 — fingerprint is (byte0, byte1). Fires in all
//                   operational states (DLI and LID) on every gun identification.
//
// byte0 matches between the two variants for the same physical gun.
// C8 00 04 is a generic "laser detected" trigger common to all guns in Detect mode.

static constexpr AlpGunCode GUN_TABLE[] = {
    {0xC8, 0xD5, AlpGunType::PL3_PROLITE},        {0xC8, 0xD6, AlpGunType::DRAGONEYE_COMPACT},
    {0xC9, 0xF5, AlpGunType::LTI_TRUSPEED_LR},    {0xCB, 0xEB, AlpGunType::LASER_ATLANTA_PL2},
    {0xCD, 0xD6, AlpGunType::MARKSMAN_ULTRALYTE}, {0xCD, 0xEB, AlpGunType::STALKER_LZ1},
    {0xCD, 0xD7, AlpGunType::LASER_ALLY},         {0xCE, 0xEB, AlpGunType::ATLANTA_STEALTH},
};
static constexpr size_t GUN_TABLE_SIZE = sizeof(GUN_TABLE) / sizeof(GUN_TABLE[0]);

AlpGunType alpLookupGun(uint8_t byte0, uint8_t gunCode) {
    for (size_t i = 0; i < GUN_TABLE_SIZE; ++i) {
        if (GUN_TABLE[i].byte0 == byte0 && GUN_TABLE[i].gunCode == gunCode) {
            return GUN_TABLE[i].gun;
        }
    }
    return AlpGunType::UNKNOWN;
}

// ── Detect-frame gun lookup table ───────────────────────────────────
// Pattern: CX YY 00 where byte2=0x00, byte1!=0x00.
// Fingerprint is (byte0, byte1). Live-captured April 2026.
// byte0 matches the LID-deploy gun family for the same physical gun.

static constexpr AlpGunCode DETECT_GUN_TABLE[] = {
    {0xC8, 0x0D, AlpGunType::PL3_PROLITE},        // live capture: PL3 ProLite
    {0xC8, 0x11, AlpGunType::DRAGONEYE_COMPACT},  // live capture: DragonEye Compact
    {0xC9, 0x0E, AlpGunType::LTI_TRUSPEED_LR},    // live capture: TruSpeed LR
    {0xCB, 0x10, AlpGunType::LASER_ATLANTA_PL2},  // live capture: Laser Atlanta PL2
    {0xCD, 0x0C, AlpGunType::MARKSMAN_ULTRALYTE}, // live capture: Ultralyte
    {0xCD, 0x0D, AlpGunType::STALKER_LZ1},        // live capture: Stalker LZ1
    {0xCD, 0x10, AlpGunType::LASER_ALLY},         // live capture: Laser Ally
    {0xCE, 0x0C, AlpGunType::ATLANTA_STEALTH},    // live capture: Atlanta Stealth
};
static constexpr size_t DETECT_GUN_TABLE_SIZE = sizeof(DETECT_GUN_TABLE) / sizeof(DETECT_GUN_TABLE[0]);

AlpGunType alpLookupGunDetect(uint8_t byte0, uint8_t byte1) {
    for (size_t i = 0; i < DETECT_GUN_TABLE_SIZE; ++i) {
        if (DETECT_GUN_TABLE[i].byte0 == byte0 && DETECT_GUN_TABLE[i].gunCode == byte1) {
            return DETECT_GUN_TABLE[i].gun;
        }
    }
    return AlpGunType::UNKNOWN;
}

// ── Logging helpers ──────────────────────────────────────────────────

#ifndef UNIT_TEST
#define ALP_LOG(fmt, ...) Serial.printf("[ALP] " fmt "\n", ##__VA_ARGS__)
// ALP_TRACE: high-frequency per-frame logging. Off by default.
// Enable via build flag -DALP_TRACE_ENABLED for serial capture sessions.
#ifdef ALP_TRACE_ENABLED
#define ALP_TRACE(fmt, ...) Serial.printf("[ALP] " fmt "\n", ##__VA_ARGS__)
#else
#define ALP_TRACE(fmt, ...) ((void)0)
#endif
#else
#define ALP_LOG(fmt, ...) ((void)0)
#define ALP_TRACE(fmt, ...) ((void)0)
#endif

// ── begin() ──────────────────────────────────────────────────────────

void AlpRuntimeModule::begin(bool enabled, AlpSdLogger* sdLogger) {
    sdLogger_ = sdLogger;
    enabled_ = enabled;
    begun_ = true;

    // Reset session + Warm-Up state on every begin(). This matters
    // when begin() is re-invoked after a settings change: we must not
    // carry a stale session across re-init.
    session_ = AlertSession{};
    firstFrameMs_ = 0;
    warmUpPreambleMs_ = 0;

    if (!enabled) {
        state_ = AlpState::OFF;
        ALP_LOG("begin: disabled");
        return;
    }

#ifndef UNIT_TEST
    // Configure UART2 at 19200 8N1, receive-only (no TX pin assigned).
    // GPIO 2 left undriven — formerly used as EN pin but ALP needs no
    // enable signal, and driving it HIGH may interfere with the ALP circuit.

    // Pull RX HIGH *before* Serial2.begin() so the pin is at UART idle level
    // during the GPIO-matrix mux transition. Without this the pin floats for
    // a brief window during init and the UART peripheral latches a spurious
    // start bit. The ~45kΩ internal pull is trivially overridden by the ALP
    // TX driver when the ALP is actually connected.
    gpio_pullup_en(static_cast<gpio_num_t>(ALP_RX_PIN));
    ALP_LOG("begin: RX pull-up enabled on pin %d", ALP_RX_PIN);

    Serial2.setRxBufferSize(UART_RX_BUFFER_SIZE);
    Serial2.begin(ALP_BAUD, SERIAL_8N1, ALP_RX_PIN, -1);
    ALP_LOG("begin: UART2 open baud=%lu RX=%d (TX=none) bufSize=%u", (unsigned long)ALP_BAUD, ALP_RX_PIN,
            (unsigned)UART_RX_BUFFER_SIZE);

    // Drain any bytes that arrived during UART peripheral init itself.
    while (Serial2.available())
        Serial2.read();

    // GPIO glitch filter on RX pin — rejects sub-10µs I2S crosstalk
    // during active LID deploy while passing valid 52.1µs UART bits.
    // Requires proper ground reference between ALP and ESP32.
    gpio_glitch_filter_handle_t filterHandle = nullptr;
    gpio_pin_glitch_filter_config_t filterConfig = {};
    filterConfig.clk_src = GLITCH_FILTER_CLK_SRC_DEFAULT;
    filterConfig.gpio_num = static_cast<gpio_num_t>(ALP_RX_PIN);
    esp_err_t err = gpio_new_pin_glitch_filter(&filterConfig, &filterHandle);
    if (err == ESP_OK && filterHandle) {
        gpio_glitch_filter_enable(filterHandle);
        ALP_LOG("begin: GPIO glitch filter enabled on pin %d", ALP_RX_PIN);
    } else {
        ALP_LOG("begin: WARNING — GPIO glitch filter failed err=%d", err);
    }
#endif

    state_ = AlpState::IDLE;
    ALP_LOG("begin: enabled -> IDLE");
}

// ── process() — main loop entry ──────────────────────────────────────

void AlpRuntimeModule::process(uint32_t nowMs) {
    if (!begun_ || !enabled_)
        return;

    // TEARDOWN timeout runs before parseRingBuffer(). If it returns us to
    // LISTENING, any queued housekeeping heartbeats still sitting in the
    // ring for this same process() call must not heartbeat-reopen the
    // session. Real captures bounce 01->00->01 during cleanup; both the
    // steady-01 resume and the same-process 00->01 edge must wait until
    // the next tick.
    suppressHeartbeatResumeThisProcess_ = false;

    // Drain UART into ring buffer
    drainUart(nowMs);

    // Timeout checks (state-dependent)
    switch (state_) {
    case AlpState::LISTENING:
        handleHeartbeatTimeout(nowMs);
        break;
    case AlpState::ALERT_ACTIVE:
        handleAlertActiveTimeout(nowMs);
        break;
    case AlpState::NOISE_WINDOW:
        handleNoiseWindowTimeout(nowMs);
        break;
    case AlpState::TEARDOWN:
        handleTeardownTimeout(nowMs);
        break;
    default:
        break;
    }

    // Parse whatever is in the ring buffer
    parseRingBuffer(nowMs);

    // Keep the atomic snapshot current even when no state transition fires.
    updateCurrentEvent(nowMs);

    // ── Instrumentation (Phase 0) ──────────────────────────────────────
    maybeLogDisplayWindowEdge(nowMs);
}

// ── snapshot() ───────────────────────────────────────────────────────

AlpStatus AlpRuntimeModule::snapshot() const {
    AlpStatus s;
    s.state = state_;
    s.lastGun = lastGun_;
    s.lastGunTimestampMs = lastGunTimestampMs_;
    s.lastHeartbeatMs = lastHeartbeatMs_;
    s.statusBurstCount = statusBurstCount_;
    s.heartbeatCount = heartbeatCount_;
    s.frameErrors = frameErrors_;
    s.noiseWindowCount = noiseWindowCount_;
    s.lastHbByte1 = lastHbByte1_;
    s.laserDirection = currentEvent_.active ? currentEvent_.direction : AlpLaserDirection::UNKNOWN;
    s.directionSampleByte1 = currentEvent_.active ? session_.directionSampleByte1 : 0x00;
    s.uartActive = uartHasReceivedData_;
    s.hasLaserEvent = currentEvent_.active;
    return s;
}

// ── Phase 2: Current event snapshot update ────────────────────────────

bool AlpRuntimeModule::updateCurrentEvent(uint32_t nowMs) {
    AlpLaserEvent next;
    // Display window spans the full session lifetime — ALERT_ACTIVE,
    // NOISE_WINDOW, and TEARDOWN. Real PL3/laser engagements cause byte1 to
    // oscillate between 01 (Targeted) and 00 (IDLE) as pulses fire and
    // quiet between, which flips us into TEARDOWN mid-engagement. The
    // session stays open (session_.active remains true) until engagementEnd
    // (TEARDOWN→LISTENING) or silentReset in transitionTo(). Gating the
    // event on ALERT_ACTIVE alone caused display cut-off at ~100-200 ms on
    // real PL3 captures (alp_6-61aa53a8.csv: DISPLAY_WINDOW_CLOSE at
    // +115 ms into a 5130 ms engagement).
    // TEARDOWN gating: a session that reached TEARDOWN without ever
    // identifying a gun is a phantom — ALP opened ALERT_ACTIVE from noise
    // and self-cleared without ever classifying laser energy. Real
    // engagements receive a GUN_ID C-frame while still ALERT_ACTIVE (or
    // at the latest within ~50 ms of entering TEARDOWN). Gating TEARDOWN
    // display-active on session_.gun != UNKNOWN prevents phantom sessions
    // from painting a 5-second false laser alert through the TEARDOWN
    // timeout (evidence: alp_6-61aa53a8.csv @ 48531 ms — mode=01 session
    // opens, flips ALERT_ACTIVE→TEARDOWN in ~0 ms, no gun ever identified,
    // would otherwise display for 5 s with the unconditional TEARDOWN gate).
    const bool teardownDisplayable = (state_ == AlpState::TEARDOWN) && (session_.gun != AlpGunType::UNKNOWN);
    next.active = session_.active && !session_.isWarmUp &&
                  (state_ == AlpState::ALERT_ACTIVE || state_ == AlpState::NOISE_WINDOW || teardownDisplayable);
    next.gun = next.active ? session_.gun : AlpGunType::UNKNOWN;
    next.direction = next.active ? session_.direction : AlpLaserDirection::UNKNOWN;
    next.lidActive = (lastHbByte1_ == 0x04);
    next.openedAtMs = currentEvent_.openedAtMs;
    next.closedAtMs = currentEvent_.closedAtMs;

    // Latch opened/closed timestamps on state edges
    if (next.active && !currentEvent_.active) {
        next.openedAtMs = nowMs;
        next.closedAtMs = 0;
    }
    if (!next.active && currentEvent_.active) {
        next.closedAtMs = nowMs;
    }

    // Check if anything relevant changed
    const bool changed = (next.active != currentEvent_.active) || (next.gun != currentEvent_.gun) ||
                         (next.direction != currentEvent_.direction) || (next.lidActive != currentEvent_.lidActive);

    currentEvent_ = next;
    return changed;
}

// ── Event bus publishing ──────────────────────────────────────────────

void AlpRuntimeModule::publishStateChangeEvent(uint32_t nowMs, uint16_t detail) {
    if (!bus_)
        return;
    SystemEvent e;
    e.type = SystemEventType::ALP_STATE_CHANGED;
    e.tsMs = nowMs;
    e.detail = detail;
    bus_->publish(e);
}

// ── State transitions ────────────────────────────────────────────────

void AlpRuntimeModule::transitionTo(AlpState newState, uint32_t nowMs) {
    AlpState oldState = state_;

    // Clear the display-decision dedup cache so state-driven edges always
    // re-emit after a state change (a "DISP_ALP_STATE A->B" transition
    // and any subsequent DISP_V1_EVENT / DISP_ALP_EVENT entries in that
    // state must not be suppressed just because they textually match the
    // previous state's last log).
    lastDisplayLogEvent_[0] = '\0';
    lastDisplayLogDetail_[0] = '\0';

    ALP_LOG("state: %s -> %s at %lu ms", alpStateName(state_), alpStateName(newState), (unsigned long)nowMs);
    if (sdLogger_) {
        sdLogger_->logStateTransition(nowMs, state_, newState, sessionDirectionName(session_));
    }

    // Clear stale gun ID only on a genuinely fresh engagement — one that
    // arrives from LISTENING or IDLE. TEARDOWN → ALERT_ACTIVE is the ALP
    // protocol's in-engagement re-arm cycle (driven by byte1 01→02→01 or
    // by a second 98 trigger during post-alert teardown). The gun frame
    // arrives only once, at the opening of an engagement — it does not
    // re-arrive on re-arms. Clearing on every ALERT_ACTIVE entry wiped the
    // gun mid-engagement.
    //
    // Regression reference: alp_18.csv at 36.768s identified PL2, then at
    // 46.352s the byte1 01→02 drop + immediate 98 02 00 re-arm wiped it,
    // leaving ~20s of the same alert showing generic "LASER" on the display
    // instead of "PL2". The LISTENING/IDLE narrowing preserves Rev 6's
    // "previous alert must not bleed through" guarantee (see
    // test_new_alert_clears_stale_gun_via_98_trigger etc.) while fixing
    // the in-engagement case (test_gun_persists_through_teardown_rearm).
    if (newState == AlpState::ALERT_ACTIVE && (state_ == AlpState::LISTENING || state_ == AlpState::IDLE)) {
        lastGun_ = AlpGunType::UNKNOWN;
        lastGunTimestampMs_ = 0;
    }

    // ── Session lifecycle ──────────────────────────────────────────────
    // Four edges matter:
    //
    //   (a) LISTENING|IDLE → ALERT_ACTIVE: open a new session. Flag as
    //       Warm-Up if we're inside the boot envelope AND a preamble
    //       was observed.
    //   (b) TEARDOWN → ALERT_ACTIVE: in-engagement re-arm. Session stays
    //       open; bump rearmCount for diagnostics.
    //   (c) TEARDOWN → LISTENING: real engagement end. Close the session.
    //   (d) * → IDLE with a session still open: heartbeat timeout killed
    //       the session. Close it with endMs marked.
    //
    // NOISE_WINDOW entries/exits do not alter session state — the session
    // straddles noise as part of a single engagement.
    const bool freshEngagement =
        (newState == AlpState::ALERT_ACTIVE) && (state_ == AlpState::LISTENING || state_ == AlpState::IDLE);
    const bool midEngagementRearm = (newState == AlpState::ALERT_ACTIVE) && (state_ == AlpState::TEARDOWN);
    const bool engagementEnd = (newState == AlpState::LISTENING) && (state_ == AlpState::TEARDOWN);
    const bool silentReset = (newState == AlpState::IDLE);

    if (freshEngagement) {
        // Envelope clause — only meaningful pre-first-gun-ID. Once any
        // gun has identified in this boot (bootGunConfirmed_), the ALP
        // has demonstrably left its post-boot Warm-Up sequence. Treating
        // post-gun-ID sessions as envelope-warm-up blacks out real
        // back-to-back engagements inside the 35s window. Evidence:
        // alp_5-3fe19956.csv sessions 3/4/5 opened on byte1=01 edges
        // after session #2 (PL3 gun-ID at +21.0s) closed, all within
        // the 35s envelope, all wrongly flagged, all dark for the
        // driver. Spec backs this: Warm-Up never emits a CX gun-ID,
        // so lastGun identification is proof Warm-Up has ended.
        const bool inEnvelope = (warmUpPreambleMs_ != 0) && (firstFrameMs_ != 0) &&
                                (nowMs - firstFrameMs_) < WARM_UP_ENVELOPE_MS && !bootGunConfirmed_;
        // Pre-heartbeat clause: if the ALP has never sent a B0 heartbeat,
        // the session cannot be a real engagement. Real engagements require
        // the ALP to have already completed enough boot sequencing to emit
        // at least one heartbeat. Closes a gap where cold-boot Warm-Up
        // emits 98 triggers before any F0/A8 preamble is observed (see
        // docs/plans/ALP_WARMUP_NO_HEARTBEAT_FIX.md for log evidence).
        const bool noHeartbeatYet = (lastHbByte1_ == 0xFF);
        // Unconfirmed-detect clause: a bare 98 02 00 that arrives while
        // the ALP still reports a resting/transitional heartbeat is not
        // display-ready. Field captures show generic detect triggers landing
        // in byte1=00, 02, and 06 with no gun frame and later painting a
        // false generic LASER. Keep the session open, but suppress it from
        // display until a real confirmation arrives (Targeted byte1=01,
        // LID-active byte1=04, or a gun-ID frame).
        const bool unconfirmedDetectMode = isUnconfirmedDetectOpenMode(lastHbByte1_);
        const bool flagWarmUp = inEnvelope || noHeartbeatYet || unconfirmedDetectMode;
        session_ = AlertSession{};
        session_.active = true;
        updateCurrentEvent(nowMs);
        publishStateChangeEvent(nowMs, 0x01);
        session_.startMs = nowMs;
        session_.isWarmUp = flagWarmUp;
        session_.modeAtOpen = lastHbByte1_;
        const char* warmUpReason =
            inEnvelope
                ? "preamble_envelope"
                : (noHeartbeatYet ? "no_heartbeat_yet" : (unconfirmedDetectMode ? "unconfirmed_detect_mode" : nullptr));
        if (flagWarmUp) {
            ALP_LOG("SESSION: open at +%lu ms — flagged WARM_UP (%s, suppressed from display)",
                    (unsigned long)(firstFrameMs_ != 0 ? nowMs - firstFrameMs_ : 0),
                    warmUpReason ? warmUpReason : "unknown");
        } else {
            ALP_LOG("SESSION: open at %lu ms — real engagement", (unsigned long)nowMs);
        }
        if (sdLogger_) {
            char openExtra[64];
            snprintf(openExtra, sizeof(openExtra), "warmUp=%d mode=%02X", flagWarmUp ? 1 : 0, lastHbByte1_);
            sdLogger_->logSessionEvent(nowMs, "SESSION_OPEN", state_, AlpGunType::UNKNOWN, openExtra,
                                       sessionDirectionName(session_));
        }
        if (flagWarmUp && sdLogger_) {
            sdLogger_->logSessionEvent(nowMs, "WARMUP_FLAG", state_, AlpGunType::UNKNOWN,
                                       warmUpReason ? warmUpReason : "unknown", sessionDirectionName(session_));
        }
    }
    if (midEngagementRearm && session_.active) {
        session_.rearmCount++;
        if (sdLogger_) {
            char extra[48];
            snprintf(extra, sizeof(extra), "rearm=%lu gun=%s", (unsigned long)session_.rearmCount,
                     alpGunName(session_.gun));
            sdLogger_->logSessionEvent(nowMs, "SESSION_REARM", state_, session_.gun, extra,
                                       sessionDirectionName(session_));
        }
    }
    if ((engagementEnd || silentReset) && session_.active) {
        ALP_LOG("SESSION: close at %lu ms  gun=%s  dur=%lu ms  triggers=%lu  rearms=%lu  warmUp=%d",
                (unsigned long)nowMs, alpGunName(session_.gun), (unsigned long)(nowMs - session_.startMs),
                (unsigned long)session_.triggerCount, (unsigned long)session_.rearmCount, session_.isWarmUp ? 1 : 0);
        if (sdLogger_) {
            char extra[96];
            snprintf(extra, sizeof(extra), "dur=%lu trig=%lu rearm=%lu warmUp=%d",
                     (unsigned long)(nowMs - session_.startMs), (unsigned long)session_.triggerCount,
                     (unsigned long)session_.rearmCount, session_.isWarmUp ? 1 : 0);
            sdLogger_->logSessionEvent(nowMs, "SESSION_CLOSE", state_, session_.gun, extra,
                                       sessionDirectionName(session_));
        }
        session_.active = false;
        updateCurrentEvent(nowMs);
        publishStateChangeEvent(nowMs, 0x02);
        session_.endMs = nowMs;
    }

    state_ = newState;

    // ── Publish state-change event to display pipeline ──────────────────
    uint16_t detail = 0x00;
    if (newState == AlpState::ALERT_ACTIVE) {
        detail = 0x10;
    } else if (oldState == AlpState::ALERT_ACTIVE) {
        detail = 0x11;
    } else if (newState == AlpState::NOISE_WINDOW) {
        detail = 0x12;
    } else if (oldState == AlpState::NOISE_WINDOW) {
        detail = 0x13;
    }
    updateCurrentEvent(nowMs);
    publishStateChangeEvent(nowMs, detail);
}

// ── UART drain ───────────────────────────────────────────────────────

void AlpRuntimeModule::drainUart(uint32_t nowMs) {
#ifndef UNIT_TEST
    const int available = Serial2.available();
    if (available <= 0)
        return;

    // First data ever — log it
    if (!uartHasReceivedData_) {
        uartHasReceivedData_ = true;
        ALP_LOG("UART first data at %lu ms (%d bytes available)", (unsigned long)nowMs, available);
    }

    // Read into ring buffer, up to remaining capacity
    const size_t space = RING_CAPACITY - ringLen_;
    if (space == 0) {
        // Ring full — discard oldest bytes to make room
        ALP_LOG("WARNING: ring buffer full, discarding %u bytes", (unsigned)(RING_CAPACITY / 2));
        const size_t keep = RING_CAPACITY / 2;
        memmove(ringBuf_, ringBuf_ + (RING_CAPACITY - keep), keep);
        ringLen_ = keep;
        frameErrors_++;
    }

    const size_t toRead = (space < (size_t)available) ? space : (size_t)available;
    const size_t bytesRead = Serial2.readBytes(ringBuf_ + ringLen_, toRead);
    ringLen_ += bytesRead;
    if (bytesRead > 0) {
        // Stamp every batch of bytes — car-mode power-off path watches this.
        // Raw bytes (not valid frames) because ignition loss drops the line
        // to zero reception; noise-from-live produces bytes, not silence.
        lastUartByteMs_ = nowMs;
    }
#endif
}

// ── Ring buffer parsing ──────────────────────────────────────────────
// All frames are 4 bytes with 7-bit checksum. On valid checksum, dispatch
// by byte0. On invalid checksum, advance 1 byte to resync. Consecutive
// checksum failures from ALERT_ACTIVE trigger NOISE_WINDOW transition.

void AlpRuntimeModule::parseRingBuffer(uint32_t nowMs) {
    int maxIterations = 32;

    while (ringLen_ >= FRAME_LEN && maxIterations-- > 0) {
        // Try to parse a valid 4-byte frame at current position
        if (tryParseFrame(nowMs))
            continue;

        // Checksum failed — noise or misalignment
        consecutiveBadChecksums_++;
        frameErrors_++;

        // UART flood happens in BOTH DLI (detection-circuit crosstalk) and
        // LID (I2S speaker crosstalk during deploy). Enter NOISE_WINDOW from
        // ALERT_ACTIVE or LISTENING (if we've seen alert-mode heartbeats)
        // after enough consecutive failures.
        if (consecutiveBadChecksums_ >= NOISE_CHECKSUM_THRESHOLD && state_ != AlpState::NOISE_WINDOW) {
            bool enterNoise = false;
            if (state_ == AlpState::ALERT_ACTIVE) {
                enterNoise = true;
            } else if (state_ == AlpState::LISTENING && alertDetectedViaHb_) {
                // Heartbeat byte1=01 told us alert is active, then noise hit
                // before we saw a 98 trigger. Transition through ALERT_ACTIVE.
                ALP_LOG("ALERT via heartbeat byte1=01 + noise — entering ALERT_ACTIVE");
                transitionTo(AlpState::ALERT_ACTIVE, nowMs);
                statusBurstCount_++;
                enterNoise = true;
            }
            if (enterNoise) {
                ALP_LOG("NOISE: %lu consecutive bad checksums — entering NOISE_WINDOW",
                        (unsigned long)consecutiveBadChecksums_);
                if (sdLogger_)
                    sdLogger_->logEvent(nowMs, "NOISE_ENTER", state_, consecutiveBadChecksums_);
                transitionTo(AlpState::NOISE_WINDOW, nowMs);
                noiseWindowEntryMs_ = nowMs;
                noiseWindowCount_++;
            }
        }

        // During NOISE_WINDOW, drain the buffer efficiently (no per-byte log)
        if (state_ == AlpState::NOISE_WINDOW) {
            if (ringLen_ > FRAME_LEN) {
                // Keep one full 4-byte candidate frame so the next clean
                // frame can terminate NOISE_WINDOW immediately instead of
                // getting stranded behind a 3-byte tail.
                size_t discard = ringLen_ - FRAME_LEN;
                consumeBytes(discard);
            } else {
                consumeBytes(1);
            }
            continue;
        }

        // Not in noise window — throttled RESYNC logging
        if (consecutiveBadChecksums_ <= 3 || (consecutiveBadChecksums_ % RESYNC_LOG_INTERVAL) == 0) {
            ALP_TRACE("RESYNC: bad checksum at 0x%02X %02X %02X %02X (err#%lu)", ringBuf_[0], ringBuf_[1], ringBuf_[2],
                      ringBuf_[3], (unsigned long)consecutiveBadChecksums_);
        }
        consumeBytes(1);
    }
}

// ── Frame parser (checksum-validated dispatch) ───────────────────────

bool AlpRuntimeModule::tryParseFrame(uint32_t nowMs) {
    if (ringLen_ < FRAME_LEN)
        return false;

    const uint8_t b0 = ringBuf_[0];
    const uint8_t b1 = ringBuf_[1];
    const uint8_t b2 = ringBuf_[2];
    const uint8_t cs = ringBuf_[3];

    // Validate checksum — the core integrity check
    if (!alpValidateChecksum(b0, b1, b2, cs))
        return false;

    // Valid frame — reset bad checksum counter
    consecutiveBadChecksums_ = 0;

    // Capture first-valid-frame timestamp for the Warm-Up window
    // calculation. Session gating anchors on this.
    if (firstFrameMs_ == 0) {
        firstFrameMs_ = nowMs;
    }

    // If we were in NOISE_WINDOW, first valid frame = teardown
    if (state_ == AlpState::NOISE_WINDOW) {
        ALP_LOG("NOISE_WINDOW ended — first valid frame %02X %02X %02X %02X after %lu ms", b0, b1, b2, cs,
                (unsigned long)(nowMs - noiseWindowEntryMs_));
        transitionTo(AlpState::TEARDOWN, nowMs);
        teardownEntryMs_ = nowMs;
    }

    // Dispatch by byte0 range
    if (b0 == ALERT_BYTE0) {
        handleAlertFrame(b1, b2, nowMs);
    } else if (b0 == HEARTBEAT_SINGLE_0 || b0 == HEARTBEAT_PAIRED_0 || b0 == HEARTBEAT_TRIPLE_0 ||
               b0 == SETUP_BYTE0_A8 || b0 == SETUP_BYTE0_F0) {
        handleHeartbeatFrame(b0, b1, b2, nowMs);
    } else if (b0 >= 0xC8 && b0 <= 0xCE) {
        handleGunCandidate(b0, b1, b2, nowMs);
    } else if (b0 >= 0xD0 && b0 <= 0xD3) {
        handleRegisterFrame(b0, b1, b2, nowMs);
    } else if (b0 == DISCOVERY_BYTE0) {
        handleDiscoveryFrame(b1, b2, nowMs);
    } else {
        // Valid checksum but unrecognized byte0 — treat as sign of life
        ALP_TRACE("UNKNOWN_FRAME: %02X %02X %02X %02X (valid checksum)", b0, b1, b2, cs);
        lastHeartbeatMs_ = nowMs;
        lastFrameMs_ = nowMs;
    }

    consumeBytes(FRAME_LEN);
    return true;
}

// ── Frame handlers ──────────────────────────────────────────────────

// TEARDOWN timer invariant: only transitions into TEARDOWN stamp
// teardownEntryMs_. Ordinary heartbeat/C-frame/register/discovery traffic
// that arrives while already in TEARDOWN is housekeeping from the same
// engagement and must not refresh the timer, or the TEARDOWN timeout can
// be extended indefinitely by cleanup chatter.

void AlpRuntimeModule::handleAlertFrame(uint8_t b1, uint8_t b2, uint32_t nowMs) {
    lastHeartbeatMs_ = nowMs;
    lastFrameMs_ = nowMs;

    if (b1 == ALERT_BYTE1 && b2 == ALERT_BYTE2) {
        // LID-deploy trigger: 98 00 E3 — ALP is actively firing IR
        // countermeasures at the source. Fires only when LID is above speed
        // limit and a real laser hit is detected. Rare in daily driving.
        statusBurstCount_++;
        lastAlertTriggerMs_ = nowMs;
        ALP_LOG("ALERT_TRIGGER: 98 00 E3 (LID deploy) — burst #%lu", (unsigned long)statusBurstCount_);
        if (sdLogger_)
            sdLogger_->logFrame(nowMs, "ALERT_LID_DEPLOY", ALERT_BYTE0, b1, b2, alpChecksum(ALERT_BYTE0, b1, b2),
                                state_, sessionDirectionName(session_));

        if (state_ != AlpState::ALERT_ACTIVE) {
            transitionTo(AlpState::ALERT_ACTIVE, nowMs);
        }
        if (session_.active)
            session_.triggerCount++;

        // LID deploy is the ALP actively firing countermeasures — an
        // unambiguous real engagement. Any Warm-Up flag that was speculatively
        // set at SESSION_OPEN is wrong, release immediately.
        // NOTE: No boot-envelope gate here — intentional. LID deploy during
        // boot Warm-Up is not observed in any capture (the probe burst at
        // ~5.9 s emits 02→04 heartbeats and detect triggers, but no 98 00 E3).
        // If a future capture shows LID deploy during boot, add the gate.
        if (session_.active && session_.isWarmUp) {
            ALP_LOG("WARM_UP: LID deploy observed — unmarking session as real");
            session_.isWarmUp = false;
            updateCurrentEvent(nowMs);
            publishStateChangeEvent(nowMs, 0x01);
            if (sdLogger_) {
                sdLogger_->logSessionEvent(nowMs, "WARMUP_RELEASE", state_, session_.gun, "lid_deploy",
                                           sessionDirectionName(session_));
            }
        }
    } else if (b1 == 0x02 && b2 == 0x00) {
        // Detect trigger: 98 02 00 — generic laser-detected trigger, fires
        // in all operational states (DLI, LID pre-deploy, LID post-deploy).
        statusBurstCount_++;
        lastAlertTriggerMs_ = nowMs;
        ALP_LOG("ALERT_TRIGGER: 98 02 00 (Detect) — burst #%lu", (unsigned long)statusBurstCount_);
        if (sdLogger_)
            sdLogger_->logFrame(nowMs, "ALERT_DETECT", ALERT_BYTE0, b1, b2, alpChecksum(ALERT_BYTE0, b1, b2), state_,
                                sessionDirectionName(session_));

        if (state_ != AlpState::ALERT_ACTIVE) {
            transitionTo(AlpState::ALERT_ACTIVE, nowMs);
        }
        if (session_.active)
            session_.triggerCount++;

        // NOTE: Earlier revisions released Warm-Up on triggerCount>=2 as a
        // "sustained detect" heuristic. That heuristic was wrong in both
        // directions: it promoted ALP boot Warm-Up sessions (which emit
        // two 98 02 00 frames during the rear-probe Warm-Up sequence,
        // evidence: alp_2-64991486.csv @ 5063/8358 ms) and it failed to
        // promote real DLI engagements where the ALP only sends a single
        // 98 02 00 frame (evidence: same log @ 109072 ms — 18.7 s of
        // byte1=03 heartbeats with only one Detect trigger). The correct
        // release signals come from the heartbeat byte1 edge (02→03/04
        // transition) handled in handleHeartbeatFrame(), not from
        // trigger-frame counting.
    } else {
        // Other 98 XX YY frames (status/config)
        ALP_TRACE("STATUS_FRAME: 98 %02X %02X  state=%s", b1, b2, alpStateName(state_));

        if (state_ == AlpState::IDLE) {
            transitionTo(AlpState::LISTENING, nowMs);
        }
    }
}

void AlpRuntimeModule::handleHeartbeatFrame(uint8_t b0, uint8_t b1, uint8_t b2, uint32_t nowMs) {
    lastHeartbeatMs_ = nowMs;
    lastFrameMs_ = nowMs;
    heartbeatCount_++;

    // Warm-Up preamble detection. F0 or A8 frames arriving inside the
    // first 5 seconds of module uptime mark the boot Warm-Up envelope.
    // We latch the first occurrence; later F0/A8 frames (which per spec
    // only arrive at cold boot anyway) are ignored.
    if ((b0 == SETUP_BYTE0_F0 || b0 == SETUP_BYTE0_A8) && firstFrameMs_ != 0 && warmUpPreambleMs_ == 0 &&
        (nowMs - firstFrameMs_) < WARM_UP_PREAMBLE_WINDOW_MS) {
        warmUpPreambleMs_ = nowMs;
        ALP_LOG("WARM_UP: %02X preamble at +%lu ms — Warm-Up envelope captured", b0,
                (unsigned long)(nowMs - firstFrameMs_));
    }

    ALP_TRACE("HEARTBEAT: %02X %02X %02X  state=%s  hb#=%lu", b0, b1, b2, alpStateName(state_),
              (unsigned long)heartbeatCount_);

    // ── Heartbeat byte1 alert detection (B0 frames only) ────────────
    // B0 heartbeats carry alert status in byte1 (per manual):
    //   byte1=01 → Targeted (laser detected)
    //   byte1=02 → Warm-Up (After LID Timeout or boot warm-up)
    //   byte1=03 → DLI active (Detection of Laser Interference)
    //   byte1=04 → LID active (Laser Interference Defense)
    //   byte1=00/06 → transitional / engaged
    // During ALERT_ACTIVE the same byte1 stream carries the direction
    // classifier: 01 → FRONT, 03/04 → REAR, 00/02 → defer/unknown.
    // The byte1 01 state is the universal alert indicator — normally we
    // key off the edge, but if TEARDOWN timed out back to LISTENING while
    // the ALP stayed Targeted we must reopen on the steady 01 as well.
    if (b0 == HEARTBEAT_SINGLE_0) {
        uint8_t prevByte1 = lastHbByte1_;
        lastHbByte1_ = b1;
        const bool pastBootEnvelope = (firstFrameMs_ != 0) && (nowMs - firstFrameMs_) >= WARM_UP_ENVELOPE_MS;

        // Targeted heartbeat release: byte1=01 is the ALP's universal
        // "laser detected" signal in steady-state operation. Boot Warm-Up
        // also emits a single ~800 ms byte1=01 blip inside the Warm-Up
        // envelope, so do not release speculative sessions on 01 until the
        // boot envelope has elapsed unless a real gun already identified.
        const bool targetedHeartbeatReleaseConfirmed = pastBootEnvelope || session_.gun != AlpGunType::UNKNOWN;
        if (session_.active && session_.isWarmUp && b1 == HB_BYTE1_ALERT) {
            if (targetedHeartbeatReleaseConfirmed) {
                ALP_LOG("WARM_UP: heartbeat -> 01 (Targeted) — unmarking session as real");
                session_.isWarmUp = false;
                updateCurrentEvent(nowMs);
                publishStateChangeEvent(nowMs, 0x01);
                if (sdLogger_) {
                    sdLogger_->logSessionEvent(nowMs, "WARMUP_RELEASE", state_, session_.gun, "hb_targeted",
                                               sessionDirectionName(session_));
                }
            } else {
                ALP_LOG("WARM_UP: heartbeat -> 01 inside boot envelope — keeping session suppressed");
            }
        }

        // Warm-Up release via 02/06→03/04 heartbeat edge. Per
        // docs/ALP_PROTOCOL_EVIDENCE.md, byte1=02 is Warm-Up/PDC-at-rest
        // and byte1=03/04 are DLI/LID-active states. Field captures show
        // that 02→03 can happen
        // after a bare detect-trigger with no gun identification, which
        // painted a false generic LASER on the display. Treat byte1=04 as
        // a standalone confirmation, but require a session-local gun ID
        // before byte1=03 can release the speculative Warm-Up flag.
        //
        // Boot-envelope gate: the ALP's post-boot Warm-Up walks through
        // the same 02→04 transition as part of its rear-probe sequence
        // (evidence: alp_2-64991486.csv @ 5144→5931 ms, B0 02→B0 04
        // inside the envelope). Without this gate, the release path
        // would promote the Warm-Up sequence to the display. After
        // WARM_UP_ENVELOPE_MS from firstFrameMs_, the ALP is committed
        // to operational state and 02→03/04 is real engagement.
        // Accept 0x06 (transitional/engaged) as equivalent to 0x02
        // for release purposes. Real captures show the ALP inserting
        // byte1=06 between Warm-Up (02) and operational modes (03/04),
        // producing a 02→06→03 sequence that never crosses the 02→03
        // edge. This caused 46% of real sessions to stay warm-up-
        // flagged for their entire duration (alp_6-61aa53a8.csv
        // session #1: 23.4s engagement, zero display output).
        const bool prevWasResting = (prevByte1 == 0x02 || prevByte1 == 0x06);
        const bool heartbeatReleaseConfirmed = (b1 == 0x04) || (b1 == 0x03 && session_.gun != AlpGunType::UNKNOWN);
        if (session_.active && session_.isWarmUp && pastBootEnvelope && prevWasResting && heartbeatReleaseConfirmed) {
            ALP_LOG("WARM_UP: heartbeat %02X -> %02X (DLI/LID engage) — unmarking session as real", prevByte1, b1);
            session_.isWarmUp = false;
            updateCurrentEvent(nowMs);
            publishStateChangeEvent(nowMs, 0x01);
            if (sdLogger_) {
                char reason[24];
                snprintf(reason, sizeof(reason), "hb_%02X_to_%02X", prevByte1, b1);
                sdLogger_->logSessionEvent(nowMs, "WARMUP_RELEASE", state_, session_.gun, reason,
                                           sessionDirectionName(session_));
            }
        }

        // During TEARDOWN, byte1 toggles rapidly between 01/00 as the CPU
        // does post-alert housekeeping. Two same-process shapes matter when
        // TEARDOWN times out back to LISTENING before the ring is drained:
        //   (a) steady 01 resume
        //   (b) 01 -> 00 -> 01 cleanup bounce
        // Neither is a real new encounter, so the one-process suppressor
        // blocks heartbeat-driven reopen for this process() pass only.
        if (state_ != AlpState::TEARDOWN) {
            const bool suppressSameProcessHeartbeatAlert =
                suppressHeartbeatResumeThisProcess_ && (state_ == AlpState::LISTENING || state_ == AlpState::IDLE);
            const bool heartbeatAlertEdge =
                (b1 == HB_BYTE1_ALERT && prevByte1 != HB_BYTE1_ALERT && !suppressSameProcessHeartbeatAlert);
            const bool heartbeatAlertResume =
                (b1 == HB_BYTE1_ALERT && prevByte1 == HB_BYTE1_ALERT &&
                 (state_ == AlpState::LISTENING || state_ == AlpState::IDLE) && !suppressSameProcessHeartbeatAlert &&
                 !suppressHeartbeatResumeThisProcess_);

            if (heartbeatAlertEdge || heartbeatAlertResume) {
                // Transition to alert — heartbeat either flipped to 01 or
                // remained at 01 across a TEARDOWN timeout back to LISTENING.
                alertDetectedViaHb_ = true;
                if (heartbeatAlertEdge) {
                    ALP_LOG("HB ALERT: byte1 %02X -> 01 — laser detected via heartbeat", prevByte1);
                    if (sdLogger_) {
                        sdLogger_->logHeartbeatByte1(
                            nowMs, prevByte1, b1, state_,
                            alpLaserDirectionName(classifyLaserDirectionFromHeartbeatByte1(b1)));
                    }
                } else {
                    ALP_LOG("HB ALERT RESUME: byte1 stayed at 01 after teardown — reopening alert");
                }

                if (state_ == AlpState::LISTENING || state_ == AlpState::IDLE) {
                    transitionTo(AlpState::ALERT_ACTIVE, nowMs);
                    statusBurstCount_++;
                }
            } else if (b1 != HB_BYTE1_ALERT && prevByte1 == HB_BYTE1_ALERT) {
                // Transition back to idle — alert resolved
                alertDetectedViaHb_ = false;
                ALP_LOG("HB IDLE: byte1 01 -> %02X — alert resolved via heartbeat", b1);
                if (sdLogger_) {
                    sdLogger_->logHeartbeatByte1(nowMs, prevByte1, b1, state_, sessionDirectionName(session_));
                }

                if (state_ == AlpState::ALERT_ACTIVE) {
                    transitionTo(AlpState::TEARDOWN, nowMs);
                    teardownEntryMs_ = nowMs;
                }
            }
        }
    }

    // State transitions (generic — applies to all heartbeat types)
    if (state_ == AlpState::IDLE) {
        transitionTo(AlpState::LISTENING, nowMs);
    }

    if (b0 == HEARTBEAT_SINGLE_0) {
        sampleSessionDirection(b1, nowMs);
    }

    // SD trace: log every heartbeat frame to capture the raw byte1
    // cycling pattern alongside the runtime's current direction view.
    if (sdLogger_) {
        sdLogger_->logHeartbeat(nowMs, b0, b1, b2, state_, sessionDirectionName(session_));
    }
}

void AlpRuntimeModule::handleGunCandidate(uint8_t b0, uint8_t b1, uint8_t b2, uint32_t nowMs) {
    lastHeartbeatMs_ = nowMs;
    lastFrameMs_ = nowMs;
    heartbeatCount_++;

    // Gun fingerprint — two patterns:
    //   LID-deploy: CX 00 YY — byte1=0x00, fingerprint (byte0, byte2)
    //   Detect:     CX YY 00 — byte2=0x00, byte1!=0x00, fingerprint (byte0, byte1)
    AlpGunType gun = AlpGunType::UNKNOWN;
    if (b1 == 0x00) {
        gun = alpLookupGun(b0, b2);
        if (gun != AlpGunType::UNKNOWN) {
            ALP_LOG("GUN IDENTIFIED (LID-deploy): byte0=0x%02X gunCode=0x%02X -> %s", b0, b2, alpGunName(gun));
            if (sdLogger_) {
                sdLogger_->logGunIdentified(nowMs, gun, b0, b2, false, state_, sessionDirectionName(session_));
            }
        }
    }
    if (gun == AlpGunType::UNKNOWN && b2 == 0x00 && b1 != 0x00) {
        gun = alpLookupGunDetect(b0, b1);
        if (gun != AlpGunType::UNKNOWN) {
            ALP_LOG("GUN IDENTIFIED (Detect): byte0=0x%02X byte1=0x%02X -> %s", b0, b1, alpGunName(gun));
            if (sdLogger_) {
                sdLogger_->logGunIdentified(nowMs, gun, b0, b1, true, state_, sessionDirectionName(session_));
            }
        }
    }
    if (gun != AlpGunType::UNKNOWN) {
        lastGun_ = gun;
        lastGunTimestampMs_ = nowMs;
        // Boot-level "we've seen a real gun" latch — survives
        // freshEngagement's lastGun_ wipe. See header comment on
        // bootGunConfirmed_ for the session 3/4/5 display regression.
        bootGunConfirmed_ = true;
        // Session-level update. Also: a real gun ID during the Warm-Up
        // window un-declares Warm-Up — the ALP's Warm-Up sequence fires
        // generic 98 02 00 triggers but never produces a CX gun frame,
        // so any gun-identified session is real by definition. This is
        // the safety release for the "real laser gun fired during boot
        // Warm-Up" corner case.
        if (session_.active) {
            session_.gun = gun;
            session_.gunIdentifiedMs = nowMs;
            if (sdLogger_) {
                sdLogger_->logSessionEvent(nowMs, "SESSION_GUN", state_, gun, nullptr, sessionDirectionName(session_));
            }
            if (session_.isWarmUp) {
                ALP_LOG("WARM_UP: gun %s identified — unmarking session as real", alpGunName(gun));
                session_.isWarmUp = false;
                if (sdLogger_) {
                    sdLogger_->logSessionEvent(nowMs, "WARMUP_RELEASE", state_, gun, "gun_identified",
                                               sessionDirectionName(session_));
                }
            }
        }
    }

    ALP_TRACE("C_FRAME: %02X %02X %02X  state=%s", b0, b1, b2, alpStateName(state_));

    // State transitions
    if (state_ == AlpState::IDLE) {
        transitionTo(AlpState::LISTENING, nowMs);
    }
}

void AlpRuntimeModule::handleRegisterFrame(uint8_t b0, uint8_t b1, uint8_t b2, uint32_t nowMs) {
    lastHeartbeatMs_ = nowMs;
    lastFrameMs_ = nowMs;
    heartbeatCount_++;

    ALP_TRACE("REGISTER_WRITE: %02X %02X %02X  state=%s", b0, b1, b2, alpStateName(state_));

    // FD terminator at byte2 signals return-to-idle
    if (b2 == 0xFD) {
        ALP_LOG("FD terminator in register write — entering teardown");
        if (state_ == AlpState::ALERT_ACTIVE) {
            transitionTo(AlpState::TEARDOWN, nowMs);
            teardownEntryMs_ = nowMs;
        }
    }

    // State transitions
    if (state_ == AlpState::IDLE) {
        transitionTo(AlpState::LISTENING, nowMs);
    }
}

void AlpRuntimeModule::handleDiscoveryFrame(uint8_t b1, uint8_t b2, uint32_t nowMs) {
    lastHeartbeatMs_ = nowMs;
    lastFrameMs_ = nowMs;

    ALP_TRACE("DISCOVERY: 91 %02X %02X (CPU polling for control set)", b1, b2);

    if (state_ == AlpState::IDLE) {
        transitionTo(AlpState::LISTENING, nowMs);
    }
}

// ── Ring buffer management ───────────────────────────────────────────

void AlpRuntimeModule::consumeBytes(size_t count) {
    if (count >= ringLen_) {
        ringLen_ = 0;
        return;
    }
    memmove(ringBuf_, ringBuf_ + count, ringLen_ - count);
    ringLen_ -= count;
}

// ── Timeout handlers ─────────────────────────────────────────────────

void AlpRuntimeModule::handleHeartbeatTimeout(uint32_t nowMs) {
    if (lastHeartbeatMs_ == 0)
        return;
    if (nowMs - lastHeartbeatMs_ > HEARTBEAT_TIMEOUT_MS) {
        ALP_LOG("HEARTBEAT TIMEOUT: no frame for %lu ms — ALP CPU silent", (unsigned long)(nowMs - lastHeartbeatMs_));
        transitionTo(AlpState::IDLE, nowMs);
        lastHeartbeatMs_ = 0;
    }
}

void AlpRuntimeModule::handleNoiseWindowTimeout(uint32_t nowMs) {
    if (nowMs - noiseWindowEntryMs_ > NOISE_WINDOW_MAX_MS) {
        ALP_LOG("NOISE_WINDOW TIMEOUT: no clean frame after %lu ms — forced teardown",
                (unsigned long)(nowMs - noiseWindowEntryMs_));
        transitionTo(AlpState::TEARDOWN, nowMs);
        teardownEntryMs_ = nowMs;
    }
}

void AlpRuntimeModule::handleTeardownTimeout(uint32_t nowMs) {
    if (nowMs - teardownEntryMs_ > TEARDOWN_TIMEOUT_MS) {
        ALP_LOG("TEARDOWN complete: returning to LISTENING after %lu ms", (unsigned long)(nowMs - teardownEntryMs_));
        alertDetectedViaHb_ = false;
        // Block any queued heartbeat housekeeping frames from reopening
        // the session in this same process() pass. Real reopen still works
        // on the next tick or immediately via a 98 trigger frame.
        suppressHeartbeatResumeThisProcess_ = true;
        transitionTo(AlpState::LISTENING, nowMs);
    }
}

void AlpRuntimeModule::handleAlertActiveTimeout(uint32_t nowMs) {
    if (lastAlertTriggerMs_ != 0 && nowMs - lastAlertTriggerMs_ > ALERT_ACTIVE_TIMEOUT_MS) {
        ALP_LOG("ALERT_ACTIVE timeout: no 98 trigger rearm in %lu ms — teardown",
                (unsigned long)(nowMs - lastAlertTriggerMs_));
        transitionTo(AlpState::TEARDOWN, nowMs);
        teardownEntryMs_ = nowMs;
    }
}

// ── Display decision passthrough ─────────────────────────────────────

void AlpRuntimeModule::sampleSessionDirection(uint8_t heartbeatByte1, uint32_t nowMs) {
    if (!session_.active || state_ != AlpState::ALERT_ACTIVE) {
        return;
    }

    const AlpLaserDirection sampled = classifyLaserDirectionFromHeartbeatByte1(heartbeatByte1);
    if (sampled == AlpLaserDirection::UNKNOWN) {
        return;
    }

    // Gun-ID anchor (docs/ALP_PROTOCOL_EVIDENCE.md § Direction anchoring).
    // Runtime rule: sample the first non-00 B0 byte1 *after* a GUN_ID
    // frame. Without a gun-ID the ALP has not yet classified the
    // hit and mid-boot DLI/LID chatter cannot be trusted as a direction
    // signal. Evidence: alp_5-3fe19956.csv session #7 — 16× byte1=03 with
    // zero GUN_ID frames would wrongly paint "laser rear" on a front-only
    // install.
    //
    // FRONT exception: byte1=0x01 (Targeted) self-anchors. The LED color
    // rule is definitive — Targeted = RED LED = laser in front. The ALP
    // raises the Targeted flag regardless of whether it has had time to
    // emit a CX gun-ID frame (front shots TEARDOWN within ~50 ms of the
    // gun-ID, per protocol §5.1). Gating FRONT on lastGun_ would lose
    // legitimate front classifications in fast TEARDOWN cases.
    const bool gunAnchored = (lastGun_ != AlpGunType::UNKNOWN);
    const bool selfAnchoringFront = (sampled == AlpLaserDirection::FRONT);
    if (!gunAnchored && !selfAnchoringFront) {
        return;
    }

    // FRONT override: byte1=01 (Targeted) is the manufacturer's unambiguous
    // FRONT signal per the LED color rule — Targeted = RED LED = laser in
    // front of the vehicle. In multi-phase sessions where pre-gun-ID
    // DLI/LID activity (byte1=03/04) latches REAR first, a later byte1=01
    // arrival re-latches to FRONT. This matches the driver's physical
    // observation of the ALP pad LED and resolves the §5.5 edge case
    // documented in docs/plans/ALP_LASER_DIRECTION_CLASSIFIER_20260418.md.
    // REAR→REAR and FRONT→REAR re-sampling is a no-op (REAR is sticky
    // except against Targeted).
    if (session_.direction != AlpLaserDirection::UNKNOWN) {
        if (sampled == AlpLaserDirection::FRONT && session_.direction == AlpLaserDirection::REAR) {
            ALP_LOG("SESSION: direction re-latched FRONT via B0 byte1=01 at %lu ms (was REAR)", (unsigned long)nowMs);
            session_.direction = sampled;
            session_.directionSampleByte1 = heartbeatByte1;
        }
        return;
    }

    session_.direction = sampled;
    session_.directionSampleByte1 = heartbeatByte1;
    ALP_LOG("SESSION: direction latched %s via B0 byte1=%02X at %lu ms", alpLaserDirectionName(sampled), heartbeatByte1,
            (unsigned long)nowMs);
}

void AlpRuntimeModule::logDisplayDecision(uint32_t nowMs, const char* event, const char* detail) {
    if (!event)
        return; // defensive — all current callers pass a literal

    // Strict last-event/detail dedup. All current callers (display_indicators,
    // display_pipeline_module, display_edge_log) are individually edge-gated,
    // but any future consumer that forgets to edge-gate would silent-tax the
    // SD path. The cache is cleared in transitionTo() so genuine state edges
    // still re-emit even if their textual form matches a prior state's log.
    const char* detailStr = detail ? detail : "";
    if (strncmp(lastDisplayLogEvent_, event, sizeof(lastDisplayLogEvent_)) == 0 &&
        strncmp(lastDisplayLogDetail_, detailStr, sizeof(lastDisplayLogDetail_)) == 0) {
        ++displayLogSuppressedCount_;
        return;
    }
    strncpy(lastDisplayLogEvent_, event, sizeof(lastDisplayLogEvent_) - 1);
    lastDisplayLogEvent_[sizeof(lastDisplayLogEvent_) - 1] = '\0';
    strncpy(lastDisplayLogDetail_, detailStr, sizeof(lastDisplayLogDetail_) - 1);
    lastDisplayLogDetail_[sizeof(lastDisplayLogDetail_) - 1] = '\0';

    ALP_LOG("DISPLAY: %s  %s", event, detailStr);
    if (sdLogger_) {
        sdLogger_->logSessionEvent(nowMs, event, state_, session_.active ? session_.gun : AlpGunType::UNKNOWN, detail,
                                   sessionDirectionName(session_));
    }
}

// ── Display window edge logging (Phase 0) ────────────────────────────

void AlpRuntimeModule::maybeLogDisplayWindowEdge(uint32_t nowMs) {
    const bool laserEventActive = currentEvent_.active;
    if (laserEventActive != lastHasLaserEventLogged_) {
        lastHasLaserEventLogged_ = laserEventActive;
        // Log to SD via existing infrastructure
        if (sdLogger_) {
            char detail[64];
            snprintf(detail, sizeof(detail), "state=%s session=%d isWarmUp=%d byte1=%02X", alpStateName(state_),
                     session_.active ? 1 : 0, session_.isWarmUp ? 1 : 0, lastHbByte1_);
            sdLogger_->logSessionEvent(
                nowMs, laserEventActive ? "SESSION_DISPLAY_WINDOW_OPEN" : "SESSION_DISPLAY_WINDOW_CLOSE", state_,
                session_.active ? session_.gun : AlpGunType::UNKNOWN, detail, sessionDirectionName(session_));
        }
    }
}

// ── Test instrumentation ─────────────────────────────────────────────

#ifdef UNIT_TEST
void AlpRuntimeModule::testInjectBytes(const uint8_t* data, size_t len) {
    const size_t space = RING_CAPACITY - ringLen_;
    const size_t toWrite = (len < space) ? len : space;
    memcpy(ringBuf_ + ringLen_, data, toWrite);
    ringLen_ += toWrite;
    if (!uartHasReceivedData_ && toWrite > 0) {
        uartHasReceivedData_ = true;
    }
}

void AlpRuntimeModule::testSetLastUartByteMs(uint32_t ms) {
    lastUartByteMs_ = ms;
}
#endif
