#pragma once

#include <cstdint>
#include <cstring>

namespace V1FirmwareCompat {

inline constexpr uint32_t kInitialGen2Version = 40000;
inline constexpr uint32_t kCustomFrequenciesVersion = 41018;
inline constexpr uint32_t kVolumeChangeVersion = 41026;
inline constexpr uint32_t kModeObservationVersion = 41028;
inline constexpr uint32_t kKaPriorityAndFastLaserVersion = 41031;
inline constexpr uint32_t kKeepBluetoothLedOnVersion = 41032;
inline constexpr uint32_t kKaSensitivityVersion = 41032;
inline constexpr uint32_t kStartupRestingDisplayAndBsmVersion = 41035;
inline constexpr uint32_t kAutoMuteVersion = 41036;
inline constexpr uint32_t kSavedVolumeVersion = 41037;
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
    result.gen2 = firmwareVersion >= kInitialGen2Version;
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

} // namespace V1FirmwareCompat
