#pragma once

// Firmware version-gating vectors transcribed from Valentine Research's
// official iOS ESP Library, ValentineResearch/iOSESPLibrary @ d04f665
// ("Added support for V4.1039"). Held apart from src/v1_firmware_compat.h so
// that a wrong belief about a version gate cannot also author its own
// expected values.

#include <stdint.h>

namespace vendor_esp {

// V1VersionUtil.h (lines 12-46).
inline constexpr uint32_t kInitialV1Gen2Version = 40000;      // INITIAL_V1_GEN_2_VERSION
inline constexpr uint32_t kMaxV1Gen2Version = 49999;          // MAX_V1_GEN_2_VERSION
inline constexpr uint32_t kAllowGatsoRT4StartVersion = 41039; // ALLOW_GATSO_RT4_START_VERSION
// ALLOW_PHOTO_INTERSECTION_FILTER_START_VERSON (sic, vendor typo preserved).
inline constexpr uint32_t kAllowPhotoIntersectionFilterStartVersion = 41039;
// DEFAULT_V1_VERSION: the vendor library substitutes this when no version
// has been received from the connected V1.
inline constexpr uint32_t kDefaultV1Version = 41039;

// ESPV1UserBytes.m, getNumberOfSupportedBytesForV1Version (lines 135-148):
//   version < 40000 -> 6  "V1 Gen1 supported the full 6 bytes in the ESP
//                          specification"
//   version < 41039 -> 4  "Earlier V1 Gen2 versions only supported 4 user
//                          bytes internally, but still expected 6 bytes on
//                          the ESP bus"
//   otherwise       -> 6
struct SupportedByteCountRow {
    uint32_t version;
    uint8_t count;
    const char* note;
};
inline constexpr SupportedByteCountRow kSupportedUserByteCounts[] = {
    {1, 6, "Gen1 floor"},
    {38920, 6, "typical Gen1 release"},
    {39999, 6, "last pre-Gen2 version"},
    {40000, 4, "INITIAL_V1_GEN_2_VERSION"},
    {41032, 4, "early Gen2 (Ka sensitivity era)"},
    {41038, 4, "last legacy-shape Gen2"},
    {41039, 6, "ALLOW_GATSO_RT4_START_VERSION"},
    {41040, 6, "post-4.1039"},
    {49999, 6, "MAX_V1_GEN_2_VERSION"},
};

// ESPV1UserBytes.m, defaultUnusedBytesForV1Version (lines 151-159):
// bytes at index >= supported count are set to 0xFF.
inline constexpr uint8_t kUnusedUserByteFill = 0xFF;

} // namespace vendor_esp
