/**
 * V1 Profile Manager
 * Stores and manages V1 Gen2 user settings profiles on SD card
 */

#pragma once
#ifndef V1_PROFILES_H
#define V1_PROFILES_H

#include <Arduino.h>
#include <FS.h>
#include <vector>
#include <ArduinoJson.h>

// V1 Gen2 User Settings (6 bytes)
// V1 protocol summary: docs/V1_PROTOCOL_REFERENCES.md#user-settings-bytes.
struct V1UserSettings {
    uint8_t bytes[6];

    // Byte 0 bits
    bool xBandEnabled() const { return bytes[0] & 0x01; }
    bool kBandEnabled() const { return bytes[0] & 0x02; }
    bool kaBandEnabled() const { return bytes[0] & 0x04; }
    bool laserEnabled() const { return bytes[0] & 0x08; }
    bool muteToMuteVolume() const { return !(bytes[0] & 0x10); }  // Inverted: bit clear = MZ enabled
    bool bogeyLockLoud() const { return bytes[0] & 0x20; }
    bool muteXKRear() const { return !(bytes[0] & 0x40); }  // Inverted
    bool kuBandEnabled() const { return !(bytes[0] & 0x80); }  // Inverted

    // Byte 1 bits
    bool euroMode() const { return !(bytes[1] & 0x01); }  // Inverted
    bool kVerifier() const { return bytes[1] & 0x02; }  // TMF
    bool laserRear() const { return bytes[1] & 0x04; }
    bool customFreqs() const { return !(bytes[1] & 0x08); }  // Inverted
    bool kaAlwaysPriority() const { return !(bytes[1] & 0x10); }  // Inverted
    bool fastLaserDetect() const { return bytes[1] & 0x20; }
    uint8_t kaSensitivity() const { return (bytes[1] >> 6) & 0x03; }  // 3=Full, 2=Original, 1=Relaxed

    // Byte 2 bits
    bool startupSequence() const { return bytes[2] & 0x01; }
    bool restingDisplay() const { return bytes[2] & 0x02; }
    bool bsmPlus() const { return !(bytes[2] & 0x04); }  // Inverted
    uint8_t autoMute() const { return (bytes[2] >> 3) & 0x03; }  // 3=Off, 1=On, 2=Advanced
    uint8_t kSensitivity() const { return (bytes[2] >> 5) & 0x03; }  // 3=Original, 2=Full, 1=Relaxed
    bool mrct() const { return !(bytes[2] & 0x80); }  // Inverted

    // Byte 3 bits
    uint8_t xSensitivity() const { return bytes[3] & 0x03; }  // 3=Original, 2=Full, 1=Relaxed
    bool driveSafe3D() const { return !(bytes[3] & 0x04); }  // Inverted
    bool driveSafe3DHD() const { return !(bytes[3] & 0x08); }  // Inverted
    bool redflexHalo() const { return !(bytes[3] & 0x10); }  // Inverted
    bool redflexNK7() const { return !(bytes[3] & 0x20); }  // Inverted
    bool ekin() const { return !(bytes[3] & 0x40); }  // Inverted
    bool photoVerifier() const { return !(bytes[3] & 0x80); }  // Inverted

    // Setters
    void setXBandEnabled(bool v) { if (v) bytes[0] |= 0x01; else bytes[0] &= ~0x01; }
    void setKBandEnabled(bool v) { if (v) bytes[0] |= 0x02; else bytes[0] &= ~0x02; }
    void setKaBandEnabled(bool v) { if (v) bytes[0] |= 0x04; else bytes[0] &= ~0x04; }
    void setLaserEnabled(bool v) { if (v) bytes[0] |= 0x08; else bytes[0] &= ~0x08; }
    void setMuteToMuteVolume(bool v) { if (v) bytes[0] &= ~0x10; else bytes[0] |= 0x10; }  // Inverted
    void setBogeyLockLoud(bool v) { if (v) bytes[0] |= 0x20; else bytes[0] &= ~0x20; }
    void setMuteXKRear(bool v) { if (v) bytes[0] &= ~0x40; else bytes[0] |= 0x40; }  // Inverted
    void setKuBandEnabled(bool v) { if (v) bytes[0] &= ~0x80; else bytes[0] |= 0x80; }  // Inverted

    void setEuroMode(bool v) { if (v) bytes[1] &= ~0x01; else bytes[1] |= 0x01; }  // Inverted
    void setKVerifier(bool v) { if (v) bytes[1] |= 0x02; else bytes[1] &= ~0x02; }
    void setLaserRear(bool v) { if (v) bytes[1] |= 0x04; else bytes[1] &= ~0x04; }
    void setCustomFreqs(bool v) { if (v) bytes[1] &= ~0x08; else bytes[1] |= 0x08; }  // Inverted
    void setKaAlwaysPriority(bool v) { if (v) bytes[1] &= ~0x10; else bytes[1] |= 0x10; }  // Inverted
    void setFastLaserDetect(bool v) { if (v) bytes[1] |= 0x20; else bytes[1] &= ~0x20; }
    void setKaSensitivity(uint8_t v) { bytes[1] = (bytes[1] & 0x3F) | ((v & 0x03) << 6); }

    void setStartupSequence(bool v) { if (v) bytes[2] |= 0x01; else bytes[2] &= ~0x01; }
    void setRestingDisplay(bool v) { if (v) bytes[2] |= 0x02; else bytes[2] &= ~0x02; }
    void setBsmPlus(bool v) { if (v) bytes[2] &= ~0x04; else bytes[2] |= 0x04; }  // Inverted
    void setAutoMute(uint8_t v) { bytes[2] = (bytes[2] & 0xE7) | ((v & 0x03) << 3); }
    void setKSensitivity(uint8_t v) { bytes[2] = (bytes[2] & 0x9F) | ((v & 0x03) << 5); }
    void setMrct(bool v) { if (v) bytes[2] &= ~0x80; else bytes[2] |= 0x80; }  // Inverted

    void setXSensitivity(uint8_t v) { bytes[3] = (bytes[3] & 0xFC) | (v & 0x03); }
    void setDriveSafe3D(bool v) { if (v) bytes[3] &= ~0x04; else bytes[3] |= 0x04; }  // Inverted
    void setDriveSafe3DHD(bool v) { if (v) bytes[3] &= ~0x08; else bytes[3] |= 0x08; }  // Inverted
    void setRedflexHalo(bool v) { if (v) bytes[3] &= ~0x10; else bytes[3] |= 0x10; }  // Inverted
    void setRedflexNK7(bool v) { if (v) bytes[3] &= ~0x20; else bytes[3] |= 0x20; }  // Inverted
    void setEkin(bool v) { if (v) bytes[3] &= ~0x40; else bytes[3] |= 0x40; }  // Inverted
    void setPhotoVerifier(bool v) { if (v) bytes[3] &= ~0x80; else bytes[3] |= 0x80; }  // Inverted

    // Initialize to factory defaults (all 0xFF)
    void setDefaults() {
        memset(bytes, 0xFF, 6);
    }

    V1UserSettings() {
        setDefaults();
    }
};

// Profile with name and settings
struct V1Profile {
    String name;
    String description;
    V1UserSettings settings;
    bool displayOn;       // V1 main display on/off (dark mode)
    uint8_t mainVolume;   // Main volume 0-9 (0xFF = don't change)
    uint8_t mutedVolume;  // Muted volume 0-9 (0xFF = don't change)

    V1Profile() : name("Default"), description(""), displayOn(true), mainVolume(0xFF), mutedVolume(0xFF) {}
    V1Profile(const String& n) : name(n), description(""), displayOn(true), mainVolume(0xFF), mutedVolume(0xFF) {}
    V1Profile(const String& n, const V1UserSettings& s) : name(n), description(""), settings(s), displayOn(true), mainVolume(0xFF), mutedVolume(0xFF) {}
};

// Save result with detailed error info
struct ProfileSaveResult {
    bool success;
    String error;  // Empty if success, detailed message if failed

    ProfileSaveResult() : success(false), error("") {}
    ProfileSaveResult(bool ok) : success(ok), error("") {}
    ProfileSaveResult(bool ok, const String& err) : success(ok), error(err) {}
};

class V1ProfileManager {
public:
    V1ProfileManager();

    // Initialize with filesystem
    bool begin(fs::FS* filesystem, fs::FS* importFilesystem = nullptr);
    bool isReady() const { return ready_; }

    // Profile CRUD
    std::vector<String> listProfiles() const;
    bool loadProfile(const String& name, V1Profile& profile) const;
    ProfileSaveResult saveProfile(const V1Profile& profile);
    bool deleteProfile(const String& name);
    bool renameProfile(const String& oldName, const String& newName);
    uint32_t catalogRevision() const { return catalogRevisionCounter_; }

    // Get last error message
    const String& getLastError() const { return lastError_; }

    // Current V1 settings (from last pull)
    bool hasCurrentSettings() const { return currentValid_; }
    const V1UserSettings& getCurrentSettings() const { return currentSettings_; }
    void setCurrentSettings(const uint8_t* bytes);

    // JSON serialization for web API
    String profileToJson(const V1Profile& profile) const;
    String settingsToJson(const V1UserSettings& settings) const;
    bool jsonToSettings(const String& json, V1UserSettings& settings) const;
    bool jsonToSettings(const JsonObject& settingsObj, V1UserSettings& settings) const;

private:
    fs::FS* fs_;
    bool ready_;
    String profileDir_;
    mutable String lastError_;  // Last error message for detailed reporting
    uint32_t catalogRevisionCounter_ = 1;

    V1UserSettings currentSettings_;
    bool currentValid_;

    String profilePath(const String& name) const;
    static uint32_t calculateCRC32(const uint8_t* data, size_t length);
    void bumpCatalogRevision();

    // Startup recovery for interrupted saves
    void recoverInterruptedSaves();
    size_t migrateProfilesFrom(fs::FS* sourceFs);
};

// Global instance
extern V1ProfileManager v1ProfileManager;
#endif  // V1_PROFILES_H
