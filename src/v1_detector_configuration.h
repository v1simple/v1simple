/**
 * Pure V1 Gen2 user-settings and detector-configuration value model.
 *
 * Profile serialization and storage intentionally live in v1_profiles.h.
 */

#pragma once
#ifndef V1_DETECTOR_CONFIGURATION_H
#define V1_DETECTOR_CONFIGURATION_H

#include <cstdint>
#include <cstring>

#include "v1_custom_frequency_definitions.h"

// V1 Gen2 User Settings (6 bytes). Accessor behavior is pinned by
// test_protocol_spec_conformance and test_v1_profiles.
struct V1UserSettings {
    uint8_t bytes[6];

    // Byte 0 bits
    bool xBandEnabled() const { return bytes[0] & 0x01; }
    bool kBandEnabled() const { return bytes[0] & 0x02; }
    bool kaBandEnabled() const { return bytes[0] & 0x04; }
    bool laserEnabled() const { return bytes[0] & 0x08; }
    bool muteToMuteVolume() const { return bytes[0] & 0x10; }
    bool bogeyLockLoud() const { return bytes[0] & 0x20; }
    bool muteXKRear() const { return !(bytes[0] & 0x40); }    // Inverted
    bool kuBandEnabled() const { return !(bytes[0] & 0x80); } // Inverted

    // Byte 1 bits
    bool euroMode() const { return !(bytes[1] & 0x01); } // Inverted
    bool kVerifier() const { return bytes[1] & 0x02; }   // TMF
    bool laserRear() const { return bytes[1] & 0x04; }
    bool customFreqs() const { return !(bytes[1] & 0x08); }      // Inverted
    bool kaAlwaysPriority() const { return !(bytes[1] & 0x10); } // Inverted
    bool fastLaserDetect() const { return bytes[1] & 0x20; }
    uint8_t kaSensitivity() const { return (bytes[1] >> 6) & 0x03; } // 3=Full, 2=Original, 1=Relaxed

    // Byte 2 bits
    bool startupSequence() const { return bytes[2] & 0x01; }
    bool restingDisplay() const { return bytes[2] & 0x02; }
    bool bsmPlus() const { return !(bytes[2] & 0x04); }             // Inverted
    uint8_t autoMute() const { return (bytes[2] >> 3) & 0x03; }     // 3=Off, 2=On, 1=Advanced
    uint8_t kSensitivity() const { return (bytes[2] >> 5) & 0x03; } // 3=Original, 2=Full, 1=Relaxed
    bool mrct() const { return !(bytes[2] & 0x80); }                // Inverted

    // Byte 3 bits
    uint8_t xSensitivity() const { return bytes[3] & 0x03; }  // 3=Original, 2=Full, 1=Relaxed
    bool driveSafe3D() const { return !(bytes[3] & 0x04); }   // Inverted
    bool driveSafe3DHD() const { return !(bytes[3] & 0x08); } // Inverted
    bool redflexHalo() const { return !(bytes[3] & 0x10); }   // Inverted
    bool redflexNK7() const { return !(bytes[3] & 0x20); }    // Inverted
    bool ekin() const { return !(bytes[3] & 0x40); }          // Inverted
    bool photoVerifier() const { return !(bytes[3] & 0x80); } // Inverted

    // Byte 4 bits (V4.1039+)
    bool gatsoRT4() const { return !(bytes[4] & 0x01); }                // Inverted
    bool photoIntersectionFilter() const { return !(bytes[4] & 0x02); } // Inverted

    // Setters
    void setXBandEnabled(bool v) {
        if (v)
            bytes[0] |= 0x01;
        else
            bytes[0] &= ~0x01;
    }
    void setKBandEnabled(bool v) {
        if (v)
            bytes[0] |= 0x02;
        else
            bytes[0] &= ~0x02;
    }
    void setKaBandEnabled(bool v) {
        if (v)
            bytes[0] |= 0x04;
        else
            bytes[0] &= ~0x04;
    }
    void setLaserEnabled(bool v) {
        if (v)
            bytes[0] |= 0x08;
        else
            bytes[0] &= ~0x08;
    }
    void setMuteToMuteVolume(bool v) {
        if (v)
            bytes[0] |= 0x10;
        else
            bytes[0] &= ~0x10;
    }
    void setBogeyLockLoud(bool v) {
        if (v)
            bytes[0] |= 0x20;
        else
            bytes[0] &= ~0x20;
    }
    void setMuteXKRear(bool v) {
        if (v)
            bytes[0] &= ~0x40;
        else
            bytes[0] |= 0x40;
    } // Inverted
    void setKuBandEnabled(bool v) {
        if (v)
            bytes[0] &= ~0x80;
        else
            bytes[0] |= 0x80;
    } // Inverted

    void setEuroMode(bool v) {
        if (v)
            bytes[1] &= ~0x01;
        else
            bytes[1] |= 0x01;
    } // Inverted
    void setKVerifier(bool v) {
        if (v)
            bytes[1] |= 0x02;
        else
            bytes[1] &= ~0x02;
    }
    void setLaserRear(bool v) {
        if (v)
            bytes[1] |= 0x04;
        else
            bytes[1] &= ~0x04;
    }
    void setCustomFreqs(bool v) {
        if (v)
            bytes[1] &= ~0x08;
        else
            bytes[1] |= 0x08;
    } // Inverted
    void setKaAlwaysPriority(bool v) {
        if (v)
            bytes[1] &= ~0x10;
        else
            bytes[1] |= 0x10;
    } // Inverted
    void setFastLaserDetect(bool v) {
        if (v)
            bytes[1] |= 0x20;
        else
            bytes[1] &= ~0x20;
    }
    void setKaSensitivity(uint8_t v) { bytes[1] = (bytes[1] & 0x3F) | ((v & 0x03) << 6); }

    void setStartupSequence(bool v) {
        if (v)
            bytes[2] |= 0x01;
        else
            bytes[2] &= ~0x01;
    }
    void setRestingDisplay(bool v) {
        if (v)
            bytes[2] |= 0x02;
        else
            bytes[2] &= ~0x02;
    }
    void setBsmPlus(bool v) {
        if (v)
            bytes[2] &= ~0x04;
        else
            bytes[2] |= 0x04;
    } // Inverted
    void setAutoMute(uint8_t v) { bytes[2] = (bytes[2] & 0xE7) | ((v & 0x03) << 3); }
    void setKSensitivity(uint8_t v) { bytes[2] = (bytes[2] & 0x9F) | ((v & 0x03) << 5); }
    void setMrct(bool v) {
        if (v)
            bytes[2] &= ~0x80;
        else
            bytes[2] |= 0x80;
    } // Inverted

    void setXSensitivity(uint8_t v) { bytes[3] = (bytes[3] & 0xFC) | (v & 0x03); }
    void setDriveSafe3D(bool v) {
        if (v)
            bytes[3] &= ~0x04;
        else
            bytes[3] |= 0x04;
    } // Inverted
    void setDriveSafe3DHD(bool v) {
        if (v)
            bytes[3] &= ~0x08;
        else
            bytes[3] |= 0x08;
    } // Inverted
    void setRedflexHalo(bool v) {
        if (v)
            bytes[3] &= ~0x10;
        else
            bytes[3] |= 0x10;
    } // Inverted
    void setRedflexNK7(bool v) {
        if (v)
            bytes[3] &= ~0x20;
        else
            bytes[3] |= 0x20;
    } // Inverted
    void setEkin(bool v) {
        if (v)
            bytes[3] &= ~0x40;
        else
            bytes[3] |= 0x40;
    } // Inverted
    void setPhotoVerifier(bool v) {
        if (v)
            bytes[3] &= ~0x80;
        else
            bytes[3] |= 0x80;
    } // Inverted
    void setGatsoRT4(bool v) {
        if (v)
            bytes[4] &= ~0x01;
        else
            bytes[4] |= 0x01;
    } // Inverted
    void setPhotoIntersectionFilter(bool v) {
        if (v)
            bytes[4] &= ~0x02;
        else
            bytes[4] |= 0x02;
    } // Inverted

    // Initialize to factory defaults (all 0xFF)
    void setDefaults() { memset(bytes, 0xFF, 6); }

    V1UserSettings() { setDefaults(); }
};

enum class V1UserSettingsPolicy : uint8_t {
    Unchanged = 0,
    Value = 1,
};

enum class V1ModePolicy : uint8_t {
    Unchanged = 0,
    Value = 1,
};

enum class V1DisplayPolicy : uint8_t {
    Unchanged = 0,
    On = 1,
    Off = 2,
};

enum class V1VolumePolicy : uint8_t {
    Unchanged = 0,
    Temporary = 1,
    Saved = 2,
};

enum class V1VolumeFeedbackPolicy : uint8_t {
    None = 0,
    ChangedOnly = 1,
    Always = 2,
};

enum class V1VolumeDisconnectPolicy : uint8_t {
    RestoreSaved = 0,
    KeepCurrent = 1,
};

enum class V1BluetoothLedPolicy : uint8_t {
    Unchanged = 0,
    Off = 1,
    On = 2,
};

enum class V1CustomFrequencyPolicy : uint8_t {
    Unchanged = 0,
    Value = 1,
};

// Everything sent to the detector belongs to the selected profile.  Auto-Push
// slots retain only selection and V1Simple presentation overlays after the
// legacy-slot migration has committed.
struct V1DetectorConfiguration {
    V1UserSettingsPolicy userSettingsPolicy = V1UserSettingsPolicy::Value;
    V1ModePolicy modePolicy = V1ModePolicy::Unchanged;
    uint8_t mode = 0;
    V1DisplayPolicy displayPolicy = V1DisplayPolicy::Unchanged;
    V1VolumePolicy volumePolicy = V1VolumePolicy::Unchanged;
    uint8_t mainVolume = 0;
    uint8_t mutedVolume = 0;
    V1VolumeFeedbackPolicy volumeFeedback = V1VolumeFeedbackPolicy::None;
    V1VolumeDisconnectPolicy volumeDisconnect = V1VolumeDisconnectPolicy::RestoreSaved;
    V1BluetoothLedPolicy bluetoothLedPolicy = V1BluetoothLedPolicy::Unchanged;
    V1CustomFrequencyPolicy customFrequencyPolicy = V1CustomFrequencyPolicy::Unchanged;
    V1CustomFrequencyDefinitionList customFrequencyDefinitions;
};

inline bool operator==(const V1DetectorConfiguration& lhs, const V1DetectorConfiguration& rhs) {
    return lhs.userSettingsPolicy == rhs.userSettingsPolicy && lhs.modePolicy == rhs.modePolicy &&
           lhs.mode == rhs.mode && lhs.displayPolicy == rhs.displayPolicy &&
           lhs.volumePolicy == rhs.volumePolicy && lhs.mainVolume == rhs.mainVolume &&
           lhs.mutedVolume == rhs.mutedVolume && lhs.volumeFeedback == rhs.volumeFeedback &&
           lhs.volumeDisconnect == rhs.volumeDisconnect && lhs.bluetoothLedPolicy == rhs.bluetoothLedPolicy &&
           lhs.customFrequencyPolicy == rhs.customFrequencyPolicy &&
           lhs.customFrequencyDefinitions == rhs.customFrequencyDefinitions;
}

inline bool operator!=(const V1DetectorConfiguration& lhs, const V1DetectorConfiguration& rhs) {
    return !(lhs == rhs);
}

#endif // V1_DETECTOR_CONFIGURATION_H
