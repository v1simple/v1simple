/**
 * Alert table parsing and priority-selection path for PacketParser.
 */

#include "packet_parser.h"
#include "band_utils.h"
#include <algorithm>

#ifndef UNIT_TEST
#include "perf_metrics.h"
#define PARSER_PERF_INC(counter) PERF_INC(counter)
#define PARSER_TRACE_ENABLED() (perfDebugEnabled)
#else
#define PARSER_PERF_INC(counter) do { } while (0)
#define PARSER_TRACE_ENABLED() (false)
#endif

namespace {
uint16_t combineMSBLSB(uint8_t msb, uint8_t lsb) {
    return (static_cast<uint16_t>(msb) << 8) | lsb;
}

// Freshness guards prevent old partial rows from being reused as "complete" data.
static constexpr uint32_t kAlertRowFreshnessMs = 1500;
static constexpr uint32_t kAlertAssemblyTimeoutMs = 1800;

const char* bandToString(Band band) {
    return bandName(band);
}

} // namespace

void PacketParser::resetAlertAssembly() {
    // Drop any partially collected alert rows without altering display state.
    clearAlertCache();
}

void PacketParser::clearAlertCache() {
    alertChunkPresent_.fill(false);
    alertChunkCountTag_.fill(0);
    alertChunkRxMs_.fill(0);
    alertTableFirstSeenMs_.fill(0);
}

void PacketParser::clearAlertCacheForCount(uint8_t count) {
    if (count == 0 || count > MAX_ALERTS) {
        return;
    }
    for (size_t i = 0; i < RAW_ALERT_INDEX_SLOTS; ++i) {
        if (alertChunkPresent_[i] && alertChunkCountTag_[i] == count) {
            alertChunkPresent_[i] = false;
            alertChunkCountTag_[i] = 0;
            alertChunkRxMs_[i] = 0;
        }
    }
    alertTableFirstSeenMs_[count] = 0;
}

Band PacketParser::decodeBand(uint8_t bandArrow) const {
    // V1 protocol summary: docs/V1_PROTOCOL_REFERENCES.md#alert-rows.
    // The band value lives in the low 5 bits of the bandArrow byte and is a
    // raw integer (not a bitmask of multiple bands). Recognised values:
    // 0x01=Laser, 0x02=Ka, 0x04=K, 0x08=X, 0x10=Ku.  We preserve the
    // historical bit-test order for known-good Laser/Ka/K/X handling, then
    // explicitly check 0x10 for Ku so it no longer falls through to BAND_NONE.
    if (bandArrow & 0b00000001) return BAND_LASER;
    if (bandArrow & 0b00000010) return BAND_KA;
    if (bandArrow & 0b00000100) return BAND_K;
    if (bandArrow & 0b00001000) return BAND_X;
    if ((bandArrow & 0x1F) == 0x10) return BAND_KU;
    return BAND_NONE;
}

Direction PacketParser::decodeDirection(uint8_t bandArrow) const {
    if (bandArrow & 0b00100000) return DIR_FRONT;
    if (bandArrow & 0b01000000) return DIR_SIDE;
    if (bandArrow & 0b10000000) return DIR_REAR;
    return DIR_NONE;
}

uint8_t PacketParser::mapStrengthToBars(Band band, uint8_t raw) const {
    // Convert raw RSSI to a 0..6 bar count for per-alert direction strength
    // used by secondary alert context and alert history. Primary
    // simple signal bars do NOT use this path; they mirror the V1's
    // InfDisplayData LED bitmap in DisplayState::signalBars.
    // V1 protocol summary: docs/V1_PROTOCOL_REFERENCES.md#alert-rows.
    // The Valentine bargraph thresholds produce 0..8 bars; we keep those
    // thresholds here, then proportionally compress 0..8 → 0..6 with round-half-up:
    // out = (vrBars * 6 + 4) / 8.
    //   VR 0 -> 0   VR 1 -> 1   VR 2 -> 2   VR 3 -> 2
    //   VR 4 -> 3   VR 5 -> 4   VR 6 -> 5   VR 7 -> 5   VR 8 -> 6
    auto vrBars = [band, raw]() -> uint8_t {
        switch (band) {
            case BAND_LASER:
                // VR returns 8 bars for any laser alert, including Invalid.
                return 8;
            case BAND_KA:
                if (raw >= 0xBA) return 8;
                if (raw >= 0xB3) return 7;
                if (raw >= 0xAC) return 6;
                if (raw >= 0xA5) return 5;
                if (raw >= 0x9E) return 4;
                if (raw >= 0x97) return 3;
                if (raw >= 0x90) return 2;
                if (raw >= 0x01) return 1;
                return 0;
            case BAND_X:
                if (raw >= 0xD0) return 8;
                if (raw >= 0xC5) return 7;
                if (raw >= 0xBD) return 6;
                if (raw >= 0xB4) return 5;
                if (raw >= 0xAA) return 4;
                if (raw >= 0xA0) return 3;
                if (raw >= 0x96) return 2;
                if (raw >= 0x01) return 1;
                return 0;
            case BAND_K:
            case BAND_KU:
                // VR groups K, Ku, and Photo onto the same K-band scale.
                // (Items 4 and a portion of the Ku/Photo handling depend on
                // this branch also accepting Ku/Photo bands once item 4 wires
                // them through decodeBand.)
                if (raw >= 0xC2) return 8;
                if (raw >= 0xB8) return 7;
                if (raw >= 0xAE) return 6;
                if (raw >= 0xA4) return 5;
                if (raw >= 0x9A) return 4;
                if (raw >= 0x90) return 3;
                if (raw >= 0x88) return 2;
                if (raw >= 0x01) return 1;
                return 0;
            default:
                return 0;
        }
    }();

    return static_cast<uint8_t>((static_cast<uint16_t>(vrBars) * 6u + 4u) / 8u);
}

bool PacketParser::parseAlertData(const uint8_t* payload, size_t length, uint32_t nowMs) {
    if (!payload || length < 1) {
        return false;
    }

    // Byte0: high nibble = alert index, low nibble = alert count.
    // VR-style streams can be seen as one-based (1..count) or zero-based
    // (0..count-1). Keep rows by raw index and publish when either complete set
    // is present to avoid mode-locking on partial tables.
    uint8_t alertIndex = (payload[0] >> 4) & 0x0F;
    uint8_t receivedAlertCount = payload[0] & 0x0F;
    if (receivedAlertCount == 0) {
        alertCount_ = 0;
        clearAlertCache();
        // DON'T clear signalBars here - parseDisplayData reads V1's LED bitmap.
        displayState_.arrows = DIR_NONE;
        // DON'T clear muted here — mute state is authoritative from
        // parseDisplayData() (InfDisplayData soft-mute bit + volume==0).
        // Clearing it here races the display packet path and causes
        // mute indicator flash on every count=0 alert packet.
        displayState_.hasJunkAlert = false;
        displayState_.hasPhotoAlert = false;
        displayState_.hasKuAlert = false;
        return true;
    }

    // Each alert row is 7-8 bytes in V1G2 captures; require at least 7.
    if (length < 7) {
        return false;
    }

    if (receivedAlertCount > MAX_ALERTS) {
        // Invalid table size - drop and wait for next valid row.
        return false;
    }

    // Drop stale cached rows so a missing row from a prior cycle cannot be
    // reused to fake a "complete" table later.
    for (size_t i = 0; i < RAW_ALERT_INDEX_SLOTS; ++i) {
        if (!alertChunkPresent_[i]) {
            continue;
        }
        if ((nowMs - alertChunkRxMs_[i]) > kAlertRowFreshnessMs) {
            const uint8_t staleCount = alertChunkCountTag_[i];
            alertChunkPresent_[i] = false;
            alertChunkCountTag_[i] = 0;
            alertChunkRxMs_[i] = 0;
            if (staleCount > 0 && staleCount <= MAX_ALERTS) {
                bool anyRowsForCount = false;
                for (size_t j = 0; j < RAW_ALERT_INDEX_SLOTS; ++j) {
                    if (alertChunkPresent_[j] && alertChunkCountTag_[j] == staleCount) {
                        anyRowsForCount = true;
                        break;
                    }
                }
                if (!anyRowsForCount) {
                    alertTableFirstSeenMs_[staleCount] = 0;
                }
            }
        }
    }

    // The canonical Valentine ESP library uses strictly 1-based alert indices.
    // V1 protocol summary: docs/V1_PROTOCOL_REFERENCES.md#alert-rows.
    // We deliberately accept BOTH 0-based and 1-based here for robustness —
    // some older firmware revisions and
    // third-party V1 emulators emit 0-based rows. The dual-mode auto-detection
    // logic immediately below (countRowsForMode) decides which scheme is
    // actually in use for the current count tag, so misclassification is
    // self-healing rather than fatal. This divergence is intentional; do not
    // tighten to 1-based without also updating
    // test_alert_stream_zero_based_index_is_supported and
    // test_alert_stream_ambiguous_first_row_does_not_lock_wrong_mode.
    const bool indexValidOneBased = (alertIndex >= 1 && alertIndex <= receivedAlertCount);
    const bool indexValidZeroBased = (alertIndex < receivedAlertCount);
    if (!indexValidOneBased && !indexValidZeroBased) {
        if (PARSER_TRACE_ENABLED()) {
            Serial.printf("[AlertAsm] drop idx=%u cnt=%u (invalid raw index)\n",
                          static_cast<unsigned>(alertIndex),
                          static_cast<unsigned>(receivedAlertCount));
        }
        return true;
    }
    const size_t rawSlot = static_cast<size_t>(alertIndex);

    bool hadRowsForCount = false;
    for (size_t i = 0; i < RAW_ALERT_INDEX_SLOTS; ++i) {
        if (alertChunkPresent_[i] && alertChunkCountTag_[i] == receivedAlertCount) {
            hadRowsForCount = true;
            break;
        }
    }

    const bool replacingRow =
        alertChunkPresent_[rawSlot] && (alertChunkCountTag_[rawSlot] == receivedAlertCount);
    if (replacingRow) {
        PARSER_PERF_INC(alertTableRowReplacements);
    }
    alertChunkPresent_[rawSlot] = true;
    alertChunkCountTag_[rawSlot] = receivedAlertCount;
    alertChunkRxMs_[rawSlot] = nowMs;
    if (!hadRowsForCount || alertTableFirstSeenMs_[receivedAlertCount] == 0) {
        alertTableFirstSeenMs_[receivedAlertCount] = nowMs;
    }
    std::array<uint8_t, 8>& chunk = alertChunks_[rawSlot];
    chunk.fill(0);
    size_t copyLen = std::min<size_t>(8, length);
    for (size_t i = 0; i < copyLen; ++i) {
        chunk[i] = payload[i];
    }

    auto countRowsForMode = [&](bool zeroBased) -> size_t {
        size_t rows = 0;
        for (uint8_t logicalIdx = 0; logicalIdx < receivedAlertCount; ++logicalIdx) {
            const size_t expectedRawIndex = zeroBased
                ? static_cast<size_t>(logicalIdx)
                : static_cast<size_t>(logicalIdx + 1);
            if (expectedRawIndex >= RAW_ALERT_INDEX_SLOTS) {
                continue;
            }
            if (alertChunkPresent_[expectedRawIndex] &&
                alertChunkCountTag_[expectedRawIndex] == receivedAlertCount &&
                (nowMs - alertChunkRxMs_[expectedRawIndex]) <= kAlertRowFreshnessMs) {
                ++rows;
            }
        }
        return rows;
    };

    const size_t rowsZeroBased = countRowsForMode(true);
    const size_t rowsOneBased = countRowsForMode(false);
    const bool completeZeroBased = (rowsZeroBased == receivedAlertCount);
    const bool completeOneBased = (rowsOneBased == receivedAlertCount);
    const size_t rowsForCount = std::max(rowsZeroBased, rowsOneBased);

    if (PARSER_TRACE_ENABLED()) {
        Serial.printf(
            "[AlertRow] idx=%u cnt=%u rawSlot=%u rows0=%u rows1=%u repl=%u raw0=0x%02X f=%u bandArrow=0x%02X aux0=0x%02X\n",
            static_cast<unsigned>(alertIndex),
            static_cast<unsigned>(receivedAlertCount),
            static_cast<unsigned>(rawSlot),
            static_cast<unsigned>(rowsZeroBased),
            static_cast<unsigned>(rowsOneBased),
            replacingRow ? 1u : 0u,
            static_cast<unsigned>(payload[0]),
            static_cast<unsigned>(combineMSBLSB(payload[1], payload[2])),
            static_cast<unsigned>(payload[5]),
            static_cast<unsigned>(payload[6]));
    }

    // Rolling raw-index cache: only publish when at least one full scheme is ready.
    if (!completeZeroBased && !completeOneBased) {
        if ((alertTableFirstSeenMs_[receivedAlertCount] != 0) &&
            ((nowMs - alertTableFirstSeenMs_[receivedAlertCount]) > kAlertAssemblyTimeoutMs)) {
            PARSER_PERF_INC(alertTableAssemblyTimeouts);
            if (PARSER_TRACE_ENABLED()) {
                Serial.printf("[AlertAsm] timeout cnt=%u rows=%u/%u ageMs=%lu\n",
                              static_cast<unsigned>(receivedAlertCount),
                              static_cast<unsigned>(rowsForCount),
                              static_cast<unsigned>(receivedAlertCount),
                              static_cast<unsigned long>(
                                  nowMs - alertTableFirstSeenMs_[receivedAlertCount]));
            }
            clearAlertCacheForCount(receivedAlertCount);
            return true;
        }
        if (PARSER_TRACE_ENABLED()) {
            Serial.printf("[AlertAsm] partial rows=%u/%u (rows0=%u rows1=%u)\n",
                          static_cast<unsigned>(rowsForCount),
                          static_cast<unsigned>(receivedAlertCount),
                          static_cast<unsigned>(rowsZeroBased),
                          static_cast<unsigned>(rowsOneBased));
        }
        return true;
    }

    enum class AlertIndexMode : uint8_t {
        ZeroBased = 0,
        OneBased = 1,
    };
    AlertIndexMode decodeMode = completeZeroBased ? AlertIndexMode::ZeroBased : AlertIndexMode::OneBased;
    if (completeZeroBased && completeOneBased) {
        PARSER_PERF_INC(prioritySelectAmbiguousIndex);
    }

    if (PARSER_TRACE_ENABLED()) {
        const char* mode = (decodeMode == AlertIndexMode::ZeroBased) ? "zero" : "one";
        Serial.printf("[AlertAsm] publish mode=%s cnt=%u rows0=%u rows1=%u\n",
                      mode,
                      static_cast<unsigned>(receivedAlertCount),
                      static_cast<unsigned>(rowsZeroBased),
                      static_cast<unsigned>(rowsOneBased));
    }

    std::array<AlertData, MAX_ALERTS> nextAlerts{};
    size_t nextAlertCount = 0;
    bool anyJunk = false;
    bool anyPhoto = false;
    bool anyKu = false;

    // We have enough rows; validate all required row slots for this count and
    // decode into a new table buffer before publishing.
    for (size_t i = 0; i < receivedAlertCount && i < MAX_ALERTS; ++i) {
        const size_t expectedRawIndex = (decodeMode == AlertIndexMode::ZeroBased) ? i : (i + 1);
        const bool rowFresh =
            (expectedRawIndex < RAW_ALERT_INDEX_SLOTS) &&
            alertChunkPresent_[expectedRawIndex] &&
            alertChunkCountTag_[expectedRawIndex] == receivedAlertCount &&
            ((nowMs - alertChunkRxMs_[expectedRawIndex]) <= kAlertRowFreshnessMs);
        if (!rowFresh) {
            if (PARSER_TRACE_ENABLED()) {
                Serial.printf("[AlertAsm] missing row rawIdx=%u cnt=%u rows=%u ageMs=%lu\n",
                              static_cast<unsigned>(expectedRawIndex),
                              static_cast<unsigned>(receivedAlertCount),
                              static_cast<unsigned>(rowsForCount),
                              static_cast<unsigned long>(
                                  (expectedRawIndex < RAW_ALERT_INDEX_SLOTS &&
                                   alertChunkPresent_[expectedRawIndex])
                                      ? (nowMs - alertChunkRxMs_[expectedRawIndex])
                                      : 0));
            }
            return true;
        }

        const auto& a = alertChunks_[expectedRawIndex];
        uint8_t bandArrow = a[5];  // band + arrow bits (low-5 contains raw band encoding)
        uint8_t aux0 = a[6];       // aux0: bit7=priority, bit6=junk, low nibble=photo type
        const uint8_t rawBandBits = static_cast<uint8_t>(bandArrow & 0x1F);
        const bool isKu = (rawBandBits == 0x10);

        Band band = decodeBand(bandArrow);
        if (isKu) {
            PARSER_PERF_INC(parserRowsKuRaw);
        }
        if (band == BAND_NONE) {
            PARSER_PERF_INC(parserRowsBandNone);
        }
        Direction dir = decodeDirection(bandArrow);
        bool isPriority = (aux0 & 0x80) != 0;  // (aux0 & 128) != 0
        // Match official Android/iOS library behavior:
        // junk flag is valid on V1 4.1032+, photo type on 4.1037+.
        const bool junkSupported =
            !displayState_.hasV1Version || (displayState_.v1FirmwareVersion >= 41032);
        const bool photoSupported =
            !displayState_.hasV1Version || (displayState_.v1FirmwareVersion >= 41037);
        bool isJunk = junkSupported && ((aux0 & 0x40) != 0);
        uint8_t photoType = photoSupported ? static_cast<uint8_t>(aux0 & 0x0F) : 0;

        if (nextAlertCount < MAX_ALERTS) {
            AlertData& alert = nextAlerts[nextAlertCount++];
            alert.band = band;
            alert.direction = dir;
            alert.isPriority = isPriority;
            alert.isJunk = isJunk;
            alert.photoType = photoType;
            alert.rawBandBits = rawBandBits;
            alert.isKu = isKu;

            // Bytes 3 and 4 are raw RSSI values for front/rear antennas.
            alert.frontStrength = mapStrengthToBars(band, a[3]);
            alert.rearStrength = mapStrengthToBars(band, a[4]);

            alert.frequency = (band == BAND_LASER) ? 0 : combineMSBLSB(a[1], a[2]); // MHz
            alert.isValid = true;

            anyJunk |= isJunk;
            anyPhoto |= (photoType != 0);
            anyKu |= (band == BAND_KU);
        }
    }

    alerts_ = nextAlerts;
    alertCount_ = nextAlertCount;

    if (alertCount_ > 0) {
        // Priority resolution order:
        // 1) Per-row aux0 bit7 (matches AndroidESPLibrary2 AlertData::isPriority())
        // 2) First usable alert
        // 3) Entry 0 as last-resort safety fallback
        auto isUsableAlert = [this](int idx) -> bool {
            if (idx < 0 || idx >= static_cast<int>(alertCount_)) return false;
            const AlertData& a = alerts_[static_cast<size_t>(idx)];
            if (!a.isValid || a.band == BAND_NONE) return false;
            if (a.band != BAND_LASER && a.frequency == 0) return false;
            return true;
        };

        int priorityFromRowFlag = -1;
        for (size_t i = 0; i < alertCount_; ++i) {
            if (alerts_[i].isPriority) {
                priorityFromRowFlag = static_cast<int>(i);
                break;
            }
        }
        if (priorityFromRowFlag >= 0 && !isUsableAlert(priorityFromRowFlag)) {
            PARSER_PERF_INC(prioritySelectUnusableIndex);
        }

        enum class PrioritySource : uint8_t { RowFlag = 2, FirstUsable = 3, FirstEntry = 4 };

        int priorityIdx = -1;
        PrioritySource source = PrioritySource::FirstEntry;
        if (isUsableAlert(priorityFromRowFlag)) {
            priorityIdx = priorityFromRowFlag;
            source = PrioritySource::RowFlag;
        }
        if (priorityIdx < 0) {
            for (size_t i = 0; i < alertCount_; ++i) {
                if (isUsableAlert(static_cast<int>(i))) {
                    priorityIdx = static_cast<int>(i);
                    source = PrioritySource::FirstUsable;
                    break;
                }
            }
        }
        if (priorityIdx < 0) {
            priorityIdx = 0;
            source = PrioritySource::FirstEntry;
        }

        switch (source) {
            case PrioritySource::RowFlag:
                PARSER_PERF_INC(prioritySelectRowFlag);
                break;
            case PrioritySource::FirstUsable:
                PARSER_PERF_INC(prioritySelectFirstUsable);
                break;
            case PrioritySource::FirstEntry:
            default:
                PARSER_PERF_INC(prioritySelectFirstEntry);
                break;
        }

        if (!isUsableAlert(priorityIdx)) {
            PARSER_PERF_INC(prioritySelectInvalidChosen);
        }

        if (PARSER_TRACE_ENABLED()) {
            const char* src =
                (source == PrioritySource::RowFlag) ? "rowFlag" :
                (source == PrioritySource::FirstUsable) ? "firstUsable" : "firstEntry";
            Serial.printf("[AlertPri] src=%s idx=%u cnt=%u rowFlagIdx=%d\n",
                          src,
                          static_cast<unsigned>(priorityIdx),
                          static_cast<unsigned>(alertCount_),
                          priorityFromRowFlag);
        }

        displayState_.v1PriorityIndex = priorityIdx;
        displayState_.priorityArrow = alerts_[priorityIdx].direction;
    }

    // Keep mute authoritative from display packets (InfDisplayData/Aux0 soft mute).
    displayState_.hasJunkAlert = anyJunk;
    displayState_.hasPhotoAlert = anyPhoto;
    // Mirror Ku-alert presence onto a
    // display-state flag so the renderer can re-label the K cell as "Ku"
    // while a Ku alert is active.  Cleared above on count=0 alert tables.
    displayState_.hasKuAlert = anyKu;

    PARSER_PERF_INC(alertTablePublishes);
    if (receivedAlertCount == 3) {
        PARSER_PERF_INC(alertTablePublishes3Bogey);
    }

    // Match VR AlertDataProcessor behavior: clear per-count rows after a complete
    // table publish so the next table is assembled from fresh rows.
    clearAlertCacheForCount(receivedAlertCount);
    return true;
}

AlertData PacketParser::getPriorityAlert() const {
    if (alertCount_ == 0) {
        return AlertData();
    }

    // displayState_.v1PriorityIndex is resolved in parseAlertData():
    // alert-row aux0 bit7 first, then safety fallbacks.
    uint8_t idx = displayState_.v1PriorityIndex;
    if (idx < alertCount_) {
        return alerts_[idx];
    }
    return alerts_[0];  // Fallback
}

bool PacketParser::getRenderablePriorityAlert(AlertData& out) const {
    out = AlertData();
    if (alertCount_ == 0) {
        return false;
    }

    auto isRenderable = [](const AlertData& a) -> bool {
        if (!a.isValid || a.band == BAND_NONE) {
            return false;
        }
        return (a.band == BAND_LASER) || (a.frequency != 0);
    };

    const AlertData priority = getPriorityAlert();
    if (isRenderable(priority)) {
        out = priority;
        return true;
    }

    for (size_t i = 0; i < alertCount_; ++i) {
        if (isRenderable(alerts_[i])) {
            out = alerts_[i];
            return true;
        }
    }

    return false;
}
