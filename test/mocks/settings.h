#pragma once
#ifndef SETTINGS_H
#define SETTINGS_H

#ifdef ARDUINO
#include <Arduino.h>
#else
#include "Arduino.h"
#endif
#include <cstdint>

// Minimal display font enum (for compatibility with older tests)
enum FontStyle : uint8_t {
    FONT_STYLE_CLASSIC = 0,
    FONT_STYLE_MODERN = 1,
    FONT_STYLE_HEMI = 2
};

enum V1Mode : uint8_t {
    V1_MODE_UNKNOWN = 0x00,
    V1_MODE_ALL_BOGEYS = 0x01,
    V1_MODE_LOGIC = 0x02,
    V1_MODE_ADVANCED_LOGIC = 0x03
};

struct AutoPushSlot {
    String profileName;
    V1Mode mode = V1_MODE_UNKNOWN;
};

// Voice alert content mode
enum VoiceAlertMode : uint8_t {
    VOICE_MODE_DISABLED = 0,
    VOICE_MODE_BAND_ONLY = 1,
    VOICE_MODE_FREQ_ONLY = 2,
    VOICE_MODE_BAND_FREQ = 3
};

// Mocked settings structure (superset of fields used in modules/tests)
struct V1Settings {
    // Display
    uint8_t brightness = 128;
    bool displayOn = true;
    FontStyle fontStyle = FONT_STYLE_CLASSIC;
    // Colors (kept for older display tests)
    uint16_t colorX = 0x07E0;
    uint16_t colorK = 0x07FF;
    uint16_t colorKa = 0xF800;
    uint16_t colorLaser = 0xFFFF;
    uint16_t colorPhoto = 0xF81F;
    uint16_t colorMuted = 0x8410;
    uint16_t colorBogey = 0xFFE0;
    uint16_t colorPersisted = 0x18C3;
    uint16_t colorFrequency = 0xFFFF;
    bool freqUseBandColor = true;
    // Band indicator colors (match real settings.h defaults)
    uint16_t colorBandL = 0x001F;    // Blue (laser)
    uint16_t colorBandKa = 0xF800;   // Red
    uint16_t colorBandK = 0x001F;    // Blue
    uint16_t colorBandX = 0x07E0;    // Green
    uint16_t colorBandPhoto = 0xFD20;
    // Arrow colors
    uint16_t colorArrowFront = 0xF800;  // Red
    uint16_t colorArrowSide  = 0xF800;  // Red
    uint16_t colorArrowRear  = 0xF800;  // Red
    // Signal bar colors
    uint16_t colorBar1 = 0x07E0;  // Green (weakest)
    uint16_t colorBar2 = 0x07E0;  // Green
    uint16_t colorBar3 = 0xFFE0;  // Yellow
    uint16_t colorBar4 = 0xFFE0;  // Yellow
    uint16_t colorBar5 = 0xF800;  // Red
    uint16_t colorBar6 = 0xF800;  // Red (strongest)
    // Indicator badge colors
    uint16_t colorObd        = 0x001F;  // Blue OBD badge
    uint16_t colorAlpConnected = 0x07E0;  // Green ALP connected
    uint16_t colorAlpDli        = 0xFD20;  // Orange ALP DLI active (below LID speed limit)
    uint16_t colorAlpLidActive  = 0x001F;  // Blue ALP LID active (above LID speed limit)
    uint16_t colorAlpAlert     = 0xF800;  // Red ALP alert (solid during session)
    // Volume/RSSI indicator colors
    uint16_t colorVolumeMain = 0xF800;  // Red main volume
    uint16_t colorVolumeMute = 0x7BEF;  // Grey muted volume
    uint16_t colorRssiV1     = 0x07E0;  // Green V1 RSSI label
    uint16_t colorRssiProxy  = 0x001F;  // Blue proxy RSSI label
    // BLE icon colors
    uint16_t colorBleConnected    = 0x07E0;  // Green
    uint16_t colorBleDisconnected = 0x001F;  // Blue
    // Visibility flags
    bool hideProfileIndicator = false;
    bool hideBatteryIcon      = false;
    bool showBatteryPercent   = false;
    bool hideBleIcon          = false;
    bool hideVolumeIndicator  = false;
    bool hideRssiIndicator    = false;

    // Voice alerts
    uint8_t voiceVolume = 75;
    VoiceAlertMode voiceAlertMode = VOICE_MODE_BAND_FREQ;
    bool voiceDirectionEnabled = true;
    bool announceBogeyCount = true;
    bool muteVoiceIfVolZero = true;

    // Secondary alert announcements
    bool announceSecondaryAlerts = true;
    bool secondaryLaser = true;
    bool secondaryKa = true;
    bool secondaryK = true;
    bool secondaryX = true;

    // Profile slot colors/names
    String slot0Name = "DEFAULT";
    String slot1Name = "HIGHWAY";
    String slot2Name = "COMFORT";
    uint16_t slot0Color = 0x400A;
    uint16_t slot1Color = 0xFFE0;
    uint16_t slot2Color = 0x07E0;
    // WiFi icon colors
    uint16_t colorWiFiIcon      = 0x07FF;  // Cyan
    uint16_t colorWiFiConnected = 0x07E0;  // Green
    bool hideWifiIcon = false;

    // V1 volume / quiet controls
    uint8_t volume = 5;

    // Volume fade
    bool alertVolumeFadeEnabled = false;
    uint8_t alertVolumeFadeDelaySec = 5;
    uint8_t alertVolumeFadeVolume = 3;
    
    // Speed mute
    bool speedMuteEnabled = false;
    uint8_t speedMuteThresholdMph = 25;
    uint8_t speedMuteHysteresisMph = 3;
    uint8_t speedMuteVolume = 0;
    bool speedMuteVoice = true;
    bool stealthEnabled = false;

    // Misc flags retained for compatibility
    bool obdEnabled = false;
    bool gpsEnabled = false;
    uint8_t autoPowerOffMinutes = 10;
    String obdSavedAddress = "";
    String obdSavedName = "";
    uint8_t obdSavedAddrType = 0;
    int8_t obdMinRssi = -90;
    uint32_t obdScanWindowMs = 15000;
    uint32_t obdRetryIntervalMs = 120000;
    uint32_t proxyOpenWindowMs = 60000;
    uint32_t wifiOpenTimeoutMs = 30000;
    uint32_t v1SettleQuietMs = 500;
    uint32_t v1SettleFallbackMs = 1500;
    uint32_t cycleTeardownAckTimeoutMs = 100;
    bool bleProxyEnabled = true;
    uint8_t activeSlot = 0;
    bool autoPushEnabled = false;
    bool powerOffSdLog = false;
    // Whether ALP module is enabled — used by auto power-off logic.
    bool alpEnabled = false;
    bool alpSdLogEnabled = false;
    uint8_t alpAlertPersistSec = 0;
    bool alpDisableV1LaserOnPush = true;

};

enum class SettingsPersistMode : uint8_t {
    Immediate,
    ImmediateNvsDeferredBackup,
    Deferred,
};

struct ObdSettingsUpdate {
    bool hasEnabled = false;
    bool enabled = false;
    bool hasMinRssi = false;
    int8_t minRssi = -90;
    bool hasObdScanWindowMs = false;
    uint32_t obdScanWindowMs = 0;
    bool hasObdRetryIntervalMs = false;
    uint32_t obdRetryIntervalMs = 0;
    bool hasProxyOpenWindowMs = false;
    uint32_t proxyOpenWindowMs = 0;
    bool hasWifiOpenTimeoutMs = false;
    uint32_t wifiOpenTimeoutMs = 0;
    bool hasV1SettleQuietMs = false;
    uint32_t v1SettleQuietMs = 0;
    bool hasV1SettleFallbackMs = false;
    uint32_t v1SettleFallbackMs = 0;
    bool hasCycleTeardownAckTimeoutMs = false;
    uint32_t cycleTeardownAckTimeoutMs = 0;
    bool hasSavedAddress = false;
    String savedAddress;
    bool hasSavedName = false;
    String savedName;
    bool hasSavedAddrType = false;
    uint8_t savedAddrType = 0;
    bool resetSavedNameOnAddressChange = false;
};

struct AutoPushSlotUpdate {
    int slot = 0;
    bool hasName = false;
    String name;
    bool hasColor = false;
    uint16_t color = 0;
    bool hasVolume = false;
    uint8_t volume = 0xFF;
    bool hasMuteVolume = false;
    uint8_t muteVolume = 0xFF;
    bool hasDarkMode = false;
    bool darkMode = false;
    bool hasMuteToZero = false;
    bool muteToZero = false;
    bool hasAlertPersist = false;
    uint8_t alertPersist = 0;
    bool hasPriorityArrowOnly = false;
    bool priorityArrowOnly = false;
    bool hasProfileName = false;
    String profileName;
    bool hasMode = false;
    V1Mode mode = V1_MODE_UNKNOWN;
};

struct AutoPushStateUpdate {
    bool hasActiveSlot = false;
    int activeSlot = 0;
    bool hasEnabled = false;
    bool enabled = false;
};

// Settings manager stub
class SettingsManager {
public:
    struct NvsDiagnostic {
        String activeNamespace;
        uint8_t nvsValidMarker = 0;
        uint8_t settingsVersion = 0;
        uint8_t nvsBrightness = 0;
        bool nvsProxyBle = false;
        bool nvsAutoPush = false;
        bool healthy = false;
    };

    V1Settings settings;
    int saveCalls = 0;
    int saveDeferredBackupCalls = 0;
    int requestDeferredPersistCalls = 0;
    int applyAutoPushSlotUpdateCalls = 0;
    int applyAutoPushStateUpdateCalls = 0;
    int backupToSDCalls = 0;
    int requestDeferredBackupCalls = 0;
    String slotNames[3];
    uint16_t slotColors[3] = {0, 0, 0};
    uint8_t slotAlertPersistSec[3] = {0, 0, 0};
    AutoPushSlot slotConfigs[3];
    uint8_t slotVolumes[3] = {0xFF, 0xFF, 0xFF};
    uint8_t slotMuteVolumes[3] = {0xFF, 0xFF, 0xFF};
    bool slotDarkModes[3] = {false, false, false};
    bool slotMuteToZero[3] = {false, false, false};
    bool slotPriorityArrowOnly[3] = {false, false, false};
    bool backupToSDResult = true;
    
    void load() {}
    void save() { ++saveCalls; }
    void updateBrightness(uint8_t brightness) { settings.brightness = brightness; }
    void updateVoiceVolume(uint8_t volume) { settings.voiceVolume = volume; }
    void saveDeferredBackup() { ++saveDeferredBackupCalls; }
    void requestDeferredPersist() { ++requestDeferredPersistCalls; }
    void serviceDeferredPersist(uint32_t) {}
    bool deferredPersistPending() const { return false; }
    bool deferredPersistRetryScheduled() const { return false; }
    uint32_t deferredPersistNextAttemptAtMs() const { return 0; }
    void setDefaults() {}
    bool backupToSD() {
        ++backupToSDCalls;
        return backupToSDResult;
    }
    void requestDeferredBackupFromCurrentState() { ++requestDeferredBackupCalls; }
    bool deferredBackupPending() const { return false; }
    bool deferredBackupRetryScheduled() const { return false; }
    uint32_t deferredBackupNextAttemptAtMs() const { return 0; }
    void setLastV1Address(const char*) {}
    void setActiveSlot(int slot) { settings.activeSlot = static_cast<uint8_t>(slot); }
    void setAutoPushEnabled(bool enabled) { settings.autoPushEnabled = enabled; }
    void setSlot(int slotNum, const String& profile, V1Mode mode) {
        if (slotNum < 0 || slotNum > 2) return;
        slotConfigs[slotNum].profileName = profile;
        slotConfigs[slotNum].mode = mode;
    }
    const AutoPushSlot& getSlot(int slotNum) const {
        static AutoPushSlot fallback;
        if (slotNum < 0 || slotNum > 2) return fallback;
        return slotConfigs[slotNum];
    }
    uint8_t getSlotVolume(int slotNum) const {
        return (slotNum >= 0 && slotNum < 3) ? slotVolumes[slotNum] : 0xFF;
    }
    uint8_t getSlotMuteVolume(int slotNum) const {
        return (slotNum >= 0 && slotNum < 3) ? slotMuteVolumes[slotNum] : 0xFF;
    }
    bool getSlotDarkMode(int slotNum) const {
        return (slotNum >= 0 && slotNum < 3) ? slotDarkModes[slotNum] : false;
    }
    bool getSlotMuteToZero(int slotNum) const {
        return (slotNum >= 0 && slotNum < 3) ? slotMuteToZero[slotNum] : false;
    }
    bool getSlotPriorityArrowOnly(int slotNum) const {
        return (slotNum >= 0 && slotNum < 3) ? slotPriorityArrowOnly[slotNum] : false;
    }
    void setSlotVolumes(int slotNum, uint8_t volume, uint8_t muteVolume) {
        if (slotNum < 0 || slotNum > 2) return;
        slotVolumes[slotNum] = volume;
        slotMuteVolumes[slotNum] = muteVolume;
    }
    void setSlotDarkMode(int slotNum, bool darkMode) {
        if (slotNum < 0 || slotNum > 2) return;
        slotDarkModes[slotNum] = darkMode;
    }
    void setSlotMuteToZero(int slotNum, bool enabled) {
        if (slotNum < 0 || slotNum > 2) return;
        slotMuteToZero[slotNum] = enabled;
    }
    
    const V1Settings& get() const { return settings; }
    V1Settings& getMutable() { return settings; }
    V1Settings& mutableSettings() { return settings; }
    bool applyObdSettingsUpdate(const ObdSettingsUpdate& update,
                                SettingsPersistMode persistMode = SettingsPersistMode::Immediate) {
        (void)persistMode;
        bool changed = false;
        if (update.resetSavedNameOnAddressChange &&
            update.hasSavedAddress &&
            settings.obdSavedAddress != update.savedAddress &&
            !update.hasSavedName) {
            settings.obdSavedName = "";
            changed = true;
        }
        if (update.hasEnabled && settings.obdEnabled != update.enabled) {
            settings.obdEnabled = update.enabled;
            changed = true;
        }
        if (update.hasMinRssi && settings.obdMinRssi != update.minRssi) {
            settings.obdMinRssi = update.minRssi;
            changed = true;
        }
        if (update.hasObdScanWindowMs && settings.obdScanWindowMs != update.obdScanWindowMs) {
            settings.obdScanWindowMs = update.obdScanWindowMs;
            changed = true;
        }
        if (update.hasObdRetryIntervalMs && settings.obdRetryIntervalMs != update.obdRetryIntervalMs) {
            settings.obdRetryIntervalMs = update.obdRetryIntervalMs;
            changed = true;
        }
        if (update.hasProxyOpenWindowMs && settings.proxyOpenWindowMs != update.proxyOpenWindowMs) {
            settings.proxyOpenWindowMs = update.proxyOpenWindowMs;
            changed = true;
        }
        if (update.hasWifiOpenTimeoutMs && settings.wifiOpenTimeoutMs != update.wifiOpenTimeoutMs) {
            settings.wifiOpenTimeoutMs = update.wifiOpenTimeoutMs;
            changed = true;
        }
        if (update.hasV1SettleQuietMs && settings.v1SettleQuietMs != update.v1SettleQuietMs) {
            settings.v1SettleQuietMs = update.v1SettleQuietMs;
            changed = true;
        }
        if (update.hasV1SettleFallbackMs &&
            settings.v1SettleFallbackMs != update.v1SettleFallbackMs) {
            settings.v1SettleFallbackMs = update.v1SettleFallbackMs;
            changed = true;
        }
        if (update.hasCycleTeardownAckTimeoutMs &&
            settings.cycleTeardownAckTimeoutMs != update.cycleTeardownAckTimeoutMs) {
            settings.cycleTeardownAckTimeoutMs = update.cycleTeardownAckTimeoutMs;
            changed = true;
        }
        if (update.hasSavedAddress && settings.obdSavedAddress != update.savedAddress) {
            settings.obdSavedAddress = update.savedAddress;
            changed = true;
        }
        if (update.hasSavedName && settings.obdSavedName != update.savedName) {
            settings.obdSavedName = update.savedName;
            changed = true;
        }
        if (update.hasSavedAddrType && settings.obdSavedAddrType != update.savedAddrType) {
            settings.obdSavedAddrType = update.savedAddrType;
            changed = true;
        }
        if (changed) {
            if (persistMode == SettingsPersistMode::Deferred) {
                ++requestDeferredPersistCalls;
            } else if (persistMode == SettingsPersistMode::ImmediateNvsDeferredBackup) {
                ++saveDeferredBackupCalls;
            } else {
                ++saveCalls;
            }
        }
        return changed;
    }
    bool applyAutoPushSlotUpdate(const AutoPushSlotUpdate& update,
                                 SettingsPersistMode persistMode = SettingsPersistMode::Immediate) {
        ++applyAutoPushSlotUpdateCalls;
        if (update.slot < 0 || update.slot > 2) {
            return false;
        }

        bool changed = false;
        if (update.hasName && slotNames[update.slot] != update.name) {
            slotNames[update.slot] = update.name;
            changed = true;
        }
        if (update.hasColor && slotColors[update.slot] != update.color) {
            slotColors[update.slot] = update.color;
            changed = true;
        }
        if (update.hasVolume && slotVolumes[update.slot] != update.volume) {
            slotVolumes[update.slot] = update.volume;
            changed = true;
        }
        if (update.hasMuteVolume && slotMuteVolumes[update.slot] != update.muteVolume) {
            slotMuteVolumes[update.slot] = update.muteVolume;
            changed = true;
        }
        if (update.hasDarkMode && slotDarkModes[update.slot] != update.darkMode) {
            slotDarkModes[update.slot] = update.darkMode;
            changed = true;
        }
        if (update.hasMuteToZero && slotMuteToZero[update.slot] != update.muteToZero) {
            slotMuteToZero[update.slot] = update.muteToZero;
            changed = true;
        }
        if (update.hasAlertPersist && slotAlertPersistSec[update.slot] != update.alertPersist) {
            slotAlertPersistSec[update.slot] = update.alertPersist;
            changed = true;
        }
        if (update.hasPriorityArrowOnly &&
            slotPriorityArrowOnly[update.slot] != update.priorityArrowOnly) {
            slotPriorityArrowOnly[update.slot] = update.priorityArrowOnly;
            changed = true;
        }
        if (update.hasProfileName && slotConfigs[update.slot].profileName != update.profileName) {
            slotConfigs[update.slot].profileName = update.profileName;
            changed = true;
        }
        if (update.hasMode && slotConfigs[update.slot].mode != update.mode) {
            slotConfigs[update.slot].mode = update.mode;
            changed = true;
        }

        if (changed) {
            if (persistMode == SettingsPersistMode::Deferred) {
                ++requestDeferredPersistCalls;
            } else if (persistMode == SettingsPersistMode::ImmediateNvsDeferredBackup) {
                ++saveDeferredBackupCalls;
            } else {
                ++saveCalls;
            }
        }
        return changed;
    }
    bool applyAutoPushStateUpdate(const AutoPushStateUpdate& update,
                                  SettingsPersistMode persistMode = SettingsPersistMode::Immediate) {
        ++applyAutoPushStateUpdateCalls;
        bool changed = false;
        if (update.hasActiveSlot && settings.activeSlot != update.activeSlot) {
            settings.activeSlot = static_cast<uint8_t>(update.activeSlot);
            changed = true;
        }
        if (update.hasEnabled && settings.autoPushEnabled != update.enabled) {
            settings.autoPushEnabled = update.enabled;
            changed = true;
        }
        if (changed) {
            if (persistMode == SettingsPersistMode::Deferred) {
                ++requestDeferredPersistCalls;
            } else if (persistMode == SettingsPersistMode::ImmediateNvsDeferredBackup) {
                ++saveDeferredBackupCalls;
            } else {
                ++saveCalls;
            }
        }
        return changed;
    }
    uint8_t getSlotAlertPersistSec(uint8_t slot) const {
        return (slot < 3) ? slotAlertPersistSec[slot] : 0;
    }

    // ALP-global persist — separate from per-slot V1 window. Defaults to 0 in
    // the mock to match production; tests that need the ALP latch tail set
    // this field directly.
    uint8_t alpAlertPersistSec = 0;
    uint8_t getAlpAlertPersistSec() const { return alpAlertPersistSec; }
    
    // Convenience helpers used by some tests
    uint8_t getBrightness() const { return settings.brightness; }
    void setBrightness(uint8_t b) { settings.brightness = b; }

    bool isDisplayOn() const { return settings.displayOn; }
    void setDisplayOn(bool on) { settings.displayOn = on; }

    void setStealthEnabled(bool enabled) { settings.stealthEnabled = enabled; save(); }
};

// Global settings instance
extern SettingsManager settingsManager;

#endif // SETTINGS_H
