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
#include "v1_firmware_compat.h"
#include "v1_packet_framing.h"

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
    d.ka = (v & 0b00000010) != 0;
    d.k = (v & 0b00000100) != 0;
    d.x = (v & 0b00001000) != 0;
    d.mute = (v & 0b00010000) != 0;
    d.front = (v & 0b00100000) != 0;
    d.side = (v & 0b01000000) != 0;
    d.rear = (v & 0b10000000) != 0;
    return d;
}

// Decode V1's 7-segment bogey counter byte to a character
// Based on V1 protocol - shows J=Junk, P=Photo, volume digits, L=Logic, etc.
// Bit 7 = decimal point (returned separately)
// Returns: character to display, hasDot = true if decimal point should show
char decodeBogeyCounterByte(uint8_t bogeyImage, bool& hasDot) {
    hasDot = (bogeyImage & 0x80) != 0; // Bit 7 = decimal point

    switch (bogeyImage & 0x7F) {
    case 6:
        return '1';
    case 7:
        return '7';
    case 24:
        return '&'; // Little L (logic mode)
    case 28:
        return 'u';
    case 30:
        return 'J'; // Junk
    case 56:
        return 'L'; // Logic
    case 57:
        return 'C';
    case 62:
        return 'U';
    case 63:
        return '0';
    case 73:
        return '#'; // LASER bars
    case 79:
        return '3';
    case 88:
        return 'c';
    case 91:
        return '2';
    case 94:
        return 'd';
    case 102:
        return '4';
    case 109:
        return '5';
    case 111:
        return '9';
    case 113:
        return 'F';
    case 115:
        return 'P'; // Photo radar
    case 119:
        return 'A';
    case 121:
        return 'E';
    case 124:
        return 'b';
    case 125:
        return '6';
    case 127:
        return '8';
    default:
        return ' '; // Blank/unknown
    }
}

bool isAsciiDigit(uint8_t v) {
    return v >= '0' && v <= '9';
}

char decodeQualifiedModeObservation(const uint8_t* payload, uint32_t firmwareVersion) {
    if (!payload || !V1FirmwareCompat::capabilities(firmwareVersion).modeObservation) return 0;
    const bool euroMode = (payload[5] & 0x10) != 0;
    const bool customSweeps = (payload[5] & 0x20) != 0;
    switch ((payload[6] >> 2) & 0x03) {
    case 1: return euroMode ? (customSweeps ? 'C' : 'U') : 'A';
    case 2: return euroMode ? (customSweeps ? 'c' : 'u') : 'l';
    case 3: return euroMode ? 0 : 'L';
    default: return 0;
    }
}

} // namespace

PacketParser::PacketParser() : alertCount_(0) {
    alertChunkPresent_.fill(false);
    alertChunkCountTag_.fill(0);
    alertChunkRxMs_.fill(0);
    alertTableFirstSeenMs_.fill(0);
}

bool PacketParser::parse(const uint8_t* data, size_t length) {
    return parseInternal(data, length, false, 0, 0);
}

bool PacketParser::parse(const uint8_t* data, size_t length, uint32_t nowMs) {
    return parseInternal(data, length, true, nowMs, 0);
}

bool PacketParser::parse(const uint8_t* data, size_t length, uint32_t nowMs, uint32_t ingressSequence) {
    return parseInternal(data, length, true, nowMs, ingressSequence);
}

bool PacketParser::parseInternal(const uint8_t* data, size_t length, bool hasNowMs, uint32_t nowMs,
                                 uint32_t ingressSequence) {
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
    case PACKET_ID_RESP_CURRENT_VOLUME:
    case PACKET_ID_RESP_USER_BYTES:
    case PACKET_ID_VERSION:
    case PACKET_ID_RESP_REQUEST_NOT_PROCESSED:
    case PACKET_ID_INF_V1_BUSY:
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
    case PACKET_ID_DISPLAY_DATA: {
        // InfDisplayData is an eight-byte broadcast payload. Accept both
        // documented V1 originators (EAh with checksum and E9h without), but
        // never let corrupt or foreign traffic alter the live render state.
        if (!V1PacketFraming::hasCanonicalResponseEvidenceForDestination(data, length, 8, 0xD8)) {
            return false;
        }
        const bool hadAlerts = hasAlerts();
        const bool parsed = parseDisplayData(payload, payloadLen);
        if (parsed) {
            const uint32_t sequence = ++settingsObservationSequence_;
            ++displayOnObservation_.revision;
            displayOnObservation_.sequence = sequence;
            displayOnObservation_.ingressSequence = ingressSequence;
            displayOnObservation_.available = true;
            displayOnObservation_.value = (payload[5] & 0x08) != 0;

            // The Aux1 Bluetooth-image pair is a documented Gen2 4.1018+
            // field. Before a supported version is known, those bits are not
            // authoritative Bluetooth state and must remain unavailable.
            if (displayState_.hasV1Version &&
                V1FirmwareCompat::capabilities(displayState_.v1FirmwareVersion).customSweeps) {
                ++bluetoothIndicatorObservation_.revision;
                bluetoothIndicatorObservation_.sequence = sequence;
                bluetoothIndicatorObservation_.ingressSequence = ingressSequence;
                bluetoothIndicatorObservation_.image1 = (payload[6] & 0x40) != 0;
                bluetoothIndicatorObservation_.image2 = (payload[6] & 0x80) != 0;
                if (!bluetoothIndicatorObservation_.image1 && !bluetoothIndicatorObservation_.image2) {
                    bluetoothIndicatorObservation_.state = V1BluetoothIndicatorState::Off;
                    bluetoothIndicatorObservation_.available = true;
                } else if (bluetoothIndicatorObservation_.image1 != bluetoothIndicatorObservation_.image2) {
                    bluetoothIndicatorObservation_.state = V1BluetoothIndicatorState::Blinking;
                    bluetoothIndicatorObservation_.available = true;
                } else {
                    bluetoothIndicatorObservation_.state = V1BluetoothIndicatorState::On;
                    bluetoothIndicatorObservation_.available = true;
                }
            }

            if (displayState_.hasV1Version &&
                V1FirmwareCompat::capabilities(displayState_.v1FirmwareVersion).modeObservation) {
                ++modeObservation_.revision;
                modeObservation_.sequence = sequence;
                modeObservation_.ingressSequence = ingressSequence;
                modeObservation_.value =
                    decodeQualifiedModeObservation(payload, displayState_.v1FirmwareVersion);
                modeObservation_.available = modeObservation_.value != 0;
            }

            ++displayVolumeObservation_.revision;
            displayVolumeObservation_.sequence = sequence;
            displayVolumeObservation_.ingressSequence = ingressSequence;
            displayVolumeObservation_.main = static_cast<uint8_t>((payload[7] >> 4) & 0x0F);
            displayVolumeObservation_.muted = static_cast<uint8_t>(payload[7] & 0x0F);
            displayVolumeObservation_.available =
                displayVolumeObservation_.main <= 9 && displayVolumeObservation_.muted <= 9;
            if (displayVolumeObservation_.available) {
                displayState_.mainVolume = displayVolumeObservation_.main;
                displayState_.muteVolume = displayVolumeObservation_.muted;
                displayState_.hasVolumeData = true;
            }
        }
        if (hadAlerts != hasAlerts()) {
            ++alertLifetime_;
        }
        return parsed;
    }
    case PACKET_ID_ALERT_DATA: {
        // Every RespAlertData row, including count-zero clear rows, carries the
        // seven-byte table shape defined by ESP 3.016. Priority, frequency and
        // direction must never be sourced from a bad checksum or wrong bus
        // destination.
        if (!V1PacketFraming::hasCanonicalResponseEvidenceForDestination(data, length, 7, 0xD8)) {
            return false;
        }
        const bool hadAlerts = hasAlerts();
        const uint32_t alertNowMs = hasNowMs ? nowMs : static_cast<uint32_t>(millis());
        const bool parsed = parseAlertData(payload, payloadLen, alertNowMs);
        if (hadAlerts != hasAlerts()) {
            ++alertLifetime_;
        }
        return parsed;
    }

    // Outbound command IDs may be echoed on the shared bus. They are requests,
    // not V1 acknowledgements, and never prove that a mutation was applied.
    case PACKET_ID_WRITE_USER_BYTES: // 0x13 - setUserBytes request/echo
        return true;
    case PACKET_ID_TURN_OFF_DISPLAY: // 0x32 - outbound request/echo, never evidence
    case PACKET_ID_TURN_ON_DISPLAY:  // 0x33 - outbound request/echo, never evidence
        return true;
    case PACKET_ID_MUTE_ON:          // 0x34 - mute-on request/echo
    case PACKET_ID_MUTE_OFF:         // 0x35 - mute-off request/echo
    case 0x36:                       // 0x36 - reqChangeMode request/echo
    case PACKET_ID_REQ_WRITE_VOLUME: // 0x39 - reqWriteVolume request/echo
    case PACKET_ID_RESP_USER_BYTES:  // 0x12 - User bytes response
        return true;                 // Acknowledged, no further processing needed

    case PACKET_ID_RESP_VERSION: { // 0x02 - respVersion (reply from V1)
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
        // ESP originator EAh carries seven ASCII version bytes plus checksum
        // (PL=8); no-checksum E9h canonically carries just those seven bytes
        // (PL=7). Other origins cannot qualify persisted V1 capabilities.
        if (!payload ||
            !V1PacketFraming::hasCanonicalResponseEvidenceForDestination(data, length, 7, 0xD6)) {
            return false;
        }
        const uint8_t letter = payload[0];
        const bool letterAlphabetic = (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z');
        if (!letterAlphabetic || !isAsciiDigit(payload[1]) || payload[2] != '.' || !isAsciiDigit(payload[3]) ||
            !isAsciiDigit(payload[4]) || !isAsciiDigit(payload[5]) || !isAsciiDigit(payload[6])) {
            return false;
        }
        char major = static_cast<char>(payload[1]);
        char minor = static_cast<char>(payload[3]);
        char rev1 = static_cast<char>(payload[4]);
        char rev2 = static_cast<char>(payload[5]);
        char ctrl = static_cast<char>(payload[6]);

        uint32_t version = static_cast<uint32_t>(major - '0') * 10000u +
                           static_cast<uint32_t>(minor - '0') * 1000u +
                           static_cast<uint32_t>(rev1 - '0') * 100u + static_cast<uint32_t>(rev2 - '0') * 10u +
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
                !displayState_.hasV1Version || displayState_.v1FirmwareVersion != version;
            displayState_.v1FirmwareVersion = version;
            displayState_.hasV1Version = true;
            if (versionChanged) {
                Serial.printf("[PacketParser] V1 firmware version: %c.%c%c%c%c (v%lu)\n", major, minor, rev1, rev2,
                              ctrl, version);
            }
        }
        return true;
    }
    case PACKET_ID_RESP_ALL_VOLUME: { // 0x3D - respAllVolume
        // Per Valentine AndroidESPLibrary2
        // ResponseAllVolume.java the payload is exactly 4 bytes:
        //   [0] = current main volume   (0..9)
        //   [1] = current muted volume  (0..9)
        //   [2] = saved main volume     (0..9)
        //   [3] = saved muted volume    (0..9)
        // This is the authoritative source — overwrite the aux2-derived
        // mainVolume/muteVolume values from display packets when present.
        // ESP originator EAh uses PL=5 (four values plus checksum), while
        // no-checksum E9h uses PL=4. Origin-qualified width validation keeps a
        // checksum byte from becoming persisted saved-muted volume.
        if (!payload ||
            !V1PacketFraming::hasCanonicalResponseEvidenceForDestination(data, length, 4, 0xD6)) {
            return false;
        }
        // These are full-byte values, not packed nibbles. Reject the complete
        // response before mutating state so malformed wire data cannot be
        // normalized into a truthful-looking 0..9 snapshot.
        if (payload[0] > 9 || payload[1] > 9 || payload[2] > 9 || payload[3] > 9) return false;
        displayState_.mainVolume = payload[0];
        displayState_.muteVolume = payload[1];
        displayState_.savedMainVolume = payload[2];
        displayState_.savedMuteVolume = payload[3];
        displayState_.hasVolumeData = true;
        displayState_.hasSavedVolume = true;
        ++allVolumeObservation_.revision;
        allVolumeObservation_.sequence = ++settingsObservationSequence_;
        allVolumeObservation_.ingressSequence = ingressSequence;
        allVolumeObservation_.available = true;
        allVolumeObservation_.currentMain = payload[0];
        allVolumeObservation_.currentMuted = payload[1];
        allVolumeObservation_.savedMain = payload[2];
        allVolumeObservation_.savedMuted = payload[3];
        return true;
    }
    case PACKET_ID_RESP_CURRENT_VOLUME: { // 0x38 - respCurrentVolume
        // ESP Specification 3.016 / VR ResponseCurrentVolume: [main, muted].
        // The focused read exists before respAllVolume and is the strongest
        // verification source for a temporary write.
        if (!payload ||
            !V1PacketFraming::hasCanonicalResponseEvidenceForDestination(data, length, 2, 0xD6)) {
            return false;
        }
        if (payload[0] > 9 || payload[1] > 9) return false;
        displayState_.mainVolume = payload[0];
        displayState_.muteVolume = payload[1];
        displayState_.hasVolumeData = true;
        ++currentVolumeObservation_.revision;
        currentVolumeObservation_.sequence = ++settingsObservationSequence_;
        currentVolumeObservation_.ingressSequence = ingressSequence;
        currentVolumeObservation_.available = true;
        currentVolumeObservation_.main = payload[0];
        currentVolumeObservation_.muted = payload[1];
        return true;
    }
    case PACKET_ID_REQ_CURRENT_VOLUME: // 0x37 - outbound request/echo
        return true;
    case PACKET_ID_REQ_ALL_VOLUME: // 0x3C - outbound request, ignore echoes
        return true;

    case PACKET_ID_RESP_REQUEST_NOT_PROCESSED:
        // The payload is the exact request ID the V1 did not process.
        return payload &&
               V1PacketFraming::hasCanonicalResponseEvidenceForDestination(data, length, 1, 0xD6);

    case PACKET_ID_INF_V1_BUSY: {
        // ESP 3.016 p40: one to five pending request IDs, always addressed
        // to General Broadcast (D8), unlike targeted request rejection.
        // Each width must be origin/checksum/destination qualified before it
        // can suppress any writer.
        for (size_t count = 1; count <= 5; ++count) {
            if (V1PacketFraming::hasCanonicalResponseEvidenceForDestination(data, length, count, 0xD8)) {
                return true;
            }
        }
        return false;
    }

    case PACKET_ID_RESP_MAX_SWEEP_INDEX: {
        if (!payload || !V1PacketFraming::hasCanonicalResponseEvidenceForDestination(data, length, 1, 0xD6)) {
            return false;
        }
        const auto poisonSweepMax = [this, ingressSequence]() {
            ++sweepMaxObservation_.revision;
            sweepMaxObservation_.sequence = ++settingsObservationSequence_;
            sweepMaxObservation_.ingressSequence = ingressSequence;
            sweepMaxObservation_.available = false;
            sweepMaxObservation_.poisoned = true;
        };
        if (sweepMaxObservation_.poisoned) return false;
        if (payload[0] > 0x3F ||
            (sweepMaxObservation_.available && sweepMaxObservation_.maxIndex != payload[0])) {
            poisonSweepMax();
            return false;
        }
        const uint64_t allowed = payload[0] == 63 ? UINT64_MAX : ((uint64_t{1} << (payload[0] + 1u)) - 1u);
        if ((sweepDefinitionsObservation_.presentMask & ~allowed) != 0) {
            sweepDefinitionsObservation_.poisoned = true;
        }
        ++sweepMaxObservation_.revision;
        sweepMaxObservation_.sequence = ++settingsObservationSequence_;
        sweepMaxObservation_.ingressSequence = ingressSequence;
        sweepMaxObservation_.available = true;
        sweepMaxObservation_.maxIndex = payload[0];
        return true;
    }
    case PACKET_ID_RESP_SWEEP_SECTIONS: {
        if (sweepSectionsObservation_.poisoned) return false;
        const auto poisonSweepSections = [this]() {
            sweepSectionsObservation_.poisoned = true;
            sweepSectionsObservation_.available = false;
            sweepSectionsObservation_.complete = false;
        };
        size_t dataBytes = 0;
        if (V1PacketFraming::hasCanonicalResponseEvidenceForDestination(data, length, 5, 0xD6)) dataBytes = 5;
        else if (V1PacketFraming::hasCanonicalResponseEvidenceForDestination(data, length, 10, 0xD6)) dataBytes = 10;
        else if (V1PacketFraming::hasCanonicalResponseEvidenceForDestination(data, length, 15, 0xD6)) dataBytes = 15;
        else return false;
        const uint8_t declaredCount = payload[0] & 0x0F;
        // ESP 3.016 encodes the section number in the upper nibble as a
        // one-based wire value (1..count). A (0,0) vendor API sentinel is not
        // a usable Gen2 topology and must not authorize capture or Apply.
        if (declaredCount == 0 || declaredCount > 15) {
            poisonSweepSections();
            return false;
        }
        V1SweepSectionsObservation candidate = sweepSectionsObservation_;
        if (candidate.count != 0 && candidate.count != declaredCount) {
            poisonSweepSections();
            return false;
        }
        candidate.count = declaredCount;
        for (size_t offset = 0; offset < dataBytes; offset += 5) {
            const uint8_t indexCount = payload[offset];
            const uint8_t wireIndex = static_cast<uint8_t>((indexCount >> 4) & 0x0F);
            if ((indexCount & 0x0F) != declaredCount || wireIndex == 0 || wireIndex > declaredCount) {
                poisonSweepSections();
                return false;
            }
            const uint8_t index = static_cast<uint8_t>(wireIndex - 1u);
            const uint16_t upper = static_cast<uint16_t>((payload[offset + 1] << 8) | payload[offset + 2]);
            const uint16_t lower = static_cast<uint16_t>((payload[offset + 3] << 8) | payload[offset + 4]);
            const bool unused = lower == 0 && upper == 0;
            if ((!unused && lower >= upper) || (lower == 0) != (upper == 0)) {
                poisonSweepSections();
                return false;
            }
            // One canonical response contributes each wire section exactly
            // once. Accepting even an identical duplicate would make a
            // missing peer indistinguishable from complete evidence.
            if ((candidate.presentMask & (1u << index)) != 0) {
                poisonSweepSections();
                return false;
            }
            candidate.sections[index] = V1SweepSectionObservation{index, declaredCount, lower, upper};
            candidate.presentMask = static_cast<uint16_t>(candidate.presentMask | (1u << index));
        }
        ++candidate.revision;
        candidate.sequence = ++settingsObservationSequence_;
        candidate.ingressSequence = ingressSequence;
        candidate.complete = candidate.presentMask == static_cast<uint16_t>((1u << declaredCount) - 1u);
        candidate.available = true;
        sweepSectionsObservation_ = candidate;
        return true;
    }
    case PACKET_ID_RESP_SWEEP_DEFINITION: {
        if (sweepDefinitionsObservation_.poisoned) return false;
        if (!payload || !V1PacketFraming::hasCanonicalResponseEvidenceForDestination(data, length, 5, 0xD6)) {
            return false;
        }
        // ESP 3.016 defines bits 0..5 as the zero-based definition index and
        // its canonical responses set reserved bit 7 (0x80..0xBF). The vendor
        // libraries mask bit 7, so tolerate either value while requiring the
        // unsupported bit 6 to remain clear.
        if ((payload[0] & 0x40u) != 0) {
            sweepDefinitionsObservation_.poisoned = true;
            return false;
        }
        const uint8_t index = static_cast<uint8_t>(payload[0] & 0x3Fu);
        if (sweepMaxObservation_.available && index > sweepMaxObservation_.maxIndex) {
            sweepDefinitionsObservation_.poisoned = true;
            return false;
        }
        const uint16_t upper = static_cast<uint16_t>((payload[1] << 8) | payload[2]);
        const uint16_t lower = static_cast<uint16_t>((payload[3] << 8) | payload[4]);
        const bool unused = lower == 0 && upper == 0;
        if ((!unused && lower >= upper) || (lower == 0) != (upper == 0)) {
            // Once framing, checksum, destination, and selector all identify a
            // modeled definition response, an impossible range is corrupt
            // transaction evidence rather than ignorable BLE noise.
            sweepDefinitionsObservation_.poisoned = true;
            return false;
        }
        if ((sweepDefinitionsObservation_.presentMask & (uint64_t{1} << index)) != 0 &&
            (sweepDefinitionsObservation_.definitions[index].lowerMHz != lower ||
             sweepDefinitionsObservation_.definitions[index].upperMHz != upper)) {
            sweepDefinitionsObservation_.poisoned = true;
            return false;
        }
        sweepDefinitionsObservation_.definitions[index] = V1SweepDefinitionObservation{index, lower, upper};
        sweepDefinitionsObservation_.ingressSequences[index] = ingressSequence;
        sweepDefinitionsObservation_.presentMask |= (uint64_t{1} << index);
        ++sweepDefinitionsObservation_.revision;
        sweepDefinitionsObservation_.sequence = ++settingsObservationSequence_;
        sweepDefinitionsObservation_.ingressSequence = ingressSequence;
        return true;
    }
    case PACKET_ID_RESP_SWEEP_WRITE_RESULT: {
        if (!payload || !V1PacketFraming::hasCanonicalResponseEvidenceForDestination(data, length, 1, 0xD6)) {
            return false;
        }
        // Zero is success. Nonzero is the first invalid zero-based definition
        // index plus one, so 64 is the largest representable failure.
        if (payload[0] > 64) return false;
        ++sweepWriteResultObservation_.revision;
        sweepWriteResultObservation_.sequence = ++settingsObservationSequence_;
        sweepWriteResultObservation_.ingressSequence = ingressSequence;
        sweepWriteResultObservation_.available = true;
        sweepWriteResultObservation_.result = payload[0];
        return true;
    }

    case PACKET_ID_FACTORY_DEFAULT:
    case PACKET_ID_WRITE_SWEEP_DEFINITION:
    case PACKET_ID_REQ_ALL_SWEEP_DEFINITIONS:
    case PACKET_ID_REQ_MAX_SWEEP_INDEX:
    case PACKET_ID_REQ_SWEEP_SECTIONS:
        return true; // outbound requests/echoes are never proof

    case PACKET_ID_VERSION: // 0x01 - reqVersion
        // 0x01 is the OUTBOUND request id. The V1 should never send 0x01
        // back to us, but if a buggy peer or replay loop produces one we
        // ignore it silently rather than parsing it as a version reply.
        return true;

    default:
        // Unknown packet - silently ignore in hot path
        return false;
    }
}

void PacketParser::resetSweepSectionsObservation() {
    sweepSectionsObservation_ = V1SweepSectionsObservation{};
}

void PacketParser::resetSweepMaxObservation() {
    sweepMaxObservation_ = V1SweepMaxObservation{};
}

void PacketParser::resetSweepDefinitionsObservation() {
    sweepDefinitionsObservation_ = V1SweepDefinitionsObservation{};
}

void PacketParser::resetSweepWriteResultObservation() {
    sweepWriteResultObservation_ = V1SweepWriteResultObservation{};
}

bool PacketParser::copyLatestCanonicalCurrentVolume(uint8_t& main, uint8_t& muted,
                                                    uint32_t* ingressSequence) const {
    uint32_t latestSequence = 0;
    bool found = false;
    if (displayVolumeObservation_.available && displayVolumeObservation_.sequence >= latestSequence) {
        latestSequence = displayVolumeObservation_.sequence;
        main = displayVolumeObservation_.main;
        muted = displayVolumeObservation_.muted;
        if (ingressSequence) *ingressSequence = displayVolumeObservation_.ingressSequence;
        found = true;
    }
    if (allVolumeObservation_.available && allVolumeObservation_.sequence >= latestSequence) {
        latestSequence = allVolumeObservation_.sequence;
        main = allVolumeObservation_.currentMain;
        muted = allVolumeObservation_.currentMuted;
        if (ingressSequence) *ingressSequence = allVolumeObservation_.ingressSequence;
        found = true;
    }
    if (currentVolumeObservation_.available && currentVolumeObservation_.sequence >= latestSequence) {
        main = currentVolumeObservation_.main;
        muted = currentVolumeObservation_.muted;
        if (ingressSequence) *ingressSequence = currentVolumeObservation_.ingressSequence;
        found = true;
    }
    return found;
}

bool PacketParser::validatePacket(const uint8_t* data, size_t length) {
    if (length < 8) {
        return false;
    }
    if (data[0] != ESP_PACKET_START || data[length - 1] != ESP_PACKET_END) {
        return false;
    }
    // Packet-specific canonical width, origin, destination and checksum checks
    // are performed by the owning response cases. BLE long-packet chunks are
    // reassembled before this parser is called.
    return true;
}

bool PacketParser::parseDisplayData(const uint8_t* payload, size_t length) {
    // Expected payload >= 8 bytes (matches v1g2-t4s3 parsing window)
    if (!payload || length < 8) {
        return false;
    }

    // Display packet structure. Parser behavior is pinned by test_packet_parser
    // and the independent vectors in test/fixtures/protocol_spec_tables.h.
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

    // Snapshot auxData0 status bits:
    //   bit 0 (0x01) — isSoft         : audio mute (spec-true)
    //   bit 1 (0x02) — isTSHoldOff    : V1-bound writes are forbidden
    //   bit 2 (0x04) — isSystemStatus : V1 actively searching for alerts
    // softMuted is exposed as its own field; systemStatus gates the band /
    // arrow indicators below.
    //
    // bit 3 (0x08) — isDisplayOn — is recorded for settings snapshots and
    // command verification. No renderer uses displayOn as a blanking gate: a
    // live alert must remain visible on V1Simple while the V1 is in dark mode.
    bool auxSystemStatus = false;
    if (length > 5) {
        const uint8_t aux0 = payload[5];
        displayState_.softMuted = (aux0 & 0x01) != 0;
        displayState_.timeSliceHoldoff = (aux0 & 0x02) != 0;
        auxSystemStatus = (aux0 & 0x04) != 0;
        displayState_.displayOn = (aux0 & 0x08) != 0;
        displayState_.hasDisplayOn = true;

        const V1FirmwareCompat::Capabilities capabilities =
            V1FirmwareCompat::capabilities(displayState_.v1FirmwareVersion);
        displayState_.hasDisplayActive = capabilities.displayActive;
        displayState_.displayActive = capabilities.displayActive && (aux0 & 0x80) != 0;

        const uint8_t aux1 = payload[6];
        displayState_.hasLogicMuted = capabilities.logicMuted;
        displayState_.logicMuted = capabilities.logicMuted && (aux1 & 0x02) != 0;
        displayState_.hasAutoMuted = capabilities.autoMute;
        displayState_.autoMuted = capabilities.autoMute && (aux1 & 0x10) != 0;
        displayState_.hasDoubleTapActive = capabilities.doubleTap;
        displayState_.doubleTapActive = capabilities.doubleTap && (aux1 & 0x20) != 0;
    } else {
        displayState_.softMuted = false;
        displayState_.timeSliceHoldoff = true;
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
    if (arrow.laser)
        displayState_.activeBands |= BAND_LASER;
    if (arrow.ka)
        displayState_.activeBands |= BAND_KA;
    if (arrow.k)
        displayState_.activeBands |= BAND_K;
    if (arrow.x)
        displayState_.activeBands |= BAND_X;

    displayState_.arrows = DIR_NONE;
    if (arrow.front)
        displayState_.arrows = static_cast<Direction>(displayState_.arrows | DIR_FRONT);
    if (arrow.side)
        displayState_.arrows = static_cast<Direction>(displayState_.arrows | DIR_SIDE);
    if (arrow.rear)
        displayState_.arrows = static_cast<Direction>(displayState_.arrows | DIR_REAR);

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
    //
    // This debounce delays the MUTED render by one packet, so it errs toward
    // showing the alert at full urgency. Do not replace it with a raw mirror
    // without hardware evidence that the transients are gone.
    const bool rawMuteBit = (image1 & 0x10) != 0;
    if (rawMuteBit) {
        if (displayMuteConfirmCount_ < 2) {
            ++displayMuteConfirmCount_;
        }
    } else {
        displayMuteConfirmCount_ = 0;
    }
    displayState_.muted = (displayMuteConfirmCount_ >= 2);

    // Volume-dependent behavior is committed by parseInternal only after the
    // complete display frame has canonical D8/E9-or-EA framing, width and (for
    // EA) checksum evidence. Keep tolerant display parsing limited to visual
    // state so a damaged or fixture-only frame cannot become a control baseline.

    // V1 sends LED bar state directly in the display packet at payload[2].
    // This is the authoritative signal strength from V1's own display. It is
    // decoded per ESP Specification 3.015 Table 9.1 into an LED count of 0..8
    // and carried at protocol scale; V1Simple's six-slot display ceiling is
    // applied by the renderer, not here. See decodeLEDBitmap() below.
    if (length > 2) {
        uint8_t ledBitmap = payload[2];
        displayState_.signalBars = decodeLEDBitmap(ledBitmap);
    }

    // AndroidESPLibrary2 treats display aux0 as status flags (soft mute/system/euro/display active),
    // not as a direct alert-table priority index. Priority selection is resolved from alert rows.

    return true;
}

// Decode the Signal Strength Bar Graph Image byte into an LED count.
//
// ESP Specification 3.015 Table 9.1 defines one value per LED count 0..8:
//
//     $00 $01 $03 $07 $0F $1F $3F $7F $FF
//      0   1   2   3   4   5   6   7   8
//
// Preserve that protocol scale here. V1Simple's six-segment meter clamps only
// at the render boundary in drawVerticalSignalBars().
//
// ValentineResearch/AndroidESPLibrary2 InfDisplayData.getNumberOfLEDS() omits
// $00 and defaults unknown bytes to 8. Follow the specification for $00, retain
// that fail-loud default for undefined bytes, and record those substitutions.
namespace {
// Table 9.1 read as a ladder: the index is the LED count, the value is the
// bar graph byte the V1 sends for it.
constexpr uint8_t kLedBitmapDomain[] = {0x00, 0x01, 0x03, 0x07, 0x0F, 0x1F, 0x3F, 0x7F, 0xFF};
// Table 9.1 tops out at eight LEDs; this is also VR's getNumberOfLEDS()
// default-branch return.
constexpr uint8_t kLedBitmapMaxBars = 8;
} // namespace

uint8_t PacketParser::decodeLEDBitmap(uint8_t bitmap) const {
    for (uint8_t index = 0; index < sizeof(kLedBitmapDomain); ++index) {
        if (bitmap == kLedBitmapDomain[index]) {
            // In domain: Table 9.1's LED count is this value's ladder index.
            return index;
        }
    }
    return kLedBitmapMaxBars;
}

void PacketParser::decodeMode(const uint8_t* payload, size_t length) {
    // V1 4.1028+ reports the mode continuously in auxData1 bits 2-3, including
    // while the bogey glyph is occupied by an alert count. Older or
    // version-unknown sessions retain the display-glyph fallback below.
    if (payload && length > 6 && displayState_.hasV1Version &&
        V1FirmwareCompat::capabilities(displayState_.v1FirmwareVersion).modeObservation) {
        const bool euroMode = (payload[5] & 0x10) != 0;
        const bool customSweeps = (payload[5] & 0x20) != 0;
        char qualifiedMode = 0;
        switch ((payload[6] >> 2) & 0x03) {
        case 1:
            qualifiedMode = euroMode ? (customSweeps ? 'C' : 'U') : 'A';
            break;
        case 2:
            qualifiedMode = euroMode ? (customSweeps ? 'c' : 'u') : 'l';
            break;
        case 3:
            if (!euroMode) qualifiedMode = 'L';
            break;
        default:
            break;
        }
        if (qualifiedMode == 0) {
            // Code 3 is invalid in Euro mode, and code 0 is unknown. Do not
            // retain an earlier US glyph as if it described this packet.
            displayState_.modeChar = 0;
            displayState_.hasMode = false;
            return;
        }
        displayState_.modeChar = qualifiedMode;
        displayState_.hasMode = true;
        return;
    }

    // The fallback is recoverable only while the V1 is not using the bogey
    // counter to show an alert count / verdict.
    // The V1 mode is encoded as a 7-segment glyph in the bogey-counter image
    // byte (payload[0]), with the high bit being the decimal point. The accepted
    // glyph table is pinned by test_parse_display_packet_decodes_mode_from_bogey_glyph;
    // original external provenance is UNKNOWN.
    //
    //   0x77 = 'A' All Bogeys Mode (USA)
    //   0x39 = 'C' K + Custom Sweeps (USA)
    //   0x3E = 'U' Euro: Ka and Ka(Photo)
    //   0x18 = 'l' Logic Mode (USA)
    //   0x1C = 'u' Euro: Ka only
    //   0x58 = 'c' Custom Sweeps (lower)
    //   0x38 = 'L' Advanced Logic Mode (USA)
    if (!payload || length < 1) {
        return;
    }

    const uint8_t bogeyImage = payload[0] & 0x7F;
    char mode = 0;
    switch (bogeyImage) {
    case 0x77:
        mode = 'A';
        break;
    case 0x39:
        mode = 'C';
        break;
    case 0x3E:
        mode = 'U';
        break;
    case 0x18:
        mode = 'l';
        break;
    case 0x1C:
        mode = 'u';
        break;
    case 0x58:
        mode = 'c';
        break;
    case 0x38:
        mode = 'L';
        break;
    default:
        return; // Bogey shows a digit/other -> alerting. Preserve the last
                // mode actually observed in this detector session.
    }
    displayState_.modeChar = mode;
    displayState_.hasMode = (mode != 0);
}
