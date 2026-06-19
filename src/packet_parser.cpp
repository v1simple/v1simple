/**
 * ESP Packet Parser for V1 Gen2
 *
 * The V1G2 packets are framed with 0xAA ... 0xAB. Packet ID lives at byte 3,
 * payload begins at byte 5 (after dest/src/id/len).
 *
 * Protocol reference: v1g2-t4s3 (Kenny's original ESP32/T4 implementation)
 * This code maintains compatibility with the original Valentine Research protocol.
 * Packet IDs: 0x31 = display/update, 0x43 = alert table entries.
 */

#include "packet_parser.h"
#include "config.h"
#include "perf_metrics.h"  // perfRecordV1FirmwareVersion

namespace {
struct BandArrowData {
    bool laser = false;
    bool ka = false;
    bool k = false;
    bool x = false;
    bool mute = false;
    bool front = false;
    bool side = false;
    bool rear = false;
};

BandArrowData processBandArrow(uint8_t v) {
    BandArrowData d;
    d.laser = (v & 0b00000001) != 0;
    d.ka    = (v & 0b00000010) != 0;
    d.k     = (v & 0b00000100) != 0;
    d.x     = (v & 0b00001000) != 0;
    d.mute  = (v & 0b00010000) != 0;
    d.front = (v & 0b00100000) != 0;
    d.side  = (v & 0b01000000) != 0;
    d.rear  = (v & 0b10000000) != 0;
    return d;
}

// Decode V1's 7-segment bogey counter byte to a character
// Based on V1 protocol - shows J=Junk, P=Photo, volume digits, L=Logic, etc.
// Bit 7 = decimal point (returned separately)
// Returns: character to display, hasDot = true if decimal point should show
char decodeBogeyCounterByte(uint8_t bogeyImage, bool& hasDot) {
    hasDot = (bogeyImage & 0x80) != 0;  // Bit 7 = decimal point

    switch (bogeyImage & 0x7F) {
        case 6:   return '1';
        case 7:   return '7';
        case 24:  return '&';  // Little L (logic mode)
        case 28:  return 'u';
        case 30:  return 'J';  // Junk
        case 56:  return 'L';  // Logic
        case 57:  return 'C';
        case 62:  return 'U';
        case 63:  return '0';
        case 73:  return '#';  // LASER bars
        case 79:  return '3';
        case 88:  return 'c';
        case 91:  return '2';
        case 94:  return 'd';
        case 102: return '4';
        case 109: return '5';
        case 111: return '9';
        case 113: return 'F';
        case 115: return 'P';  // Photo radar
        case 119: return 'A';
        case 121: return 'E';
        case 124: return 'b';
        case 125: return '6';
        case 127: return '8';
        default:  return ' ';  // Blank/unknown
    }
}

bool isAsciiDigit(uint8_t v) {
    return v >= '0' && v <= '9';
}

} // namespace

PacketParser::PacketParser()
    : alertCount_(0) {
    alertChunkPresent_.fill(false);
    alertChunkCountTag_.fill(0);
    alertChunkRxMs_.fill(0);
    alertTableFirstSeenMs_.fill(0);
}

bool PacketParser::parse(const uint8_t* data, size_t length) {
    return parseInternal(data, length, false, 0);
}

bool PacketParser::parse(const uint8_t* data, size_t length, uint32_t nowMs) {
    return parseInternal(data, length, true, nowMs);
}

bool PacketParser::parseInternal(const uint8_t* data, size_t length, bool hasNowMs, uint32_t nowMs) {
    if (!data || length < 7) {
        return false;
    }
    if (data[0] != ESP_PACKET_START || data[length - 1] != ESP_PACKET_END) {
        return false;
    }

    uint8_t packetId = data[3];
    switch (packetId) {
        case PACKET_ID_WRITE_USER_BYTES:
        case PACKET_ID_TURN_OFF_DISPLAY:
        case PACKET_ID_TURN_ON_DISPLAY:
        case PACKET_ID_MUTE_ON:
        case PACKET_ID_MUTE_OFF:
        case 0x36:
        case PACKET_ID_REQ_WRITE_VOLUME:
        case PACKET_ID_RESP_USER_BYTES:
        case PACKET_ID_VERSION:
            break;
        default:
            if (!validatePacket(data, length)) {
                return false;
            }
            break;
    }

    const uint8_t* payload = (length > 5) ? &data[5] : nullptr;
    size_t payloadLen = (length > 6) ? length - 6 : 0; // drop start/dest/src/id/len/end

    switch (packetId) {
        case PACKET_ID_DISPLAY_DATA:
            return parseDisplayData(payload, payloadLen);
        case PACKET_ID_ALERT_DATA: {
            const uint32_t alertNowMs = hasNowMs ? nowMs : static_cast<uint32_t>(millis());
            return parseAlertData(payload, payloadLen, alertNowMs);
        }

        // ACK responses from V1 to our commands - silently ignore
        case PACKET_ID_WRITE_USER_BYTES:    // 0x13 - ACK for profile write
            return true;  // Acknowledged, no further processing needed
        case PACKET_ID_TURN_OFF_DISPLAY:    // 0x32 - ACK for display off
            // Update display power state (dark mode enabled)
            displayState_.displayOn = false;
            displayState_.hasDisplayOn = true;
            return true;
        case PACKET_ID_TURN_ON_DISPLAY:     // 0x33 - ACK for display on
            // Update display power state (dark mode disabled)
            displayState_.displayOn = true;
            displayState_.hasDisplayOn = true;
            return true;
        case PACKET_ID_MUTE_ON:             // 0x34 - ACK for mute on
        case PACKET_ID_MUTE_OFF:            // 0x35 - ACK for mute off
        case 0x36:                          // ACK for mode change (reqChangeMode)
        case PACKET_ID_REQ_WRITE_VOLUME:    // 0x39 - ACK for volume change
        case PACKET_ID_RESP_USER_BYTES:     // 0x12 - User bytes response
            return true;  // Acknowledged, no further processing needed

        case PACKET_ID_RESP_VERSION: {      // 0x02 - respVersion (reply from V1)
            // V1 protocol summary: docs/V1_PROTOCOL_REFERENCES.md#version-response.
            // The response payload is exactly 7 ASCII bytes:
            //   [0] = device letter ('v'=V1, 'C'=Concealed Display,
            //                        'R'=Remote Audio, 'S'=Savvy)
            //   [1] = major version digit
            //   [2] = literal '.'
            //   [3] = minor version digit
            //   [4] = revision digit 1
            //   [5] = revision digit 2
            //   [6] = engineering control number digit
            // Example bytes: 'v','4','.','1','0','2','8' → "v4.1028" → 41028.
            const size_t declaredPayloadLen = data[4];
            if (payload && payloadLen >= 7 && declaredPayloadLen >= 7) {
                const uint8_t letter = payload[0];
                const bool letterAlphabetic = (letter >= 'A' && letter <= 'Z') ||
                                              (letter >= 'a' && letter <= 'z');
                if (letterAlphabetic &&
                    isAsciiDigit(payload[1]) &&
                    payload[2] == '.' &&
                    isAsciiDigit(payload[3]) &&
                    isAsciiDigit(payload[4]) &&
                    isAsciiDigit(payload[5]) &&
                    isAsciiDigit(payload[6])) {
                    char major = static_cast<char>(payload[1]);
                    char minor = static_cast<char>(payload[3]);
                    char rev1 = static_cast<char>(payload[4]);
                    char rev2 = static_cast<char>(payload[5]);
                    char ctrl = static_cast<char>(payload[6]);

                    uint32_t version = static_cast<uint32_t>(major - '0') * 10000u +
                                       static_cast<uint32_t>(minor - '0') * 1000u +
                                       static_cast<uint32_t>(rev1 - '0') * 100u +
                                       static_cast<uint32_t>(rev2 - '0') * 10u +
                                       static_cast<uint32_t>(ctrl - '0');

                    // Only record main V1 firmware versions; ignore replies
                    // from other ESP devices on the bus (Concealed Display,
                    // Remote Audio, Savvy).
                    if (letter == 'v' || letter == 'V') {
                        // Log only when this is the first observation OR the
                        // reported version actually changed. V1 firmware does
                        // not hot-swap mid-session, so the steady-state cost
                        // of repeated 0x02 replies must be zero — a blocking
                        // Serial.printf on every reply adds tail-latency on
                        // the BLE-notify hot path (sd_max_peak_us /
                        // wifi_p95_us regressions).
                        const bool versionChanged =
                            !displayState_.hasV1Version ||
                            displayState_.v1FirmwareVersion != version;
                        displayState_.v1FirmwareVersion = version;
                        displayState_.hasV1Version = true;
                        // Surface the V1 firmware version through the perf-metrics channel
                        // for SD perf logs and serial diagnostics.
                        perfRecordV1FirmwareVersion(version);
                        if (versionChanged) {
                            Serial.printf("[PacketParser] V1 firmware version: %c%c.%c%c%c%c (v%lu)\n",
                                          major, '.', minor, rev1, rev2, ctrl, version);
                        }
                    }
                }
            }
            return true;
        }
        case PACKET_ID_RESP_ALL_VOLUME: {   // 0x3D - respAllVolume
            // Per Valentine AndroidESPLibrary2
            // ResponseAllVolume.java the payload is exactly 4 bytes:
            //   [0] = current main volume   (0..9)
            //   [1] = current muted volume  (0..9)
            //   [2] = saved main volume     (0..9)
            //   [3] = saved muted volume    (0..9)
            // This is the authoritative source — overwrite the aux2-derived
            // mainVolume/muteVolume values from display packets when present.
            const size_t declaredPayloadLen = data[4];
            if (payload && payloadLen >= 4 && declaredPayloadLen >= 4) {
                displayState_.mainVolume      = payload[0] & 0x0F;
                displayState_.muteVolume      = payload[1] & 0x0F;
                displayState_.savedMainVolume = payload[2] & 0x0F;
                displayState_.savedMuteVolume = payload[3] & 0x0F;
                displayState_.hasVolumeData   = true;
                displayState_.hasSavedVolume  = true;
            }
            return true;
        }
        case PACKET_ID_REQ_ALL_VOLUME:      // 0x3C - outbound request, ignore echoes
            return true;

        case PACKET_ID_VERSION:             // 0x01 - reqVersion
            // 0x01 is the OUTBOUND request id. The V1 should never send 0x01
            // back to us, but if a buggy peer or replay loop produces one we
            // ignore it silently rather than parsing it as a version reply.
            return true;

        default:
            // Unknown packet - silently ignore in hot path
            return false;
    }
}

bool PacketParser::validatePacket(const uint8_t* data, size_t length) {
    if (length < 8) {
        return false;
    }
    if (data[0] != ESP_PACKET_START || data[length - 1] != ESP_PACKET_END) {
        return false;
    }
    return true; // checksum intentionally not enforced; V1G2 can chunk packets
}

bool PacketParser::parseDisplayData(const uint8_t* payload, size_t length) {
    // Expected payload >= 8 bytes (matches v1g2-t4s3 parsing window)
    if (!payload || length < 8) {
        return false;
    }

    // Display packet structure:
    // docs/V1_PROTOCOL_REFERENCES.md#infdisplaydata.
    // payload[0] = bogey counter image1 — steady-displayed 7-segment byte (0-9, J=Junk, P=Photo, etc.)
    // payload[1] = bogey counter image2 — blink-off pair of payload[0] for the
    //             SAME single 7-segment LED. NOT a second physical digit. Segments
    //             lit in image1 but unlit in image2 are blinking on V1's hardware.
    //             FSD-002: older code treated image2 as a second digit, reversing
    //             V1's single-character junk/photo verdicts during blink phases.
    // payload[2] = LED bar bitmap
    // payload[3] = image1 (currently ON bits - bands/arrows)
    // payload[4] = image2 (steady/NOT-flashing bits)
    // payload[5] = auxData0 (status bits: soft/system/euro/display-active)
    // payload[6] = auxData1 (mode/bluetooth flags)
    // payload[7] = auxData2 (volume: upper=main, lower=mute)
    // For both bogey-counter bytes and band/arrow bytes the convention is the
    // same: bits in image1 but NOT in image2 = FLASHING. V1 hardware handles the
    // actual blink animation internally — we must do the same.

    // Decode bogey counter byte - shows what V1's display shows (J, P, volume, etc.)
    uint8_t bogeyByte = payload[0];
    bool hasDot = false;
    char bogeyChar = decodeBogeyCounterByte(bogeyByte, hasDot);
    uint8_t bogeyByte2 = payload[1];
    bool hasDot2 = false;
    char bogeyChar2 = decodeBogeyCounterByte(bogeyByte2, hasDot2);
    displayState_.bogeyCounterByte = bogeyByte;
    displayState_.bogeyCounterChar = bogeyChar;
    displayState_.bogeyCounterDot = hasDot;
    displayState_.bogeyCounterByte2 = bogeyByte2;
    displayState_.bogeyCounterChar2 = bogeyChar2;
    displayState_.bogeyCounterDot2 = hasDot2;

    uint8_t image1 = payload[3];
    uint8_t image2 = payload[4];

    // band/arrow information from image1
    BandArrowData arrow = processBandArrow(image1);
    decodeMode(payload, length);

    // Snapshot auxData0 status bits
    // up-front (docs/V1_PROTOCOL_REFERENCES.md#infdisplaydata):
    //   bit 0 (0x01) — isSoft         : audio mute (spec-true)
    //   bit 2 (0x04) — isSystemStatus : V1 actively searching for alerts
    // softMuted is exposed as its own field; systemStatus gates the band /
    // arrow indicators below.
    //
    // bit 3 (0x08) — isDisplayOn — is intentionally NOT mirrored here. A
    // renderer-side short-circuit on aux0 bit 3 would hide alerts during V1
    // dark mode (V1 wakes its display on alert; ours would stay blank) and
    // its off↔on transitions would cause full-frame clear()+repaint storms
    // that regress soak peak latencies. displayOn is still maintained from
    // the explicit 0x32 / 0x33 ACKs; aux0 bit 3 is not consumed until the
    // renderer can paint alerts on a dark base without throwing away its
    // frame.
    bool auxSystemStatus = false;
    if (length > 5) {
        const uint8_t aux0 = payload[5];
        displayState_.softMuted = (aux0 & 0x01) != 0;
        auxSystemStatus = (aux0 & 0x04) != 0;
    } else {
        displayState_.softMuted = false;
    }
    displayState_.systemStatus = auxSystemStatus;

    // Calculate flash bits: things that are ON (image1) but NOT steady (image2)
    // These bits should blink on our display
    uint8_t flashingBits = image1 & ~image2;

    // Band flash bits (lower nibble): L=0x01, Ka=0x02, K=0x04, X=0x08
    displayState_.bandFlashBits = flashingBits & 0x0F;

    // Arrow flash bits (upper nibble): Front=0x20, Side=0x40, Rear=0x80
    displayState_.flashBits = flashingBits & 0xE0;

    displayState_.activeBands = BAND_NONE;
    if (arrow.laser) displayState_.activeBands |= BAND_LASER;
    if (arrow.ka)    displayState_.activeBands |= BAND_KA;
    if (arrow.k)     displayState_.activeBands |= BAND_K;
    if (arrow.x)     displayState_.activeBands |= BAND_X;

    displayState_.arrows = DIR_NONE;
    if (arrow.front) displayState_.arrows = static_cast<Direction>(displayState_.arrows | DIR_FRONT);
    if (arrow.side)  displayState_.arrows = static_cast<Direction>(displayState_.arrows | DIR_SIDE);
    if (arrow.rear)  displayState_.arrows = static_cast<Direction>(displayState_.arrows | DIR_REAR);

    // Per Valentine InfDisplayData.isSystemStatus,
    // band/arrow data is only meaningful while the V1 is actively searching.
    // If aux0 bit 2 is clear, suppress active bands and arrows so we don't
    // present stale indicators upstream.
    if (length > 5 && !auxSystemStatus) {
        displayState_.activeBands = BAND_NONE;
        displayState_.arrows = DIR_NONE;
        displayState_.bandFlashBits = 0;
        displayState_.flashBits = 0;
    }

    // Mute from image1 bit 4.  Single-packet transients (V1 internal display
    // transitions) can briefly set this bit even when unmuted.  Require two
    // consecutive display packets with the bit set before committing to
    // muted=true.  Transition to unmuted is instant — no delay on unmute.
    const bool rawMuteBit = (image1 & 0x10) != 0;
    if (rawMuteBit) {
        if (displayMuteConfirmCount_ < 2) {
            ++displayMuteConfirmCount_;
        }
    } else {
        displayMuteConfirmCount_ = 0;
    }
    displayState_.muted = (displayMuteConfirmCount_ >= 2);

    // Extract volume from auxData2 — payload[7] when the payload region has at
    // least 9 bytes (8 display data + checksum).  The V1 protocol summary
    // documents that the checksum is inside the declared payload-length region:
    // docs/V1_PROTOCOL_REFERENCES.md#packet-framing.  The actual display data
    // ends one byte before the reported payloadLen.  Guard with
    // length > 8 to avoid misreading the checksum as volume, which can produce a
    // false mainVolume==0 and trigger spurious muted state.
    // mainVol = upper nibble, muteVol = lower nibble
    if (length > 8) {
        uint8_t auxData2 = payload[7];
        displayState_.mainVolume = (auxData2 & 0xF0) >> 4;
        displayState_.muteVolume = auxData2 & 0x0F;
        displayState_.hasVolumeData = true;  // Mark that we've received volume data
    }

    // V1 sends LED bar state directly in the display packet at payload[2].
    // This is the authoritative signal strength from V1's own display, so
    // preserve it exactly for the simple signal bars (including laser frames).
    // Bitmap: 0x01=1bar, 0x03=2bars, 0x07=3bars, 0x0F=4bars,
    // 0x1F=5bars, 0x3F=6bars. Wider/overflow bitmaps still mean "full"
    // on the six-bar V1 Gen2 display.
    if (length > 2) {
        uint8_t ledBitmap = payload[2];
        displayState_.signalBars = decodeLEDBitmap(ledBitmap);
    }

    // AndroidESPLibrary2 treats display aux0 as status flags (soft mute/system/euro/display active),
    // not as a direct alert-table priority index. Priority selection is resolved from alert rows.

    return true;
}

// Decode V1's LED bitmap to bar count (0-6)
// V1 sends signal strength as consecutive bits from LSB: 1=1bar, 3=2bars, 7=3bars, etc.
uint8_t PacketParser::decodeLEDBitmap(uint8_t bitmap) const {
    // First try the expected bitmap pattern
    switch (bitmap) {
        case 1:   return 1;
        case 3:   return 2;
        case 7:   return 3;
        case 15:  return 4;
        case 31:  return 5;
        case 63:  return 6;
        case 127:
        case 255:
            return 6;
        case 0:   return 0;
        default:
            // VR InfDisplayData.getNumberOfLEDS() returns a full-bar
            // overflow sentinel) for any byte that doesn't match a
            // standard 0..255 contiguous bar pattern. See VR's
            // ESPLibrary2.0 packets/InfDisplayData.java lines 648-677.
            // The V1 Gen2 has six physical bars, so clamp overflow to six;
            // popcount of the bitmap would be non-spec.
            return 6;
    }
}

void PacketParser::decodeMode(const uint8_t* payload, size_t length) {
    // V1 protocol summary: docs/V1_PROTOCOL_REFERENCES.md#infdisplaydata.
    // Mode is only recoverable from the bogey-counter glyph while the V1 is
    // not using that glyph to show an alert count / verdict.
    // The V1 mode is encoded as a 7-segment glyph in the bogey-counter image
    // byte (payload[0]), with the high bit being the decimal point. The raw
    // (DP-stripped) seg-pattern values are the source of truth — there are
    // NO mode bits anywhere in auxData1 (the previous (aux1>>2)&0x03 decode
    // was an undocumented invention that disagreed with the official spec).
    //
    //   0x77 = 'A' All Bogeys Mode (USA)
    //   0x39 = 'C' K + Custom Sweeps (USA)
    //   0x3E = 'U' Euro: Ka and Ka(Photo)
    //   0x18 = 'l' Logic Mode (USA)
    //   0x1C = 'u' Euro: Ka only
    //   0x58 = 'c' Custom Sweeps (lower)
    //   0x38 = 'L' Advanced Logic Mode (USA)
    if (!payload || length < 1) {
        displayState_.modeChar = 0;
        displayState_.hasMode = false;
        return;
    }

    const uint8_t bogeyImage = payload[0] & 0x7F;
    char mode = 0;
    switch (bogeyImage) {
        case 0x77: mode = 'A'; break;
        case 0x39: mode = 'C'; break;
        case 0x3E: mode = 'U'; break;
        case 0x18: mode = 'l'; break;
        case 0x1C: mode = 'u'; break;
        case 0x58: mode = 'c'; break;
        case 0x38: mode = 'L'; break;
        default:   mode = 0;   break;  // Bogey shows a digit/other -> alerting,
                                       // mode not determinable.
    }
    displayState_.modeChar = mode;
    displayState_.hasMode = (mode != 0);
}
