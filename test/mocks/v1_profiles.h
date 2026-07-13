#pragma once
#ifndef V1_PROFILES_H
#define V1_PROFILES_H

#include <Arduino.h>
#include <cstdint>
#include <cstring>

struct V1UserSettings {
    uint8_t bytes[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
};

struct V1Profile {
    String name;
    String description;
    V1UserSettings settings;
    bool displayOn = true;
    uint8_t mainVolume = 0xFF;
    uint8_t mutedVolume = 0xFF;
};

class V1ProfileManager {
public:
    int setCurrentSettingsCalls = 0;
    mutable int loadProfileCalls = 0;
    uint8_t lastSettings[6] = {};
    bool loadProfileResult = false;
    String loadableProfileName;
    V1Profile loadableProfile;
    mutable String lastLoadProfileName;

    void reset() {
        setCurrentSettingsCalls = 0;
        loadProfileCalls = 0;
        std::memset(lastSettings, 0, sizeof(lastSettings));
        loadProfileResult = false;
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
        if (!loadProfileResult) {
            return false;
        }
        if (loadableProfileName.length() > 0 && loadableProfileName != name) {
            return false;
        }
        profile = loadableProfile;
        return true;
    }
};

#endif  // V1_PROFILES_H
