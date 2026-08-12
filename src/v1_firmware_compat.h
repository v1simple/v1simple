#pragma once

#include <cstdint>
#include <cstring>

namespace V1FirmwareCompat {

inline constexpr uint32_t kInitialGen2Version = 40000;
inline constexpr uint32_t kFullUserBytesVersion = 41039;
inline constexpr uint8_t kUserByteCount = 6;
inline constexpr uint8_t kLegacyGen2UserByteCount = 4;

inline uint8_t supportedUserByteCount(uint32_t firmwareVersion) {
    if (firmwareVersion != 0 && firmwareVersion < kInitialGen2Version) {
        // Gen1 supports all six ESP user bytes (iOSESPLibrary @ d04f665,
        // ESPV1UserBytes.m getNumberOfSupportedBytesForV1Version).
        return kUserByteCount;
    }
    if (firmwareVersion >= kFullUserBytesVersion) {
        return kUserByteCount;
    }
    // Until the connected Gen2 version is known, use the legacy-safe shape.
    return kLegacyGen2UserByteCount;
}

inline void prepareUserBytesForWrite(const uint8_t input[kUserByteCount], uint8_t output[kUserByteCount],
                                     uint32_t firmwareVersion) {
    if (!input || !output) {
        return;
    }
    std::memcpy(output, input, kUserByteCount);
    for (uint8_t i = supportedUserByteCount(firmwareVersion); i < kUserByteCount; ++i) {
        output[i] = 0xFF;
    }
}

} // namespace V1FirmwareCompat
