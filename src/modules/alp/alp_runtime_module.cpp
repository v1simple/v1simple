// ALP frames: byte0 byte1 byte2 checksum, where checksum = (b0+b1+b2) & 0x7F.
// This runtime owns frame admission, sessions and display publication.

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

#include "../system/system_event_bus.h"
#include "modules/health/health_journal.h"

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

} // namespace

// Gun fingerprints are pinned by test_alp_runtime BURST_* fixtures; external
// provenance is unverified. Both variants use the same byte0 gun family:
//   LID deploy: CX 00 YY, fingerprint (byte0, byte2).
//   Detection:  CX YY 00, fingerprint (byte0, byte1).
// C8 00 04 is a generic detection trigger, not a gun identity.

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

// Detection variant: CX YY 00, byte1 != 0; fingerprint (byte0, byte1).

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

void AlpRuntimeModule::begin(bool enabled) {
    enabled_ = enabled;
    begun_ = true;

    // Reinitialization must not carry a stale session or warm-up state.
    session_ = AlertSession{};
    currentEvent_ = AlpLaserEvent{};
    firstFrameMs_ = 0;
    warmUpPreambleMs_ = 0;
    lastHeartbeatMs_ = 0;
    heartbeatTimeoutArmed_ = false;
    lastFrameMs_ = 0;
    lastUartByteMs_ = 0;
    uartSilenceTimeoutArmed_ = false;
    detectGeneration_ = 0;
    std::memset(detectRaw_, 0, sizeof(detectRaw_));

    if (!enabled) {
        state_ = AlpState::OFF;
        return;
    }

#ifndef UNIT_TEST
    // UART2: GPIO 2 RX only, 19200 8N1; no TX or enable signal.
    // Pull RX high before muxing to prevent a floating start bit. The ALP TX
    // driver overrides the ~45kOhm pull; do not drive this pin as an output.
    gpio_pullup_en(static_cast<gpio_num_t>(ALP_RX_PIN));

    Serial2.setRxBufferSize(UART_RX_BUFFER_SIZE);
    Serial2.begin(ALP_BAUD, SERIAL_8N1, ALP_RX_PIN, -1);

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
    }
#endif

    state_ = AlpState::IDLE;
}

void AlpRuntimeModule::process(uint32_t nowMs) {
    if (!begun_ || !enabled_)
        return;

    const AlpLaserEvent displayProjectionBefore = currentEvent_;

    // After teardown expires, queued steady 01 or 01->00->01 housekeeping
    // must not reopen the session during this process() pass.
    suppressHeartbeatResumeThisProcess_ = false;

    if (uartSilenceTimeoutArmed_ && nowMs - lastUartByteMs_ > HEARTBEAT_TIMEOUT_MS) {
        transitionTo(AlpState::IDLE, nowMs);
        lastHeartbeatMs_ = 0;
        heartbeatTimeoutArmed_ = false;
        lastUartByteMs_ = 0;
        uartSilenceTimeoutArmed_ = false;
        lastHbByte1_ = 0xFF;
        firstFrameMs_ = 0;
        warmUpPreambleMs_ = 0;
        bootGunConfirmed_ = false;
        alertDetectedViaHb_ = false;
        consecutiveBadChecksums_ = 0;
        ringLen_ = 0;
    }

    drainUart(nowMs);

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

    parseRingBuffer(nowMs);

    // Keep the atomic snapshot current even when no state transition fires.
    updateCurrentEvent(nowMs);

    // Gun/direction can arrive after the opening edge. Publish those changes
    // too; SystemEventBus coalesces them with any state edge from this pass.
    const bool displayProjectionChanged =
        displayProjectionBefore.active != currentEvent_.active ||
        displayProjectionBefore.gun != currentEvent_.gun ||
        displayProjectionBefore.direction != currentEvent_.direction ||
        displayProjectionBefore.lidActive != currentEvent_.lidActive;
    if (displayProjectionChanged) {
        publishDisplayEdge();
    }
}

AlpStatus AlpRuntimeModule::snapshot() const {
    AlpStatus s;
    s.state = state_;
    s.lastGun = lastGun_;
    s.lastGunTimestampMs = lastGunTimestampMs_;
    s.lastHeartbeatMs = lastHeartbeatMs_;
    s.lastHbByte1 = lastHbByte1_;
    s.laserDirection = currentEvent_.active ? currentEvent_.direction : AlpLaserDirection::UNKNOWN;
    s.directionSampleByte1 = currentEvent_.active ? session_.directionSampleByte1 : 0x00;
    s.uartActive = uartHasReceivedData_;
    s.hasLaserEvent = currentEvent_.active;
    s.detectGeneration = detectGeneration_;
    std::memcpy(s.detectRaw, detectRaw_, sizeof(s.detectRaw));
    return s;
}

void AlpRuntimeModule::updateCurrentEvent(uint32_t nowMs) {
    AlpLaserEvent next;
    // Targeted (01) / idle (00) oscillation can enter TEARDOWN mid-engagement.
    // Retain an identified gun through that gap until session close. Unknown
    // teardown stays hidden so a transient cannot paint the full 5-second tail.
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

    currentEvent_ = next;
}

void AlpRuntimeModule::publishDisplayEdge() {
    if (!bus_)
        return;
    bus_->publishAlpStateChanged();
}

void AlpRuntimeModule::transitionTo(AlpState newState, uint32_t nowMs) {
    AlpState oldState = state_;

    // Heartbeat-opened alerts also need a watchdog. Only accepted 98 triggers
    // refresh it; ordinary heartbeats must not extend the alert indefinitely.
    if (newState == AlpState::ALERT_ACTIVE && oldState != AlpState::ALERT_ACTIVE) {
        alertActiveWatchdogMs_ = nowMs;
    }

    // TEARDOWN rearm keeps the session gun; a fresh IDLE/LISTENING engagement
    // clears it. A gun-ID frame need not repeat during rearm.
    if (newState == AlpState::ALERT_ACTIVE && (state_ == AlpState::LISTENING || state_ == AlpState::IDLE)) {
        lastGun_ = AlpGunType::UNKNOWN;
        lastGunTimestampMs_ = 0;
    }

    // Open on IDLE/LISTENING -> ALERT_ACTIVE; retain through TEARDOWN rearm
    // and NOISE_WINDOW; close on TEARDOWN -> LISTENING or UART silence -> IDLE.
    const bool freshEngagement =
        (newState == AlpState::ALERT_ACTIVE) && (state_ == AlpState::LISTENING || state_ == AlpState::IDLE);
    const bool engagementEnd = (newState == AlpState::LISTENING) && (state_ == AlpState::TEARDOWN);
    const bool silentReset = (newState == AlpState::IDLE);

    if (freshEngagement) {
        ++sessionGeneration_;
        // A confirmed gun ends the boot envelope for subsequent engagements.
        const bool inEnvelope = (warmUpPreambleMs_ != 0) && (firstFrameMs_ != 0) &&
                                (nowMs - firstFrameMs_) < WARM_UP_ENVELOPE_MS && !bootGunConfirmed_;
        // Suppress pre-heartbeat detections even without an F0/A8 preamble.
        const bool noHeartbeatYet = (lastHbByte1_ == 0xFF);
        // A bare 98 02 00 in heartbeat modes 00/02/06 remains unconfirmed.
        // Keep the session open but hidden until targeted 01, LID-active 04,
        // or a gun-ID confirmation releases warm-up.
        const bool unconfirmedDetectMode = isUnconfirmedDetectOpenMode(lastHbByte1_);
        const bool flagWarmUp = inEnvelope || noHeartbeatYet || unconfirmedDetectMode;
        session_ = AlertSession{};
        session_.active = true;
        updateCurrentEvent(nowMs);
        publishDisplayEdge();
        session_.startMs = nowMs;
        session_.isWarmUp = flagWarmUp;
        session_.modeAtOpen = lastHbByte1_;
    }
    if ((engagementEnd || silentReset) && session_.active) {
        session_.active = false;
        updateCurrentEvent(nowMs);
        publishDisplayEdge();
        session_.endMs = nowMs;
    }

    state_ = newState;

    updateCurrentEvent(nowMs);
    publishDisplayEdge();
}

void AlpRuntimeModule::drainUart(uint32_t nowMs) {
#ifndef UNIT_TEST
    const int available = Serial2.available();
    if (available <= 0)
        return;

    if (!uartHasReceivedData_) {
        uartHasReceivedData_ = true;
    }

    const size_t space = RING_CAPACITY - ringLen_;
    if (space == 0) {
        const size_t keep = RING_CAPACITY / 2;
        memmove(ringBuf_, ringBuf_ + (RING_CAPACITY - keep), keep);
        ringLen_ = keep;
        HealthCounters::recordInputDrop();
    }

    const size_t toRead = (space < (size_t)available) ? space : (size_t)available;
    const size_t bytesRead = Serial2.readBytes(ringBuf_ + ringLen_, toRead);
    ringLen_ += bytesRead;
    if (bytesRead > 0) {
        // Car-mode power-off watches raw-byte silence, including invalid
        // traffic: noise still proves the UART source is powered.
        lastUartByteMs_ = nowMs;
        uartSilenceTimeoutArmed_ = true;
    }
#endif
}

// All frames are 4 bytes with a supported header and 7-bit checksum.
// On invalid framing or checksum, advance 1 byte to resync. Consecutive
// rejected candidates from ALERT_ACTIVE trigger NOISE_WINDOW transition.

void AlpRuntimeModule::parseRingBuffer(uint32_t nowMs) {
    // Every attempt consumes at least one byte; drain all complete candidates
    // in this bounded batch while retaining an incomplete frame for next time.
    size_t maxIterations = RING_CAPACITY;

    while (ringLen_ >= FRAME_LEN && maxIterations-- > 0) {
        if (tryParseFrame(nowMs))
            continue;

        consecutiveBadChecksums_++;

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
                transitionTo(AlpState::ALERT_ACTIVE, nowMs);
                enterNoise = true;
            }
            if (enterNoise) {
                transitionTo(AlpState::NOISE_WINDOW, nowMs);
                noiseWindowEntryMs_ = nowMs;
            }
        }

        // Noise can precede intact frames in the same batch. Slide to the next
        // candidate instead of discarding bytes that have not been examined.
        consumeBytes(1);
    }
}

bool AlpRuntimeModule::tryParseFrame(uint32_t nowMs) {
    if (ringLen_ < FRAME_LEN)
        return false;

    const uint8_t b0 = ringBuf_[0];
    const uint8_t b1 = ringBuf_[1];
    const uint8_t b2 = ringBuf_[2];
    const uint8_t cs = ringBuf_[3];

    const bool isHeartbeat = b0 == HEARTBEAT_SINGLE_0 || b0 == HEARTBEAT_PAIRED_0 ||
                             b0 == HEARTBEAT_TRIPLE_0 || b0 == SETUP_BYTE0_A8 || b0 == SETUP_BYTE0_F0;
    const bool isGunCandidate = b0 >= 0xC8 && b0 <= 0xCE;
    const bool isRegister = b0 >= 0xD0 && b0 <= 0xD3;
    const bool supportedHeader = b0 == ALERT_BYTE0 || isHeartbeat || isGunCandidate || isRegister ||
                                 b0 == DISCOVERY_BYTE0;
    // A checksum collision on an unknown header can overlap a real alert.
    // Reject it before consuming four bytes or changing any valid-frame state.
    if (!supportedHeader || !alpValidateChecksum(b0, b1, b2, cs))
        return false;

    consecutiveBadChecksums_ = 0;
    heartbeatTimeoutArmed_ = true;

    // Anchor the warm-up window at the first valid frame.
    if (firstFrameMs_ == 0) {
        firstFrameMs_ = nowMs;
    }

    if (state_ == AlpState::NOISE_WINDOW) {
        transitionTo(AlpState::TEARDOWN, nowMs);
        teardownEntryMs_ = nowMs;
    }

    if (b0 == ALERT_BYTE0) {
        handleAlertFrame(b1, b2, nowMs);
    } else if (isHeartbeat) {
        handleHeartbeatFrame(b0, b1, nowMs);
    } else if (isGunCandidate) {
        handleGunCandidate(b0, b1, b2, nowMs);
    } else if (isRegister) {
        handleRegisterFrame(b2, nowMs);
    } else if (b0 == DISCOVERY_BYTE0) {
        handleDiscoveryFrame(nowMs);
    }

    consumeBytes(FRAME_LEN);
    return true;
}

// Only entry into TEARDOWN stamps teardownEntryMs_; housekeeping frames
// must not extend the timeout.

void AlpRuntimeModule::handleAlertFrame(uint8_t b1, uint8_t b2, uint32_t nowMs) {
    lastHeartbeatMs_ = nowMs;
    lastFrameMs_ = nowMs;

    if (b1 == ALERT_BYTE1 && b2 == ALERT_BYTE2) {
        detectRaw_[0] = ALERT_BYTE0;
        detectRaw_[1] = b1;
        detectRaw_[2] = b2;
        if (++detectGeneration_ == 0) {
            ++detectGeneration_;
        }
        // 98 00 E3: LID countermeasure deployment above the LID speed limit.
        alertActiveWatchdogMs_ = nowMs;

        if (state_ != AlpState::ALERT_ACTIVE) {
            transitionTo(AlpState::ALERT_ACTIVE, nowMs);
        }
        // LID deployment releases warm-up immediately, including during the
        // boot envelope. Revisit only if captures show this trigger at warm-up.
        if (session_.active && session_.isWarmUp) {
            session_.isWarmUp = false;
            updateCurrentEvent(nowMs);
            publishDisplayEdge();
        }
    } else if (b1 == 0x02 && b2 == 0x00) {
        detectRaw_[0] = ALERT_BYTE0;
        detectRaw_[1] = b1;
        detectRaw_[2] = b2;
        if (++detectGeneration_ == 0) {
            ++detectGeneration_;
        }
        // Detect trigger: 98 02 00 — generic laser-detected trigger, fires
        // in all operational states (DLI, LID pre-deploy, LID post-deploy).
        alertActiveWatchdogMs_ = nowMs;

        if (state_ != AlpState::ALERT_ACTIVE) {
            transitionTo(AlpState::ALERT_ACTIVE, nowMs);
        }
        // Repeated detect frames are not a Warm-Up release signal. Release is
        // based on confirmed heartbeat transitions instead.
    } else {
        if (state_ == AlpState::IDLE) {
            transitionTo(AlpState::LISTENING, nowMs);
        }
    }
}

void AlpRuntimeModule::handleHeartbeatFrame(uint8_t b0, uint8_t b1, uint32_t nowMs) {
    lastHeartbeatMs_ = nowMs;
    lastFrameMs_ = nowMs;

    // Latch the first F0/A8 preamble inside the initial 5-second window.
    if ((b0 == SETUP_BYTE0_F0 || b0 == SETUP_BYTE0_A8) && firstFrameMs_ != 0 && warmUpPreambleMs_ == 0 &&
        (nowMs - firstFrameMs_) < WARM_UP_PREAMBLE_WINDOW_MS) {
        warmUpPreambleMs_ = nowMs;
    }

    // B0 byte1 runtime mapping: 01 targeted, 02 warm-up, 03 DLI, 04 LID,
    // 00/06 transitional. Direction sampling uses 01 front and 03/04 rear.
    // Steady 01 may reopen after teardown expiry even without a new edge.
    if (b0 == HEARTBEAT_SINGLE_0) {
        uint8_t prevByte1 = lastHbByte1_;
        lastHbByte1_ = b1;
        const bool pastBootEnvelope = (firstFrameMs_ != 0) && (nowMs - firstFrameMs_) >= WARM_UP_ENVELOPE_MS;

        // Warm-up can include a targeted 01 blip. Release only after the
        // boot envelope or a confirmed gun, not on that heartbeat alone.
        const bool targetedHeartbeatReleaseConfirmed = pastBootEnvelope || session_.gun != AlpGunType::UNKNOWN;
        if (session_.active && session_.isWarmUp && b1 == HB_BYTE1_ALERT && targetedHeartbeatReleaseConfirmed) {
            session_.isWarmUp = false;
            updateCurrentEvent(nowMs);
            publishDisplayEdge();
        }

        // After the boot envelope, 02/06 -> 04 confirms by itself; -> 03
        // also needs a session-local gun ID. Accept 06 as an intermediate
        // warm-up state so 02->06->03 can release a confirmed session.
        const bool prevWasResting = (prevByte1 == 0x02 || prevByte1 == 0x06);
        const bool heartbeatReleaseConfirmed = (b1 == 0x04) || (b1 == 0x03 && session_.gun != AlpGunType::UNKNOWN);
        if (session_.active && session_.isWarmUp && pastBootEnvelope && prevWasResting && heartbeatReleaseConfirmed) {
            session_.isWarmUp = false;
            updateCurrentEvent(nowMs);
            publishDisplayEdge();
        }

        // Teardown expiry suppresses steady 01 and 01->00->01 housekeeping
        // for this pass only; a later tick can reopen the session.
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
                alertDetectedViaHb_ = true;

                if (state_ == AlpState::LISTENING || state_ == AlpState::IDLE) {
                    transitionTo(AlpState::ALERT_ACTIVE, nowMs);
                }
            } else if (b1 != HB_BYTE1_ALERT && prevByte1 == HB_BYTE1_ALERT) {
                alertDetectedViaHb_ = false;

                if (state_ == AlpState::ALERT_ACTIVE) {
                    transitionTo(AlpState::TEARDOWN, nowMs);
                    teardownEntryMs_ = nowMs;
                }
            }
        }
    }

    if (state_ == AlpState::IDLE) {
        transitionTo(AlpState::LISTENING, nowMs);
    }

    if (b0 == HEARTBEAT_SINGLE_0) {
        sampleSessionDirection(b1, nowMs);
    }

}

void AlpRuntimeModule::handleGunCandidate(uint8_t b0, uint8_t b1, uint8_t b2, uint32_t nowMs) {
    lastHeartbeatMs_ = nowMs;
    lastFrameMs_ = nowMs;

    // Gun fingerprint — two patterns:
    //   LID-deploy: CX 00 YY — byte1=0x00, fingerprint (byte0, byte2)
    //   Detect:     CX YY 00 — byte2=0x00, byte1!=0x00, fingerprint (byte0, byte1)
    AlpGunType gun = AlpGunType::UNKNOWN;
    if (b1 == 0x00) {
        gun = alpLookupGun(b0, b2);
    }
    if (gun == AlpGunType::UNKNOWN && b2 == 0x00 && b1 != 0x00) {
        gun = alpLookupGunDetect(b0, b1);
    }
    if (gun != AlpGunType::UNKNOWN) {
        lastGun_ = gun;
        lastGunTimestampMs_ = nowMs;
        bootGunConfirmed_ = true;
        // A gun ID confirms the session and immediately releases warm-up.
        if (session_.active) {
            session_.gun = gun;
            session_.gunIdentifiedMs = nowMs;
            if (session_.isWarmUp) {
                session_.isWarmUp = false;
            }
        }
    }

    if (state_ == AlpState::IDLE) {
        transitionTo(AlpState::LISTENING, nowMs);
    }
}

void AlpRuntimeModule::handleRegisterFrame(uint8_t b2, uint32_t nowMs) {
    lastHeartbeatMs_ = nowMs;
    lastFrameMs_ = nowMs;

    if (b2 == 0xFD) {
        if (state_ == AlpState::ALERT_ACTIVE) {
            transitionTo(AlpState::TEARDOWN, nowMs);
            teardownEntryMs_ = nowMs;
        }
    }

    if (state_ == AlpState::IDLE) {
        transitionTo(AlpState::LISTENING, nowMs);
    }
}

void AlpRuntimeModule::handleDiscoveryFrame(uint32_t nowMs) {
    lastHeartbeatMs_ = nowMs;
    lastFrameMs_ = nowMs;

    if (state_ == AlpState::IDLE) {
        transitionTo(AlpState::LISTENING, nowMs);
    }
}

void AlpRuntimeModule::consumeBytes(size_t count) {
    if (count >= ringLen_) {
        ringLen_ = 0;
        return;
    }
    memmove(ringBuf_, ringBuf_ + count, ringLen_ - count);
    ringLen_ -= count;
}

void AlpRuntimeModule::handleHeartbeatTimeout(uint32_t nowMs) {
    if (!heartbeatTimeoutArmed_)
        return;
    if (nowMs - lastHeartbeatMs_ > HEARTBEAT_TIMEOUT_MS) {
        transitionTo(AlpState::IDLE, nowMs);
        lastHeartbeatMs_ = 0;
        heartbeatTimeoutArmed_ = false;
    }
}

void AlpRuntimeModule::handleNoiseWindowTimeout(uint32_t nowMs) {
    if (nowMs - noiseWindowEntryMs_ > NOISE_WINDOW_MAX_MS) {
        transitionTo(AlpState::TEARDOWN, nowMs);
        teardownEntryMs_ = nowMs;
    }
}

void AlpRuntimeModule::handleTeardownTimeout(uint32_t nowMs) {
    if (nowMs - teardownEntryMs_ > TEARDOWN_TIMEOUT_MS) {
        alertDetectedViaHb_ = false;
        // Block any queued heartbeat housekeeping frames from reopening
        // the session in this same process() pass. Real reopen still works
        // on the next tick or immediately via a 98 trigger frame.
        suppressHeartbeatResumeThisProcess_ = true;
        transitionTo(AlpState::LISTENING, nowMs);
    }
}

void AlpRuntimeModule::handleAlertActiveTimeout(uint32_t nowMs) {
    if (nowMs - alertActiveWatchdogMs_ > ALERT_ACTIVE_TIMEOUT_MS) {
        transitionTo(AlpState::TEARDOWN, nowMs);
        teardownEntryMs_ = nowMs;
    }
}

void AlpRuntimeModule::sampleSessionDirection(uint8_t heartbeatByte1, uint32_t nowMs) {
    if (!session_.active || state_ != AlpState::ALERT_ACTIVE) {
        return;
    }

    const AlpLaserDirection sampled = classifyLaserDirectionFromHeartbeatByte1(heartbeatByte1);
    if (sampled == AlpLaserDirection::UNKNOWN) {
        return;
    }

    // Rear classification requires a gun ID before a qualifying heartbeat;
    // unconfirmed DLI/LID chatter cannot establish direction. Targeted 01
    // self-anchors front, including before a gun ID in short engagements.
    const bool gunAnchored = (lastGun_ != AlpGunType::UNKNOWN);
    const bool selfAnchoringFront = (sampled == AlpLaserDirection::FRONT);
    if (!gunAnchored && !selfAnchoringFront) {
        return;
    }

    // Targeted 01 overrides an earlier rear sample. Other resampling keeps
    // the latched direction; a later 03/04 cannot replace front.
    if (session_.direction != AlpLaserDirection::UNKNOWN) {
        if (sampled == AlpLaserDirection::FRONT && session_.direction == AlpLaserDirection::REAR) {
            session_.direction = sampled;
            session_.directionSampleByte1 = heartbeatByte1;
        }
        return;
    }

    session_.direction = sampled;
    session_.directionSampleByte1 = heartbeatByte1;
}

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
    uartSilenceTimeoutArmed_ = true;
}
#endif
