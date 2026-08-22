#pragma once

#include <cstddef>
#include <cstdint>

#include "packet_parser_types.h"

// All payload and semantic-table digests in qualification evidence use the
// same byte-for-byte FNV-1a 32-bit definition. Keeping it here lets the BLE,
// parser, encounter, and display paths share one implementation.
inline constexpr uint32_t V1_FNV1A32_OFFSET_BASIS = 2166136261UL;
inline constexpr uint32_t V1_FNV1A32_PRIME = 16777619UL;
inline constexpr const char* V1_CAUSAL_DIGEST_ALGORITHM = "fnv1a32";

inline uint32_t v1Fnv1a32Update(uint32_t digest, const uint8_t* bytes, size_t length) {
    if (!bytes) {
        return digest;
    }
    for (size_t i = 0; i < length; ++i) {
        digest ^= bytes[i];
        digest *= V1_FNV1A32_PRIME;
    }
    return digest;
}

inline uint32_t v1Fnv1a32(const uint8_t* bytes, size_t length) {
    return v1Fnv1a32Update(V1_FNV1A32_OFFSET_BASIS, bytes, length);
}

inline uint32_t v1Fnv1a32U32(uint32_t digest, uint32_t value) {
    const uint8_t bytes[] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8),
                             static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 24)};
    return v1Fnv1a32Update(digest, bytes, sizeof(bytes));
}

// Ordered semantic digest used to join a display commit to the complete alert
// rows in the encounter CSV. It hashes alert_count followed by every AlertData
// field in wire-independent little-endian order; it never hashes struct padding.
inline uint32_t v1AlertTableFnv1a32(const AlertData* alerts, size_t count) {
    uint32_t digest = v1Fnv1a32U32(V1_FNV1A32_OFFSET_BASIS, static_cast<uint32_t>(count));
    if (!alerts) {
        return digest;
    }
    for (size_t i = 0; i < count; ++i) {
        const AlertData& alert = alerts[i];
        digest = v1Fnv1a32U32(digest, static_cast<uint32_t>(alert.band));
        digest = v1Fnv1a32U32(digest, static_cast<uint32_t>(alert.direction));
        const uint8_t fields[] = {
            alert.v1Index,
            alert.frontRawStrength,
            alert.rearRawStrength,
            alert.frontStrength,
            alert.rearStrength,
            static_cast<uint8_t>(alert.isValid ? 1 : 0),
            static_cast<uint8_t>(alert.isPriority ? 1 : 0),
            static_cast<uint8_t>(alert.isJunk ? 1 : 0),
            alert.photoType,
            alert.rawBandBits,
            static_cast<uint8_t>(alert.isKu ? 1 : 0),
        };
        digest = v1Fnv1a32Update(digest, fields, sizeof(fields));
        digest = v1Fnv1a32U32(digest, alert.frequency);
    }
    return digest;
}

enum class V1CausalStage : uint8_t {
    SessionStart = 0,
    Rx = 1,
    Framing = 2,
    Parse = 3,
    PublishState = 4,
    PublishAlerts = 5,
    SessionEnd = 6,
    // A qualification session may begin while the renderer still holds
    // parser-published state from before QSTART. These records retain that
    // original source identity without pretending another parse occurred.
    StateBaseline = 7,
    AlertTableBaseline = 8,
};

enum class V1CausalOutcome : uint8_t {
    Started = 0,
    Accepted = 1,
    BufferDropped = 2,
    Parsed = 3,
    Rejected = 4,
    Handled = 5,
    Published = 6,
    Ended = 7,
    ResyncDiscardedPrefix = 8,
    ResyncNoStart = 9,
    ResyncZeroLength = 10,
    ResyncTooLarge = 11,
    ResyncMissingEnd = 12,
    SessionClosedIncomplete = 13,
    Retained = 14,
};

enum class V1CausalPayloadUnit : uint8_t {
    None = 0,
    Notification = 1,
    Frame = 2,
    Candidate = 3,
};

struct V1CausalTraceRecord {
    V1CausalIdentity identity{};
    // When this owning stage ran. identity.dutMillis remains the receive time
    // of the last notification byte needed by this notification/frame/candidate.
    uint32_t stageDutMillis = 0;
    V1CausalStage stage = V1CausalStage::Rx;
    V1CausalOutcome outcome = V1CausalOutcome::Accepted;
    V1CausalPayloadUnit payloadUnit = V1CausalPayloadUnit::None;
    uint8_t packetId = 0;
    bool parseOk = false;
    uint32_t stateRevision = 0;
    uint32_t alertRevision = 0;
    uint32_t alertTableDigest = 0;
    uint32_t sourceLossCount = 0;
    uint64_t stageDutMicros = 0;
};
