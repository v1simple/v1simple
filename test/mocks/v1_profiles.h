#pragma once
#ifndef V1_PROFILES_H
#define V1_PROFILES_H

#include <Arduino.h>
#include <ArduinoJson.h>
#include <cstdint>
#include <cstring>

struct V1UserSettings {
    uint8_t bytes[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
};

inline constexpr uint8_t V1_PROFILE_SCHEMA_VERSION = 2;
enum class V1UserSettingsPolicy : uint8_t { Unchanged = 0, Value = 1 };
enum class V1ModePolicy : uint8_t { Unchanged = 0, Value = 1 };
enum class V1DisplayPolicy : uint8_t { Unchanged = 0, On = 1, Off = 2 };
enum class V1VolumePolicy : uint8_t { Unchanged = 0, Temporary = 1, Saved = 2 };
enum class V1BluetoothLedPolicy : uint8_t { Unchanged = 0 };
enum class V1CustomFrequencyPolicy : uint8_t { Unchanged = 0 };
struct V1DetectorConfiguration {
    V1UserSettingsPolicy userSettingsPolicy = V1UserSettingsPolicy::Value;
    V1ModePolicy modePolicy = V1ModePolicy::Unchanged;
    uint8_t mode = 0;
    V1DisplayPolicy displayPolicy = V1DisplayPolicy::Unchanged;
    V1VolumePolicy volumePolicy = V1VolumePolicy::Unchanged;
    uint8_t mainVolume = 0;
    uint8_t mutedVolume = 0;
    V1BluetoothLedPolicy bluetoothLedPolicy = V1BluetoothLedPolicy::Unchanged;
    V1CustomFrequencyPolicy customFrequencyPolicy = V1CustomFrequencyPolicy::Unchanged;
};

inline bool parseV1DetectorConfiguration(JsonObjectConst source, V1DetectorConfiguration& config) {
    if (source.isNull() || source.size() != 6 || !source["userSettings"].is<const char*>() ||
        !source["mode"].is<JsonObjectConst>() || !source["display"].is<const char*>() ||
        !source["volume"].is<JsonObjectConst>() ||
        String(source["bluetoothLed"] | "") != "unchanged" ||
        String(source["customFrequencies"] | "") != "unchanged") return false;
    V1DetectorConfiguration parsed;
    const String users = source["userSettings"].as<const char*>();
    if (users == "unchanged") parsed.userSettingsPolicy = V1UserSettingsPolicy::Unchanged;
    else if (users != "value") return false;
    const JsonObjectConst mode = source["mode"].as<JsonObjectConst>();
    const String modePolicy = mode["policy"] | "";
    if (modePolicy == "value") {
        if (mode.size() != 2 || !mode["value"].is<int>() || mode["value"].as<int>() < 1 || mode["value"].as<int>() > 3) return false;
        parsed.modePolicy = V1ModePolicy::Value;
        parsed.mode = mode["value"].as<uint8_t>();
    } else if (modePolicy != "unchanged" || mode.size() != 1) return false;
    const String display = source["display"] | "";
    if (display == "on") parsed.displayPolicy = V1DisplayPolicy::On;
    else if (display == "off") parsed.displayPolicy = V1DisplayPolicy::Off;
    else if (display != "unchanged") return false;
    const JsonObjectConst volume = source["volume"].as<JsonObjectConst>();
    const String volumePolicy = volume["policy"] | "";
    if (volumePolicy == "temporary" || volumePolicy == "saved") {
        if (volume.size() != 3 || !volume["main"].is<int>() || !volume["muted"].is<int>() ||
            volume["main"].as<int>() < 0 || volume["main"].as<int>() > 9 ||
            volume["muted"].as<int>() < 0 || volume["muted"].as<int>() > 9) return false;
        parsed.volumePolicy = volumePolicy == "saved" ? V1VolumePolicy::Saved : V1VolumePolicy::Temporary;
        parsed.mainVolume = volume["main"].as<uint8_t>();
        parsed.mutedVolume = volume["muted"].as<uint8_t>();
    } else if (volumePolicy != "unchanged" || volume.size() != 1) return false;
    config = parsed;
    return true;
}

struct V1Profile {
    String name;
    String description;
    V1UserSettings settings;
    V1DetectorConfiguration detector;
    uint8_t schemaVersion = V1_PROFILE_SCHEMA_VERSION;
    bool displayOn = true;
    uint8_t mainVolume = 0xFF;
    uint8_t mutedVolume = 0xFF;
};

enum class ProfileStorageStatus : uint8_t { Success = 0, NotFound, Busy, IoError, Corrupt, InvalidName };

struct ProfileOperationResult {
    ProfileStorageStatus status = ProfileStorageStatus::IoError;
    bool success() const { return status == ProfileStorageStatus::Success; }
};

class V1ProfileManager {
public:
    int setCurrentSettingsCalls = 0;
    mutable int loadProfileCalls = 0;
    uint8_t lastSettings[6] = {};
    bool loadProfileSuccess = false;
    ProfileStorageStatus nextLoadStatus = ProfileStorageStatus::NotFound;
    String loadableProfileName;
    V1Profile loadableProfile;
    mutable String lastLoadProfileName;

    void reset() {
        setCurrentSettingsCalls = 0;
        loadProfileCalls = 0;
        std::memset(lastSettings, 0, sizeof(lastSettings));
        loadProfileSuccess = false;
        nextLoadStatus = ProfileStorageStatus::NotFound;
        loadableProfileName = "";
        loadableProfile = V1Profile{};
        lastLoadProfileName = "";
    }

    void setCurrentSettings(const uint8_t* bytes) {
        setCurrentSettingsCalls++;
        if (bytes) {
            std::memcpy(lastSettings, bytes, sizeof(lastSettings));
        }
    }

    bool loadProfile(const String& name, V1Profile& profile) const {
        loadProfileCalls++;
        lastLoadProfileName = name;
        if (!loadProfileSuccess) {
            return false;
        }
        if (loadableProfileName.length() > 0 && loadableProfileName != name) {
            return false;
        }
        profile = loadableProfile;
        return true;
    }

    ProfileOperationResult loadProfileResult(const String& name, V1Profile& profile, uint32_t = 0) const {
        if (nextLoadStatus == ProfileStorageStatus::Busy) {
            loadProfileCalls++;
            lastLoadProfileName = name;
            return ProfileOperationResult{ProfileStorageStatus::Busy};
        }
        const bool ok = loadProfile(name, profile);
        return ProfileOperationResult{ok ? ProfileStorageStatus::Success : nextLoadStatus};
    }
};

#endif  // V1_PROFILES_H
