/**
 * V1 Profile Manager
 * Stores and manages V1 Gen2 user settings profiles on SD card
 */

#pragma once
#ifndef V1_PROFILES_H
#define V1_PROFILES_H

#include <Arduino.h>
#include <FS.h>
#include <ArduinoJson.h>
#include <cstring>
#include <limits>
#include <vector>

#include "json_exact_input.h"
#include "profile_name.h"
#include "v1_custom_frequency_definitions.h"
#include "v1_profile_limits.h"

class StorageManager;

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

inline constexpr uint8_t V1_PROFILE_SCHEMA_VERSION = 3;
inline constexpr uint8_t V1_PROFILE_PREVIOUS_SCHEMA_VERSION = 2;
inline bool validV1Utf8(const char* data, size_t length) {
    if (!data && length != 0) return false;
    size_t position = 0;
    while (position < length) {
        const uint8_t first = static_cast<uint8_t>(data[position++]);
        if (first < 0x80u) continue;
        uint8_t continuationCount = 0;
        uint32_t codePoint = 0;
        uint32_t minimum = 0;
        if ((first & 0xe0u) == 0xc0u) {
            continuationCount = 1;
            codePoint = first & 0x1fu;
            minimum = 0x80u;
        } else if ((first & 0xf0u) == 0xe0u) {
            continuationCount = 2;
            codePoint = first & 0x0fu;
            minimum = 0x800u;
        } else if ((first & 0xf8u) == 0xf0u) {
            continuationCount = 3;
            codePoint = first & 0x07u;
            minimum = 0x10000u;
        } else {
            return false;
        }
        if (position + continuationCount > length) return false;
        for (uint8_t index = 0; index < continuationCount; ++index) {
            const uint8_t byte = static_cast<uint8_t>(data[position++]);
            if ((byte & 0xc0u) != 0x80u) return false;
            codePoint = (codePoint << 6u) | (byte & 0x3fu);
        }
        if (codePoint < minimum || codePoint > 0x10ffffu ||
            (codePoint >= 0xd800u && codePoint <= 0xdfffu)) return false;
    }
    return true;
}

inline bool validV1ProfileDescription(const String& description) {
    return description.length() <= V1_PROFILE_DESCRIPTION_MAX_BYTES &&
           std::strlen(description.c_str()) == description.length() &&
           validV1Utf8(description.c_str(), description.length());
}

// ArduinoJson's `is<const char*>()` proves only that a value is a JSON string.
// Converting through c_str() would silently truncate an embedded NUL. Keep the
// byte length authoritative at every shared profile/settings ingress.
enum class ExactV1JsonStringStatus : uint8_t { Valid, Invalid, Unavailable };

inline ExactV1JsonStringStatus exactV1JsonStringChecked(
    JsonVariantConst value, String& output,
    size_t maxBytes = std::numeric_limits<size_t>::max()) {
    if (!value.is<const char*>()) return ExactV1JsonStringStatus::Invalid;
    const JsonString text = value.as<JsonString>();
    if (!text.c_str() || text.size() > maxBytes || std::strlen(text.c_str()) != text.size() ||
        !validV1Utf8(text.c_str(), text.size()) ||
        !ExactJsonInput::validSemanticString(text.c_str(), text.size())) {
        return ExactV1JsonStringStatus::Invalid;
    }
    output = String(text.c_str());
    if (output.length() != text.size() ||
        (text.size() > 0 && std::memcmp(output.c_str(), text.c_str(), text.size()) != 0)) {
        output = String();
        return ExactV1JsonStringStatus::Unavailable;
    }
    return ExactV1JsonStringStatus::Valid;
}

inline bool exactV1JsonString(JsonVariantConst value, String& output,
                              size_t maxBytes = std::numeric_limits<size_t>::max()) {
    return exactV1JsonStringChecked(value, output, maxBytes) == ExactV1JsonStringStatus::Valid;
}

inline bool exactV1JsonToken(JsonVariantConst value, const char* expected) {
    if (!expected || !value.is<const char*>()) return false;
    const JsonString text = value.as<JsonString>();
    const size_t expectedLength = std::strlen(expected);
    return text.c_str() && text.size() == expectedLength && std::strlen(text.c_str()) == text.size() &&
           validV1Utf8(text.c_str(), text.size()) &&
           std::memcmp(text.c_str(), expected, expectedLength) == 0;
}

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

// Profile with name and settings
struct V1Profile {
    String name;
    String description;
    V1UserSettings settings;
    V1DetectorConfiguration detector;
    uint8_t schemaVersion = V1_PROFILE_SCHEMA_VERSION;

    // Read-only migration carriers for pre-v2 files/backups. They were never
    // consumed by Auto-Push; v2 writers deliberately omit them.
    bool displayOn = true;
    uint8_t mainVolume = 0xFF;
    uint8_t mutedVolume = 0xFF;

    V1Profile() : name("Default"), description("") {}
    V1Profile(const String& n) : name(n), description("") {}
    V1Profile(const String& n, const V1UserSettings& s)
        : name(n), description(""), settings(s) {}
};

// Shared schema helpers keep profile files, backups, USB documents, and HTTP
// JSON on one strict representation.
inline void appendV1DetectorConfiguration(JsonObject target, const V1DetectorConfiguration& config) {
    target["userSettings"] =
        config.userSettingsPolicy == V1UserSettingsPolicy::Value ? "value" : "unchanged";
    JsonObject mode = target["mode"].to<JsonObject>();
    mode["policy"] = config.modePolicy == V1ModePolicy::Value ? "value" : "unchanged";
    if (config.modePolicy == V1ModePolicy::Value) mode["value"] = config.mode;
    target["display"] = config.displayPolicy == V1DisplayPolicy::On
                            ? "on"
                            : (config.displayPolicy == V1DisplayPolicy::Off ? "off" : "unchanged");
    JsonObject volume = target["volume"].to<JsonObject>();
    const char* volumePolicy = config.volumePolicy == V1VolumePolicy::Saved
                                   ? "saved"
                                   : (config.volumePolicy == V1VolumePolicy::Temporary ? "temporary" : "unchanged");
    volume["policy"] = volumePolicy;
    if (config.volumePolicy != V1VolumePolicy::Unchanged) {
        volume["main"] = config.mainVolume;
        volume["muted"] = config.mutedVolume;
        volume["feedback"] = config.volumeFeedback == V1VolumeFeedbackPolicy::Always
                                 ? "always"
                                 : (config.volumeFeedback == V1VolumeFeedbackPolicy::ChangedOnly
                                        ? "changed_only"
                                        : "none");
        volume["disconnect"] = config.volumeDisconnect == V1VolumeDisconnectPolicy::KeepCurrent
                                   ? "keep_current"
                                   : "restore_saved";
    }
    target["bluetoothLed"] = config.bluetoothLedPolicy == V1BluetoothLedPolicy::On
                                  ? "on"
                                  : (config.bluetoothLedPolicy == V1BluetoothLedPolicy::Off ? "off" : "unchanged");
    JsonObject custom = target["customFrequencies"].to<JsonObject>();
    custom["policy"] = config.customFrequencyPolicy == V1CustomFrequencyPolicy::Value ? "value" : "unchanged";
    if (config.customFrequencyPolicy == V1CustomFrequencyPolicy::Value) {
        JsonArray definitions = custom["definitions"].to<JsonArray>();
        for (const auto& definition : config.customFrequencyDefinitions) {
            JsonObject item = definitions.add<JsonObject>();
            item["index"] = definition.index;
            item["lowerMHz"] = definition.lowerMHz;
            item["upperMHz"] = definition.upperMHz;
        }
    }
}

inline void appendV1DetectorConfigurationV2(JsonObject target, const V1DetectorConfiguration& config) {
    target["userSettings"] =
        config.userSettingsPolicy == V1UserSettingsPolicy::Value ? "value" : "unchanged";
    JsonObject mode = target["mode"].to<JsonObject>();
    mode["policy"] = config.modePolicy == V1ModePolicy::Value ? "value" : "unchanged";
    if (config.modePolicy == V1ModePolicy::Value) mode["value"] = config.mode;
    target["display"] = config.displayPolicy == V1DisplayPolicy::On
                            ? "on"
                            : (config.displayPolicy == V1DisplayPolicy::Off ? "off" : "unchanged");
    JsonObject volume = target["volume"].to<JsonObject>();
    volume["policy"] = config.volumePolicy == V1VolumePolicy::Saved
                           ? "saved"
                           : (config.volumePolicy == V1VolumePolicy::Temporary ? "temporary" : "unchanged");
    if (config.volumePolicy != V1VolumePolicy::Unchanged) {
        volume["main"] = config.mainVolume;
        volume["muted"] = config.mutedVolume;
    }
    target["bluetoothLed"] = "unchanged";
    target["customFrequencies"] = "unchanged";
}

// Schema-v2 is retained as a distinct parser so an import can migrate its old
// display-off behavior explicitly instead of silently reinterpreting it under
// the v3 independent Bluetooth-LED contract.
inline bool parseV1DetectorConfigurationV2(JsonObjectConst source, V1DetectorConfiguration& config) {
    if (source.isNull() || source.size() != 6 || !source["userSettings"].is<const char*>() ||
        !source["mode"].is<JsonObjectConst>() || !source["display"].is<const char*>() ||
        !source["volume"].is<JsonObjectConst>() || !source["bluetoothLed"].is<const char*>() ||
        !source["customFrequencies"].is<const char*>()) return false;
    V1DetectorConfiguration parsed;
    if (exactV1JsonToken(source["userSettings"], "value")) parsed.userSettingsPolicy = V1UserSettingsPolicy::Value;
    else if (exactV1JsonToken(source["userSettings"], "unchanged")) parsed.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    else return false;
    const JsonObjectConst mode = source["mode"].as<JsonObjectConst>();
    if (!mode["policy"].is<const char*>()) return false;
    if (exactV1JsonToken(mode["policy"], "value")) {
        if (mode.size() != 2 || !mode["value"].is<int>() || mode["value"].as<int>() < 1 ||
            mode["value"].as<int>() > 3) return false;
        parsed.modePolicy = V1ModePolicy::Value;
        parsed.mode = static_cast<uint8_t>(mode["value"].as<int>());
    } else if (exactV1JsonToken(mode["policy"], "unchanged")) {
        if (mode.size() != 1) return false;
    } else return false;
    if (exactV1JsonToken(source["display"], "on")) parsed.displayPolicy = V1DisplayPolicy::On;
    else if (exactV1JsonToken(source["display"], "off")) parsed.displayPolicy = V1DisplayPolicy::Off;
    else if (!exactV1JsonToken(source["display"], "unchanged")) return false;
    const JsonObjectConst volume = source["volume"].as<JsonObjectConst>();
    if (!volume["policy"].is<const char*>()) return false;
    const bool volumeTemporary = exactV1JsonToken(volume["policy"], "temporary");
    const bool volumeSaved = exactV1JsonToken(volume["policy"], "saved");
    if (volumeTemporary || volumeSaved) {
        if (volume.size() != 3 || !volume["main"].is<int>() || !volume["muted"].is<int>() ||
            volume["main"].as<int>() < 0 || volume["main"].as<int>() > 9 ||
            volume["muted"].as<int>() < 0 || volume["muted"].as<int>() > 9) return false;
        parsed.volumePolicy = volumeSaved ? V1VolumePolicy::Saved : V1VolumePolicy::Temporary;
        parsed.mainVolume = static_cast<uint8_t>(volume["main"].as<int>());
        parsed.mutedVolume = static_cast<uint8_t>(volume["muted"].as<int>());
    } else if (exactV1JsonToken(volume["policy"], "unchanged")) {
        if (volume.size() != 1) return false;
    } else return false;
    if (!exactV1JsonToken(source["bluetoothLed"], "unchanged") ||
        !exactV1JsonToken(source["customFrequencies"], "unchanged")) return false;
    config = parsed;
    return true;
}

inline V1DetectorConfiguration migrateV1DetectorConfigurationV2(const V1DetectorConfiguration& v2) {
    V1DetectorConfiguration migrated = v2;
    migrated.volumeFeedback = V1VolumeFeedbackPolicy::None;
    migrated.volumeDisconnect = V1VolumeDisconnectPolicy::RestoreSaved;
    // A v2 display-off request always used the legacy no-Aux packet, which
    // turns the Bluetooth indicator off. Preserve that observable behavior.
    if (migrated.displayPolicy == V1DisplayPolicy::Off) {
        migrated.bluetoothLedPolicy = V1BluetoothLedPolicy::Off;
    }
    migrated.customFrequencyPolicy = V1CustomFrequencyPolicy::Unchanged;
    migrated.customFrequencyDefinitions.clear();
    return migrated;
}

inline bool parseV1DetectorConfiguration(JsonObjectConst source, V1DetectorConfiguration& config) {
    if (source.isNull() || source.size() != 6 || !source["userSettings"].is<const char*>() ||
        !source["mode"].is<JsonObjectConst>() || !source["display"].is<const char*>() ||
        !source["volume"].is<JsonObjectConst>() || !source["bluetoothLed"].is<const char*>() ||
        !source["customFrequencies"].is<JsonObjectConst>()) return false;

    V1DetectorConfiguration parsed;
    if (exactV1JsonToken(source["userSettings"], "value")) parsed.userSettingsPolicy = V1UserSettingsPolicy::Value;
    else if (exactV1JsonToken(source["userSettings"], "unchanged")) parsed.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    else return false;

    const JsonObjectConst mode = source["mode"].as<JsonObjectConst>();
    if (!mode["policy"].is<const char*>()) return false;
    if (exactV1JsonToken(mode["policy"], "value")) {
        if (mode.size() != 2 || !mode["value"].is<int>() || mode["value"].as<int>() < 1 ||
            mode["value"].as<int>() > 3) return false;
        parsed.modePolicy = V1ModePolicy::Value;
        parsed.mode = static_cast<uint8_t>(mode["value"].as<int>());
    } else if (exactV1JsonToken(mode["policy"], "unchanged")) {
        if (mode.size() != 1) return false;
    } else return false;

    if (exactV1JsonToken(source["display"], "on")) parsed.displayPolicy = V1DisplayPolicy::On;
    else if (exactV1JsonToken(source["display"], "off")) parsed.displayPolicy = V1DisplayPolicy::Off;
    else if (!exactV1JsonToken(source["display"], "unchanged")) return false;

    const JsonObjectConst volume = source["volume"].as<JsonObjectConst>();
    if (!volume["policy"].is<const char*>()) return false;
    const bool volumeTemporary = exactV1JsonToken(volume["policy"], "temporary");
    const bool volumeSaved = exactV1JsonToken(volume["policy"], "saved");
    if (volumeTemporary || volumeSaved) {
        if (volume.size() != 5 || !volume["main"].is<int>() || !volume["muted"].is<int>() ||
            !volume["feedback"].is<const char*>() || !volume["disconnect"].is<const char*>() ||
            volume["main"].as<int>() < 0 || volume["main"].as<int>() > 9 ||
            volume["muted"].as<int>() < 0 || volume["muted"].as<int>() > 9) return false;
        parsed.volumePolicy = volumeSaved ? V1VolumePolicy::Saved : V1VolumePolicy::Temporary;
        parsed.mainVolume = static_cast<uint8_t>(volume["main"].as<int>());
        parsed.mutedVolume = static_cast<uint8_t>(volume["muted"].as<int>());
        if (exactV1JsonToken(volume["feedback"], "none")) parsed.volumeFeedback = V1VolumeFeedbackPolicy::None;
        else if (exactV1JsonToken(volume["feedback"], "changed_only")) parsed.volumeFeedback = V1VolumeFeedbackPolicy::ChangedOnly;
        else if (exactV1JsonToken(volume["feedback"], "always")) parsed.volumeFeedback = V1VolumeFeedbackPolicy::Always;
        else return false;
        if (exactV1JsonToken(volume["disconnect"], "restore_saved")) parsed.volumeDisconnect = V1VolumeDisconnectPolicy::RestoreSaved;
        else if (exactV1JsonToken(volume["disconnect"], "keep_current")) parsed.volumeDisconnect = V1VolumeDisconnectPolicy::KeepCurrent;
        else return false;
        if (parsed.volumePolicy == V1VolumePolicy::Saved &&
            parsed.volumeDisconnect != V1VolumeDisconnectPolicy::RestoreSaved) return false;
    } else if (exactV1JsonToken(volume["policy"], "unchanged")) {
        if (volume.size() != 1) return false;
    } else return false;

    if (exactV1JsonToken(source["bluetoothLed"], "on")) parsed.bluetoothLedPolicy = V1BluetoothLedPolicy::On;
    else if (exactV1JsonToken(source["bluetoothLed"], "off")) parsed.bluetoothLedPolicy = V1BluetoothLedPolicy::Off;
    else if (!exactV1JsonToken(source["bluetoothLed"], "unchanged")) return false;

    const JsonObjectConst custom = source["customFrequencies"].as<JsonObjectConst>();
    if (!custom["policy"].is<const char*>()) return false;
    if (exactV1JsonToken(custom["policy"], "value")) {
        if (custom.size() != 2 || !custom["definitions"].is<JsonArrayConst>()) return false;
        const JsonArrayConst definitions = custom["definitions"].as<JsonArrayConst>();
        if (definitions.size() == 0 || definitions.size() > 64) return false;
        parsed.customFrequencyPolicy = V1CustomFrequencyPolicy::Value;
        uint8_t expectedIndex = 0;
        for (JsonObjectConst item : definitions) {
            if (item.size() != 3 || !item["index"].is<int>() || !item["lowerMHz"].is<int>() ||
                !item["upperMHz"].is<int>() || item["index"].as<int>() != expectedIndex ||
                item["lowerMHz"].as<int>() < 0 || item["lowerMHz"].as<int>() > 65535 ||
                item["upperMHz"].as<int>() < 0 || item["upperMHz"].as<int>() > 65535) return false;
            V1CustomFrequencyDefinition definition;
            definition.index = expectedIndex++;
            definition.lowerMHz = static_cast<uint16_t>(item["lowerMHz"].as<int>());
            definition.upperMHz = static_cast<uint16_t>(item["upperMHz"].as<int>());
            const bool unused = definition.lowerMHz == 0 && definition.upperMHz == 0;
            if ((definition.lowerMHz == 0) != (definition.upperMHz == 0) ||
                (!unused && definition.lowerMHz >= definition.upperMHz)) return false;
            if (!parsed.customFrequencyDefinitions.push_back(definition)) return false;
        }
    } else if (exactV1JsonToken(custom["policy"], "unchanged")) {
        if (custom.size() != 1) return false;
    } else return false;

    config = parsed;
    return true;
}

enum class ProfileStorageStatus : uint8_t {
    Success = 0,
    NotFound,
    Busy,
    IoError,
    Corrupt,
    InvalidName,
};

#ifdef UNIT_TEST
enum class V1ProfileAllocationFailurePoint : uint8_t {
    None = 0,
    ListGrowth,
    PageGrowth,
    SnapshotGrowth,
};
#endif

struct ProfileOperationResult {
    ProfileStorageStatus status = ProfileStorageStatus::IoError;
    String error;

    bool success() const { return status == ProfileStorageStatus::Success; }
};

struct ProfileListResult : ProfileOperationResult {
    std::vector<String> profiles;
    bool genuinelyEmpty = false;
};

struct ProfilePageResult : ProfileOperationResult {
    std::vector<String> profiles;
    size_t total = 0;
    bool hasMore = false;
    String nextCursor;
};

// Save result with detailed error info
struct ProfileSaveResult {
    bool success;
    ProfileStorageStatus status;
    String error; // Empty if success, detailed message if failed

    ProfileSaveResult() : success(false), status(ProfileStorageStatus::IoError), error("") {}
    ProfileSaveResult(bool ok)
        : success(ok), status(ok ? ProfileStorageStatus::Success : ProfileStorageStatus::IoError), error("") {}
    ProfileSaveResult(ProfileStorageStatus resultStatus, const String& err = "")
        : success(resultStatus == ProfileStorageStatus::Success), status(resultStatus), error(err) {}
    ProfileSaveResult(bool ok, const String& err)
        : success(ok), status(ok ? ProfileStorageStatus::Success : ProfileStorageStatus::IoError), error(err) {}
};

class V1ProfileManager {
  public:
    V1ProfileManager();

    // Initialize with the owning storage boundary. The fs overload remains for native tests.
    bool begin(StorageManager& storage);
    bool begin(fs::FS* filesystem, fs::FS* importFilesystem = nullptr);
    bool isReady() const { return ready_; }

    // Profile CRUD
    std::vector<String> listProfiles() const;
    ProfileListResult listProfilesResult(uint32_t timeoutMs = 0) const;
    ProfilePageResult listProfilesPageResult(const String& after, size_t limit,
                                             uint32_t timeoutMs = 0) const;
    bool loadProfile(const String& name, V1Profile& profile) const;
    ProfileOperationResult loadProfileResult(const String& name, V1Profile& profile, uint32_t timeoutMs = 0) const;
    ProfileSaveResult saveProfile(const V1Profile& profile);
    bool deleteProfile(const String& name);
    ProfileOperationResult deleteProfileResult(const String& name, uint32_t timeoutMs = 250);
    // Transaction rollback only: restore the just-deleted member of a legacy
    // grandfathered over-cap catalog without opening a general add bypass.
    ProfileSaveResult restoreProfileForTransaction(const V1Profile& profile);
    ProfileOperationResult snapshotProfiles(std::vector<V1Profile>& profiles, uint32_t timeoutMs = 0,
                                            bool allowGrandfatheredOverLimit = false) const;
    uint32_t catalogRevision() const { return catalogRevisionCounter_; }
#ifdef UNIT_TEST
    void utFailAllocation(V1ProfileAllocationFailurePoint point, size_t occurrence = 1,
                          bool repeat = false);
#endif

    // Get last error message
    const String& getLastError() const { return lastError_; }

    // Current V1 settings (from last pull)
    bool hasCurrentSettings() const { return currentValid_; }
    const V1UserSettings& getCurrentSettings() const { return currentSettings_; }
    void setCurrentSettings(const uint8_t* bytes);

    // JSON serialization for web API
    bool profileToJson(const V1Profile& profile, String& output) const;
    String profileToJson(const V1Profile& profile) const;
    String settingsToJson(const V1UserSettings& settings) const;
    bool settingsToJson(const V1UserSettings& settings, String& output) const;
    bool jsonToSettings(const String& json, V1UserSettings& settings) const;
    bool jsonToSettings(const JsonObject& settingsObj, V1UserSettings& settings) const;

  private:
    fs::FS* fs_;
    fs::FS* secondaryFs_;
    StorageManager* storage_;
    bool usingSd_;
    bool ready_;
    String profileDir_;
    mutable String lastError_; // Last error message for detailed reporting
    uint32_t catalogRevisionCounter_ = 1;
#ifdef UNIT_TEST
    mutable V1ProfileAllocationFailurePoint allocationFailurePoint_ =
        V1ProfileAllocationFailurePoint::None;
    mutable size_t allocationFailureCountdown_ = 0;
    mutable bool repeatAllocationFailure_ = false;
    void maybeFailAllocationForTest(V1ProfileAllocationFailurePoint point) const;
#endif

    V1UserSettings currentSettings_;
    bool currentValid_;

    String profilePath(const String& canonicalName) const;
    static uint32_t calculateCRC32(const uint8_t* data, size_t length);
    void bumpCatalogRevision();

    // Startup recovery for interrupted saves
    ProfileOperationResult loadProfileUnlocked(const String& canonicalName, V1Profile& profile,
                                               bool allowTransactionRecovery = true,
                                               bool verifyCandidateOwnedBySave = false) const;
    ProfileListResult listProfilesUnlocked() const;
    ProfileSaveResult saveProfileUnlocked(const V1Profile& profile, const String& canonicalName,
                                          bool allowGrandfatheredRestore = false);
    ProfileOperationResult deleteProfileUnlocked(const String& canonicalName);
    bool recoverInterruptedSavesUnlocked();
    size_t reconcileProfilesFrom(fs::FS* sourceFs);
};

#endif // V1_PROFILES_H
