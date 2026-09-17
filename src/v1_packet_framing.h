#pragma once

#include <stddef.h>
#include <stdint.h>

namespace V1PacketFraming {

constexpr uint8_t kOriginatorBase = 0xE0;
constexpr uint8_t kOriginatorMask = 0xF0;
constexpr uint8_t kDeviceIdMask = 0x0F;
constexpr uint8_t kV1WithoutChecksum = 0x09;
constexpr uint8_t kV1WithChecksum = 0x0A;

// The ESP payload-length byte includes V1's checksum when the encoded
// originator is EAh, but not for the no-checksum E9h V1. Require exactly the
// data bytes a response defines plus only that originator-qualified checksum.
inline bool hasCanonicalResponseWidth(const uint8_t* packet, size_t packetSize, size_t dataBytes) {
    if (!packet || packetSize < 6 || (packet[2] & kOriginatorMask) != kOriginatorBase) return false;

    const uint8_t originator = packet[2] & kDeviceIdMask;
    size_t checksumBytes = 0;
    if (originator == kV1WithChecksum) {
        checksumBytes = 1;
    } else if (originator != kV1WithoutChecksum) {
        return false;
    }

    const size_t expectedPayload = dataBytes + checksumBytes;
    return packet[4] == expectedPayload && packetSize == expectedPayload + 6;
}

// A response is usable only when both its V1 origin/shape and (for EAh)
// checksum are canonical. Packet owners additionally bind the destination and
// exact data width before the response can affect rendering or control state.
inline bool hasCanonicalResponseEvidence(const uint8_t* packet, size_t packetSize, size_t dataBytes) {
    if (!hasCanonicalResponseWidth(packet, packetSize, dataBytes)) return false;
    if ((packet[2] & kDeviceIdMask) == kV1WithoutChecksum) return true;

    uint8_t checksum = 0;
    for (size_t index = 0; index + 2 < packetSize; ++index) {
        checksum = static_cast<uint8_t>(checksum + packet[index]);
    }
    return packet[packetSize - 2] == checksum;
}

// A canonical source and checksum are not enough to bind a response to this
// ESP role. InfDisplayData is broadcast to D8h; direct replies to the
// V1connection requester are addressed to D6h.
inline bool hasCanonicalResponseEvidenceForDestination(const uint8_t* packet, size_t packetSize, size_t dataBytes,
                                                       uint8_t destination) {
    return packet && packet[1] == destination && hasCanonicalResponseEvidence(packet, packetSize, dataBytes);
}

} // namespace V1PacketFraming
