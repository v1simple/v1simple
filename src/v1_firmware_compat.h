#pragma once

#include <cstdint>
#include <cstring>

namespace V1FirmwareCompat {

inline constexpr uint32_t kInitialGen2Version = 40000;
inline constexpr uint32_t kFirstUnverifiedFutureMajorVersion = 50000;
inline constexpr uint32_t kCustomFrequenciesVersion = 41018;
inline constexpr uint32_t kUserSettingsVersion = 41018;
inline constexpr uint32_t kVolumeChangeVersion = 41026;
inline constexpr uint32_t kModeObservationVersion = 41028;
inline constexpr uint32_t kKaPriorityAndFastLaserVersion = 41031;
inline constexpr uint32_t kKeepBluetoothLedOnVersion = 41032;
inline constexpr uint32_t kKaSensitivityVersion = 41032;
inline constexpr uint32_t kStartupRestingDisplayAndBsmVersion = 41035;
inline constexpr uint32_t kAutoMuteVersion = 41036;
inline constexpr uint32_t kSavedVolumeVersion = 41037;
inline constexpr uint32_t kKeepCurrentVolumeOnDisconnectVersion = 41038;
inline constexpr uint32_t kDisplayActiveVersion = 41037;
inline constexpr uint32_t kKAndXSensitivityVersion = 41037;
inline constexpr uint32_t kPhotoRadarVersion = 41037;
inline constexpr uint32_t kFullUserBytesVersion = 41039;
inline constexpr uint8_t kUserByteCount = 6;
inline constexpr uint8_t kLegacyGen2UserByteCount = 4;

// One firmware capability vocabulary for protocol requests and UI/API
// presentation. Thresholds match ESP Specification 3.016 and VR's
// AndroidESPLibrary2 V1VersionInfo at 50fe7ba.
struct Capabilities {
    bool versionKnown = false;
    bool gen2 = false;
    uint8_t supportedUserByteCount = kLegacyGen2UserByteCount;
    bool customSweeps = false;
    // Vendor-defined reqWriteVolume support. This is a write capability, not a
    // claim that current/saved volume was observed by the capture session.
    bool volumeChange = false;
    bool modeObservation = false;
    bool kaAlwaysPriority = false;
    bool fastLaserDetect = false;
    bool keepBluetoothLedOn = false;
    bool kaSensitivity = false;
    bool startupSequence = false;
    bool restingDisplay = false;
    bool bsmPlus = false;
    bool autoMute = false;
    bool allVolume = false;
    bool savedVolume = false;
    bool displayActive = false;
    bool kSensitivity = false;
    bool xSensitivity = false;
    bool photoRadar = false;
    bool gatsoRT4 = false;
    bool photoIntersectionFilter = false;
};

inline uint8_t supportedUserByteCount(uint32_t firmwareVersion) {
    if (firmwareVersion >= kFirstUnverifiedFutureMajorVersion) {
        // The 4.x byte map must not be presented as qualified for an unknown
        // future major protocol. Raw captured bytes remain available through
        // the observation payload, but their writable shape is unknown.
        return 0;
    }
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

inline Capabilities capabilities(uint32_t firmwareVersion) {
    Capabilities result;
    result.versionKnown = firmwareVersion != 0;
    // Do not silently project the 4.x protocol onto a future major release.
    result.gen2 = firmwareVersion >= kInitialGen2Version && firmwareVersion < kFirstUnverifiedFutureMajorVersion;
    result.supportedUserByteCount = supportedUserByteCount(firmwareVersion);
    if (!result.versionKnown || !result.gen2) {
        return result;
    }

    result.customSweeps = firmwareVersion >= kCustomFrequenciesVersion;
    result.volumeChange = firmwareVersion >= kVolumeChangeVersion;
    result.modeObservation = firmwareVersion >= kModeObservationVersion;
    result.kaAlwaysPriority = firmwareVersion >= kKaPriorityAndFastLaserVersion;
    result.fastLaserDetect = firmwareVersion >= kKaPriorityAndFastLaserVersion;
    result.keepBluetoothLedOn = firmwareVersion >= kKeepBluetoothLedOnVersion;
    result.kaSensitivity = firmwareVersion >= kKaSensitivityVersion;
    result.startupSequence = firmwareVersion >= kStartupRestingDisplayAndBsmVersion;
    result.restingDisplay = firmwareVersion >= kStartupRestingDisplayAndBsmVersion;
    result.bsmPlus = firmwareVersion >= kStartupRestingDisplayAndBsmVersion;
    result.autoMute = firmwareVersion >= kAutoMuteVersion;
    result.allVolume = firmwareVersion >= kSavedVolumeVersion;
    result.savedVolume = firmwareVersion >= kSavedVolumeVersion;
    result.displayActive = firmwareVersion >= kDisplayActiveVersion;
    result.kSensitivity = firmwareVersion >= kKAndXSensitivityVersion;
    result.xSensitivity = firmwareVersion >= kKAndXSensitivityVersion;
    result.photoRadar = firmwareVersion >= kPhotoRadarVersion;
    result.gatsoRT4 = firmwareVersion >= kFullUserBytesVersion;
    result.photoIntersectionFilter = firmwareVersion >= kFullUserBytesVersion;
    return result;
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

// ESP Specification 3.016 Appendix 12.1. A profile may contain all six raw
// bytes, but Apply owns only the bits implemented by the connected firmware.
// Every zero bit here is copied from the live pre-apply response unchanged.
inline void supportedUserByteMasks(uint32_t firmwareVersion, uint8_t masks[kUserByteCount]) {
    if (!masks) return;
    std::memset(masks, 0, kUserByteCount);
    if (firmwareVersion < kUserSettingsVersion) return;

    masks[0] = 0xFF;
    masks[1] = 0x0F;
    if (firmwareVersion >= kKaPriorityAndFastLaserVersion) masks[1] = 0x3F;
    if (firmwareVersion >= kKaSensitivityVersion) masks[1] = 0xFF;
    if (firmwareVersion >= kStartupRestingDisplayAndBsmVersion) masks[2] = 0x07;
    if (firmwareVersion >= kAutoMuteVersion) masks[2] = 0x1F;
    if (firmwareVersion >= kKAndXSensitivityVersion) {
        masks[2] = 0xFF;
        masks[3] = 0xFF;
    }
    if (firmwareVersion >= kFullUserBytesVersion) masks[4] = 0x03;
}

inline bool hasWritableUserSettings(uint32_t firmwareVersion) {
    return firmwareVersion >= kUserSettingsVersion && firmwareVersion < kFirstUnverifiedFutureMajorVersion;
}

inline void overlaySupportedUserBytes(const uint8_t live[kUserByteCount], const uint8_t desired[kUserByteCount],
                                      uint8_t effective[kUserByteCount], uint32_t firmwareVersion) {
    if (!live || !desired || !effective) return;
    uint8_t masks[kUserByteCount];
    supportedUserByteMasks(firmwareVersion, masks);
    for (uint8_t index = 0; index < kUserByteCount; ++index) {
        effective[index] = static_cast<uint8_t>((live[index] & static_cast<uint8_t>(~masks[index])) |
                                                (desired[index] & masks[index]));
    }
}

inline bool userBytesMatchSupported(const uint8_t left[kUserByteCount], const uint8_t right[kUserByteCount],
                                    uint32_t firmwareVersion) {
    if (!left || !right) return false;
    uint8_t masks[kUserByteCount];
    supportedUserByteMasks(firmwareVersion, masks);
    for (uint8_t index = 0; index < kUserByteCount; ++index) {
        if ((left[index] & masks[index]) != (right[index] & masks[index])) return false;
    }
    return true;
}

// Multi-bit enum value zero is reserved/invalid for every sensitivity and
// Auto Mute field. V1 substitutes a default, so accepting zero would make the
// requested target inherently unverifiable.
inline bool hasValidModeledUserSettingValues(const uint8_t bytes[kUserByteCount], uint32_t firmwareVersion) {
    if (!bytes) return false;
    const Capabilities supported = capabilities(firmwareVersion);
    if (supported.kaSensitivity && ((bytes[1] >> 6) & 0x03) == 0) return false;
    if (supported.autoMute && ((bytes[2] >> 3) & 0x03) == 0) return false;
    if (supported.kSensitivity && ((bytes[2] >> 5) & 0x03) == 0) return false;
    if (supported.xSensitivity && (bytes[3] & 0x03) == 0) return false;
    return true;
}

} // namespace V1FirmwareCompat
