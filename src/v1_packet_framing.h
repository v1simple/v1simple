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

} // namespace V1PacketFraming
