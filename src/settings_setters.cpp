/**
 * Settings property setters, slot getters/setters, and reset.
 * Extracted from settings_.cpp to reduce file size.
 */

#include "settings_internals.h"

namespace {

template <typename T> bool assignIfChanged(T& target, const T& value) {
    if (target == value) {
        return false;
    }
    target = value;
    return true;
}

void persistSettingsByMode(SettingsManager& manager, SettingsPersistMode persistMode) {
    if (persistMode == SettingsPersistMode::Deferred) {
        manager.requestDeferredPersist();
        return;
    }
    if (persistMode == SettingsPersistMode::ImmediateNvsDeferredBackup) {
        manager.saveDeferredBackup();
        return;
    }
    manager.save();
}

} // namespace

// --- Simple property setters ---

void SettingsManager::setWiFiEnabled(bool enabled) {
    settings_.enableWifi = enabled;
    save();
}

void SettingsManager::setAPCredentials(const String& ssid, const String& password) {
    settings_.apSSID = sanitizeApSsidValue(ssid);
    settings_.apPassword = sanitizeApPasswordValue(password);
    save();
}

void SettingsManager::setProxyBLE(bool enabled) {
    settings_.proxyBLE = enabled;
    if (enabled) {
        settings_.obdEnabled = false;
    }
    save();
}

void SettingsManager::applyVolatileQualificationMode(bool proxyBLE, bool obdEnabled) {
    // Runtime-only serial bench override. This intentionally does
    // not call save(), requestDeferredPersist(), or touch NVS: long automated
    // bench loops must be able to exercise proxy/OBD/V1-only modes without
    // changing the user's persisted mode.
    settings_.proxyBLE = proxyBLE;
    settings_.obdEnabled = obdEnabled;
    if (settings_.proxyBLE && settings_.obdEnabled) {
        settings_.proxyBLE = false;
    }
}

void SettingsManager::setProxyName(const String& name) {
    settings_.proxyName = sanitizeProxyNameValue(name);
    save();
}

void SettingsManager::setAutoPowerOffMinutes(uint8_t minutes) {
    settings_.autoPowerOffMinutes = clampU8(minutes, 0, 60);
    save();
}

void SettingsManager::setApTimeoutMinutes(uint8_t minutes) {
    settings_.apTimeoutMinutes = clampApTimeoutValue(minutes);
    save();
}

void SettingsManager::setBrightness(uint8_t brightness) {
    settings_.brightness = brightness;
    save();
}

void SettingsManager::setDisplayOff(bool off) {
    settings_.turnOffDisplay = off;
    save();
}

void SettingsManager::setAutoPushEnabled(bool enabled) {
    settings_.autoPushEnabled = enabled;
    save();
}

void SettingsManager::setActiveSlot(int slot, SettingsPersistMode persistMode) {
    settings_.activeSlot = std::max(0, std::min(2, slot));
    persistSettingsByMode(*this, persistMode);
}

void SettingsManager::setSlot(int slotNum, const String& profileName, V1Mode mode) {
    const String safeProfileName = sanitizeProfileNameValue(profileName);
    const V1Mode safeMode = normalizeV1ModeValue(static_cast<int>(mode));
    AutoPushSlot& slot = settings_.autoPushSlotView(slotNum).config;
    slot.profileName = safeProfileName;
    slot.mode = safeMode;
    save();
}

void SettingsManager::setSlotName(int slotNum, const String& name) {
    String upperName = sanitizeSlotNameValue(name);

    settings_.autoPushSlotView(slotNum).name = upperName;
    save();
}

void SettingsManager::setSlotColor(int slotNum, uint16_t color) {
    static constexpr uint16_t kSlotColorDefaults[] = {0x400A, 0x07E0, 0x8410};
    const int idx = V1Settings::normalizeAutoPushSlotIndex(slotNum);
    settings_.autoPushSlotView(slotNum).color = sanitizeRgb565Color(color, kSlotColorDefaults[idx]);
    save();
}

void SettingsManager::setSlotVolumes(int slotNum, uint8_t volume, uint8_t muteVolume) {
    uint8_t safeVolume = clampSlotVolumeValue(volume);
    uint8_t safeMuteVolume = clampSlotVolumeValue(muteVolume);
    V1Settings::AutoPushSlotView slot = settings_.autoPushSlotView(slotNum);
    slot.volume = safeVolume;
    slot.muteVolume = safeMuteVolume;
    save();
}

void SettingsManager::setDisplayColors(uint16_t bogey, uint16_t freq, uint16_t arrowFront, uint16_t arrowSide,
                                       uint16_t arrowRear, uint16_t bandL, uint16_t bandKa, uint16_t bandK,
                                       uint16_t bandX, bool deferSave) {
    settings_.colorBogey = bogey;
    settings_.colorFrequency = freq;
    settings_.colorArrowFront = arrowFront;
    settings_.colorArrowSide = arrowSide;
    settings_.colorArrowRear = arrowRear;
    settings_.colorBandL = bandL;
    settings_.colorBandKa = bandKa;
    settings_.colorBandK = bandK;
    settings_.colorBandX = bandX;
    if (!deferSave)
        save();
}

void SettingsManager::setWiFiIconColors(uint16_t icon, uint16_t connected) {
    settings_.colorWiFiIcon = icon;
    settings_.colorWiFiConnected = connected;
    save();
}

void SettingsManager::setBleIconColors(uint16_t connected, uint16_t disconnected) {
    settings_.colorBleConnected = connected;
    settings_.colorBleDisconnected = disconnected;
    save();
}

void SettingsManager::setSignalBarColors(uint16_t bar1, uint16_t bar2, uint16_t bar3, uint16_t bar4, uint16_t bar5,
                                         uint16_t bar6) {
    settings_.colorBar1 = bar1;
    settings_.colorBar2 = bar2;
    settings_.colorBar3 = bar3;
    settings_.colorBar4 = bar4;
    settings_.colorBar5 = bar5;
    settings_.colorBar6 = bar6;
    save();
}

void SettingsManager::setMutedColor(uint16_t color) {
    settings_.colorMuted = color;
    save();
}

void SettingsManager::setBandPhotoColor(uint16_t color) {
    settings_.colorBandPhoto = color;
    save();
}

void SettingsManager::setPersistedColor(uint16_t color) {
    settings_.colorPersisted = color;
    save();
}

void SettingsManager::setVolumeMainColor(uint16_t color) {
    settings_.colorVolumeMain = color;
    save();
}

void SettingsManager::setVolumeMuteColor(uint16_t color) {
    settings_.colorVolumeMute = color;
    save();
}

void SettingsManager::setRssiV1Color(uint16_t color) {
    settings_.colorRssiV1 = color;
    save();
}

void SettingsManager::setRssiProxyColor(uint16_t color) {
    settings_.colorRssiProxy = color;
    save();
}

void SettingsManager::setFreqUseBandColor(bool use) {
    settings_.freqUseBandColor = use;
    save();
}

void SettingsManager::setHideWifiIcon(bool hide) {
    settings_.hideWifiIcon = hide;
    save();
}

void SettingsManager::setHideProfileIndicator(bool hide) {
    settings_.hideProfileIndicator = hide;
    save();
}

void SettingsManager::setHideBatteryIcon(bool hide) {
    settings_.hideBatteryIcon = hide;
    save();
}

void SettingsManager::setShowBatteryPercent(bool show) {
    settings_.showBatteryPercent = show;
    save();
}

void SettingsManager::setHideBleIcon(bool hide) {
    settings_.hideBleIcon = hide;
    save();
}

void SettingsManager::setHideVolumeIndicator(bool hide) {
    settings_.hideVolumeIndicator = hide;
    save();
}

void SettingsManager::setHideRssiIndicator(bool hide) {
    settings_.hideRssiIndicator = hide;
    save();
}

void SettingsManager::setVoiceAlertMode(VoiceAlertMode mode) {
    settings_.voiceAlertMode = clampVoiceAlertModeValue(static_cast<int>(mode));
    save();
}

void SettingsManager::setVoiceDirectionEnabled(bool enabled) {
    settings_.voiceDirectionEnabled = enabled;
    save();
}

void SettingsManager::setAnnounceBogeyCount(bool enabled) {
    settings_.announceBogeyCount = enabled;
    save();
}

void SettingsManager::setMuteVoiceIfVolZero(bool mute) {
    settings_.muteVoiceIfVolZero = mute;
    save();
}

void SettingsManager::setAnnounceSecondaryAlerts(bool enabled) {
    settings_.announceSecondaryAlerts = enabled;
    save();
}

void SettingsManager::setSecondaryLaser(bool enabled) {
    settings_.secondaryLaser = enabled;
    save();
}

void SettingsManager::setSecondaryKa(bool enabled) {
    settings_.secondaryKa = enabled;
    save();
}

void SettingsManager::setSecondaryK(bool enabled) {
    settings_.secondaryK = enabled;
    save();
}

void SettingsManager::setSecondaryX(bool enabled) {
    settings_.secondaryX = enabled;
    save();
}

void SettingsManager::setAlertVolumeFade(bool enabled, uint8_t delaySec, uint8_t volume) {
    settings_.alertVolumeFadeEnabled = enabled;
    settings_.alertVolumeFadeDelaySec = clampU8(delaySec, 1, 10);
    settings_.alertVolumeFadeVolume = clampU8(volume, 1, 9);
    save();
}

void SettingsManager::setSpeedMute(bool enabled, uint8_t thresholdMph, uint8_t hysteresisMph) {
    settings_.speedMuteEnabled = enabled;
    settings_.speedMuteThresholdMph = clampU8(thresholdMph, 5, 60);
    settings_.speedMuteHysteresisMph = clampU8(hysteresisMph, 1, 10);
    save();
}

void SettingsManager::setStealthEnabled(bool enabled, SettingsPersistMode persistMode) {
    settings_.stealthEnabled = enabled;
    persistSettingsByMode(*this, persistMode);
}

const AutoPushSlot& SettingsManager::getActiveSlot() const {
    return settings_.autoPushSlotView(settings_.activeSlot).config;
}

const AutoPushSlot& SettingsManager::getSlot(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).config;
}

uint8_t SettingsManager::getSlotVolume(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).volume;
}

uint8_t SettingsManager::getSlotMuteVolume(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).muteVolume;
}

bool SettingsManager::getSlotDarkMode(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).darkMode;
}

bool SettingsManager::getSlotMuteToZero(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).muteToZero;
}

uint8_t SettingsManager::getSlotAlertPersistSec(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).alertPersist;
}

void SettingsManager::setSlotDarkMode(int slotNum, bool darkMode) {
    settings_.autoPushSlotView(slotNum).darkMode = darkMode;
    save();
}

void SettingsManager::setSlotMuteToZero(int slotNum, bool mz) {
    settings_.autoPushSlotView(slotNum).muteToZero = mz;
    save();
}

void SettingsManager::setSlotAlertPersistSec(int slotNum, uint8_t seconds) {
    uint8_t clamped = std::min<uint8_t>(5, seconds);
    settings_.autoPushSlotView(slotNum).alertPersist = clamped;
    save();
}

bool SettingsManager::getSlotPriorityArrowOnly(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).priorityArrow;
}

void SettingsManager::setSlotPriorityArrowOnly(int slotNum, bool prioArrow) {
    settings_.autoPushSlotView(slotNum).priorityArrow = prioArrow;
    save();
}

bool SettingsManager::applyAutoPushSlotUpdate(const AutoPushSlotUpdate& update, SettingsPersistMode persistMode) {
    bool changed = false;
    V1Settings::AutoPushSlotView slot = settings_.autoPushSlotView(update.slot);

    if (update.hasName) {
        changed |= assignIfChanged(slot.name, sanitizeSlotNameValue(update.name));
    }
    if (update.hasColor) {
        changed |= assignIfChanged(slot.color, update.color);
    }
    if (update.hasVolume) {
        changed |= assignIfChanged(slot.volume, clampSlotVolumeValue(update.volume));
    }
    if (update.hasMuteVolume) {
        changed |= assignIfChanged(slot.muteVolume, clampSlotVolumeValue(update.muteVolume));
    }
    if (update.hasDarkMode) {
        changed |= assignIfChanged(slot.darkMode, update.darkMode);
    }
    if (update.hasMuteToZero) {
        changed |= assignIfChanged(slot.muteToZero, update.muteToZero);
    }
    if (update.hasAlertPersist) {
        changed |= assignIfChanged(slot.alertPersist, std::min<uint8_t>(5, update.alertPersist));
    }
    if (update.hasPriorityArrowOnly) {
        changed |= assignIfChanged(slot.priorityArrow, update.priorityArrowOnly);
    }
    if (update.hasProfileName) {
        changed |= assignIfChanged(slot.config.profileName, sanitizeProfileNameValue(update.profileName));
    }
    if (update.hasMode) {
        changed |= assignIfChanged(slot.config.mode, normalizeV1ModeValue(static_cast<int>(update.mode)));
    }

    if (changed) {
        persistSettingsByMode(*this, persistMode);
    }

    return changed;
}

bool SettingsManager::applyAutoPushStateUpdate(const AutoPushStateUpdate& update, SettingsPersistMode persistMode) {
    bool changed = false;

    if (update.hasActiveSlot) {
        changed |= assignIfChanged(settings_.activeSlot,
                                   static_cast<int>(V1Settings::normalizeAutoPushSlotIndex(update.activeSlot)));
    }
    if (update.hasEnabled) {
        changed |= assignIfChanged(settings_.autoPushEnabled, update.enabled);
    }

    if (changed) {
        persistSettingsByMode(*this, persistMode);
    }

    return changed;
}

void SettingsManager::setLastV1Address(const String& addr) {
    String safeAddr = sanitizeLastV1AddressValue(addr);
    if (safeAddr != settings_.lastV1Address) {
        settings_.lastV1Address = safeAddr;
        requestDeferredPersist();
        Serial.printf("Deferred persist for new V1 address: %s\n", safeAddr.c_str());
    }
}

void SettingsManager::applyDeviceSettingsUpdate(const DeviceSettingsUpdate& update, SettingsPersistMode persistMode) {
    bool changed = false;

    if (update.hasApCredentials) {
        changed |= assignIfChanged(settings_.apSSID, sanitizeApSsidValue(update.apSSID));
        changed |= assignIfChanged(settings_.apPassword, sanitizeApPasswordValue(update.apPassword));
    }
    if (update.hasProxyBLE) {
        changed |= assignIfChanged(settings_.proxyBLE, update.proxyBLE);
        if (update.proxyBLE) {
            // Explicit proxy/app mode: the companion app owns speed muting
            // and V1 control. Keep OBD off so the BLE radio is never
            // asked to sustain V1 + phone proxy + OBD at the same time.
            changed |= assignIfChanged(settings_.obdEnabled, false);
        }
    }
    if (update.hasProxyName) {
        changed |= assignIfChanged(settings_.proxyName, sanitizeProxyNameValue(update.proxyName));
    }
    if (update.hasAutoPowerOffMinutes) {
        changed |= assignIfChanged(settings_.autoPowerOffMinutes, clampU8(update.autoPowerOffMinutes, 0, 60));
    }
    if (update.hasApTimeoutMinutes) {
        changed |= assignIfChanged(settings_.apTimeoutMinutes, clampApTimeoutValue(update.apTimeoutMinutes));
    }
    if (update.hasAlpEnabled) {
        changed |= assignIfChanged(settings_.alpEnabled, update.alpEnabled);
    }
    if (update.hasAlpSdLogEnabled) {
        changed |= assignIfChanged(settings_.alpSdLogEnabled, update.alpSdLogEnabled);
    }
    if (update.hasAlpAlertPersistSec) {
        changed |= assignIfChanged(settings_.alpAlertPersistSec, std::min<uint8_t>(5, update.alpAlertPersistSec));
    }
    if (update.hasAlpDisableV1LaserOnPush) {
        changed |= assignIfChanged(settings_.alpDisableV1LaserOnPush, update.alpDisableV1LaserOnPush);
    }
    if (update.hasPowerOffSdLog) {
        changed |= assignIfChanged(settings_.powerOffSdLog, update.powerOffSdLog);
    }
    if (update.hasGpsEnabled) {
        changed |= assignIfChanged(settings_.gpsEnabled, update.gpsEnabled);
    }
    if (update.hasGpsBaud) {
        changed |= assignIfChanged(settings_.gpsBaud, sanitizeGpsBaudValue(update.gpsBaud));
    }
    if (update.hasGpsEnablePinActiveHigh) {
        changed |= assignIfChanged(settings_.gpsEnablePinActiveHigh, true);
    }
    if (update.hasGpsLogUtcToPerf) {
        changed |= assignIfChanged(settings_.gpsLogUtcToPerf, update.gpsLogUtcToPerf);
    }
    if (update.hasGpsLogUtcToAlp) {
        changed |= assignIfChanged(settings_.gpsLogUtcToAlp, update.gpsLogUtcToAlp);
    }

    if (changed) {
        persistSettingsByMode(*this, persistMode);
    }
}

void SettingsManager::applyQuietSettingsUpdate(const QuietSettingsUpdate& update, SettingsPersistMode persistMode) {
    bool changed = false;
    if (update.hasAlertVolumeFadeEnabled) {
        changed |= assignIfChanged(settings_.alertVolumeFadeEnabled, update.alertVolumeFadeEnabled);
    }
    if (update.hasAlertVolumeFadeDelaySec) {
        changed |= assignIfChanged(settings_.alertVolumeFadeDelaySec, clampU8(update.alertVolumeFadeDelaySec, 1, 10));
    }
    if (update.hasAlertVolumeFadeVolume) {
        changed |= assignIfChanged(settings_.alertVolumeFadeVolume, clampU8(update.alertVolumeFadeVolume, 1, 9));
    }
    if (update.hasSpeedMuteEnabled) {
        changed |= assignIfChanged(settings_.speedMuteEnabled, update.speedMuteEnabled);
    }
    if (update.hasSpeedMuteThresholdMph) {
        changed |= assignIfChanged(settings_.speedMuteThresholdMph, clampU8(update.speedMuteThresholdMph, 5, 60));
    }
    if (update.hasSpeedMuteHysteresisMph) {
        changed |= assignIfChanged(settings_.speedMuteHysteresisMph, clampU8(update.speedMuteHysteresisMph, 1, 10));
    }
    if (update.hasSpeedMuteVolume) {
        const uint8_t val = (update.speedMuteVolume <= 9) ? update.speedMuteVolume : 0;
        changed |= assignIfChanged(settings_.speedMuteVolume, val);
    }
    if (update.hasStealthEnabled) {
        changed |= assignIfChanged(settings_.stealthEnabled, update.stealthEnabled);
    }

    if (changed) {
        persistSettingsByMode(*this, persistMode);
    }
}

void SettingsManager::applyAudioSettingsUpdate(const AudioSettingsUpdate& update, SettingsPersistMode persistMode) {
    bool changed = false;

    if (update.hasVoiceAlertMode) {
        changed |= assignIfChanged(settings_.voiceAlertMode,
                                   clampVoiceAlertModeValue(static_cast<int>(update.voiceAlertMode)));
    }
    if (update.hasVoiceDirectionEnabled) {
        changed |= assignIfChanged(settings_.voiceDirectionEnabled, update.voiceDirectionEnabled);
    }
    if (update.hasAnnounceBogeyCount) {
        changed |= assignIfChanged(settings_.announceBogeyCount, update.announceBogeyCount);
    }
    if (update.hasMuteVoiceIfVolZero) {
        changed |= assignIfChanged(settings_.muteVoiceIfVolZero, update.muteVoiceIfVolZero);
    }
    if (update.hasVoiceVolume) {
        changed |= assignIfChanged(settings_.voiceVolume, clampU8(update.voiceVolume, 0, 100));
    }
    if (update.hasAnnounceSecondaryAlerts) {
        changed |= assignIfChanged(settings_.announceSecondaryAlerts, update.announceSecondaryAlerts);
    }
    if (update.hasSecondaryLaser) {
        changed |= assignIfChanged(settings_.secondaryLaser, update.secondaryLaser);
    }
    if (update.hasSecondaryKa) {
        changed |= assignIfChanged(settings_.secondaryKa, update.secondaryKa);
    }
    if (update.hasSecondaryK) {
        changed |= assignIfChanged(settings_.secondaryK, update.secondaryK);
    }
    if (update.hasSecondaryX) {
        changed |= assignIfChanged(settings_.secondaryX, update.secondaryX);
    }
    if (update.hasAlertVolumeFadeEnabled) {
        changed |= assignIfChanged(settings_.alertVolumeFadeEnabled, update.alertVolumeFadeEnabled);
    }
    if (update.hasAlertVolumeFadeDelaySec) {
        changed |= assignIfChanged(settings_.alertVolumeFadeDelaySec, clampU8(update.alertVolumeFadeDelaySec, 1, 10));
    }
    if (update.hasAlertVolumeFadeVolume) {
        changed |= assignIfChanged(settings_.alertVolumeFadeVolume, clampU8(update.alertVolumeFadeVolume, 1, 9));
    }
    if (update.hasSpeedMuteEnabled) {
        changed |= assignIfChanged(settings_.speedMuteEnabled, update.speedMuteEnabled);
    }
    if (update.hasSpeedMuteThresholdMph) {
        changed |= assignIfChanged(settings_.speedMuteThresholdMph, clampU8(update.speedMuteThresholdMph, 5, 60));
    }
    if (update.hasSpeedMuteHysteresisMph) {
        changed |= assignIfChanged(settings_.speedMuteHysteresisMph, clampU8(update.speedMuteHysteresisMph, 1, 10));
    }
    if (update.hasSpeedMuteVolume) {
        const uint8_t val = (update.speedMuteVolume <= 9) ? update.speedMuteVolume : 0;
        changed |= assignIfChanged(settings_.speedMuteVolume, val);
    }
    if (update.hasSpeedMuteVoice) {
        changed |= assignIfChanged(settings_.speedMuteVoice, update.speedMuteVoice);
    }
    if (update.hasStealthEnabled) {
        changed |= assignIfChanged(settings_.stealthEnabled, update.stealthEnabled);
    }

    if (changed) {
        persistSettingsByMode(*this, persistMode);
    }
}

void SettingsManager::applyDisplaySettingsUpdate(const DisplaySettingsUpdate& update, SettingsPersistMode persistMode) {
    bool changed = false;

    // Sanitize all incoming color values: reject 0x0000 (display-blackout value) and
    // fall back to the current stored color. Mirrors the sanitization applied on the
    // NVS-load and SD-restore paths.
#define APPLY_COLOR(field, incoming)                                                                                   \
    changed |= assignIfChanged(settings_.field, sanitizeRgb565Color((incoming), settings_.field))

    if (update.hasColorBogey)
        APPLY_COLOR(colorBogey, update.colorBogey);
    if (update.hasColorFrequency)
        APPLY_COLOR(colorFrequency, update.colorFrequency);
    if (update.hasColorArrowFront)
        APPLY_COLOR(colorArrowFront, update.colorArrowFront);
    if (update.hasColorArrowSide)
        APPLY_COLOR(colorArrowSide, update.colorArrowSide);
    if (update.hasColorArrowRear)
        APPLY_COLOR(colorArrowRear, update.colorArrowRear);
    if (update.hasColorBandL)
        APPLY_COLOR(colorBandL, update.colorBandL);
    if (update.hasColorBandKa)
        APPLY_COLOR(colorBandKa, update.colorBandKa);
    if (update.hasColorBandK)
        APPLY_COLOR(colorBandK, update.colorBandK);
    if (update.hasColorBandX)
        APPLY_COLOR(colorBandX, update.colorBandX);
    if (update.hasColorBandPhoto)
        APPLY_COLOR(colorBandPhoto, update.colorBandPhoto);
    if (update.hasColorWiFiIcon)
        APPLY_COLOR(colorWiFiIcon, update.colorWiFiIcon);
    if (update.hasColorWiFiConnected)
        APPLY_COLOR(colorWiFiConnected, update.colorWiFiConnected);
    if (update.hasColorBleConnected)
        APPLY_COLOR(colorBleConnected, update.colorBleConnected);
    if (update.hasColorBleDisconnected)
        APPLY_COLOR(colorBleDisconnected, update.colorBleDisconnected);
    if (update.hasColorBar1)
        APPLY_COLOR(colorBar1, update.colorBar1);
    if (update.hasColorBar2)
        APPLY_COLOR(colorBar2, update.colorBar2);
    if (update.hasColorBar3)
        APPLY_COLOR(colorBar3, update.colorBar3);
    if (update.hasColorBar4)
        APPLY_COLOR(colorBar4, update.colorBar4);
    if (update.hasColorBar5)
        APPLY_COLOR(colorBar5, update.colorBar5);
    if (update.hasColorBar6)
        APPLY_COLOR(colorBar6, update.colorBar6);
    if (update.hasColorMuted)
        APPLY_COLOR(colorMuted, update.colorMuted);
    if (update.hasColorPersisted)
        APPLY_COLOR(colorPersisted, update.colorPersisted);
    if (update.hasColorVolumeMain)
        APPLY_COLOR(colorVolumeMain, update.colorVolumeMain);
    if (update.hasColorVolumeMute)
        APPLY_COLOR(colorVolumeMute, update.colorVolumeMute);
    if (update.hasColorRssiV1)
        APPLY_COLOR(colorRssiV1, update.colorRssiV1);
    if (update.hasColorRssiProxy)
        APPLY_COLOR(colorRssiProxy, update.colorRssiProxy);
    if (update.hasColorObd)
        APPLY_COLOR(colorObd, update.colorObd);
    if (update.hasColorAlpConnected)
        APPLY_COLOR(colorAlpConnected, update.colorAlpConnected);
    if (update.hasColorAlpDli)
        APPLY_COLOR(colorAlpDli, update.colorAlpDli);
    if (update.hasColorAlpLidActive)
        APPLY_COLOR(colorAlpLidActive, update.colorAlpLidActive);
    if (update.hasColorAlpAlert)
        APPLY_COLOR(colorAlpAlert, update.colorAlpAlert);

#undef APPLY_COLOR
    if (update.hasFreqUseBandColor)
        changed |= assignIfChanged(settings_.freqUseBandColor, update.freqUseBandColor);
    if (update.hasHideWifiIcon)
        changed |= assignIfChanged(settings_.hideWifiIcon, update.hideWifiIcon);
    if (update.hasHideProfileIndicator) {
        changed |= assignIfChanged(settings_.hideProfileIndicator, update.hideProfileIndicator);
    }
    if (update.hasHideBatteryIcon)
        changed |= assignIfChanged(settings_.hideBatteryIcon, update.hideBatteryIcon);
    if (update.hasShowBatteryPercent)
        changed |= assignIfChanged(settings_.showBatteryPercent, update.showBatteryPercent);
    if (update.hasHideBleIcon)
        changed |= assignIfChanged(settings_.hideBleIcon, update.hideBleIcon);
    if (update.hasHideVolumeIndicator) {
        changed |= assignIfChanged(settings_.hideVolumeIndicator, update.hideVolumeIndicator);
    }
    if (update.hasHideRssiIndicator)
        changed |= assignIfChanged(settings_.hideRssiIndicator, update.hideRssiIndicator);
    if (update.hasBrightness)
        changed |= assignIfChanged(settings_.brightness, update.brightness);

    if (changed) {
        persistSettingsByMode(*this, persistMode);
    }
}

void SettingsManager::resetDisplaySettings(SettingsPersistMode persistMode) {
    settings_.colorBogey = 0xF800;
    settings_.colorFrequency = 0xF800;
    settings_.colorArrowFront = 0xF800;
    settings_.colorArrowSide = 0xF800;
    settings_.colorArrowRear = 0xF800;
    settings_.colorBandL = 0x001F;
    settings_.colorBandKa = 0xF800;
    settings_.colorBandK = 0x001F;
    settings_.colorBandX = 0x07E0;
    settings_.colorBandPhoto = 0x780F;
    settings_.colorWiFiIcon = 0x07FF;
    settings_.colorWiFiConnected = 0x07E0;
    settings_.colorBleConnected = 0x07E0;
    settings_.colorBleDisconnected = 0x001F;
    settings_.colorBar1 = 0x07E0;
    settings_.colorBar2 = 0x07E0;
    settings_.colorBar3 = 0xFFE0;
    settings_.colorBar4 = 0xFFE0;
    settings_.colorBar5 = 0xF800;
    settings_.colorBar6 = 0xF800;
    settings_.colorMuted = 0x3186;
    settings_.colorPersisted = 0x18C3;
    settings_.colorVolumeMain = 0xF800; // Red — matches constructor & NVS default
    settings_.colorVolumeMute = 0x7BEF; // Grey — matches constructor & NVS default
    settings_.colorRssiV1 = 0x07E0;
    settings_.colorRssiProxy = 0x001F;
    settings_.colorObd = 0x001F;
    settings_.colorAlpConnected = 0x07E0;
    settings_.colorAlpDli = 0xFD20;
    settings_.colorAlpLidActive = 0x001F;
    settings_.colorAlpAlert = 0xF800;
    settings_.freqUseBandColor = false;

    persistSettingsByMode(*this, persistMode);
}

bool SettingsManager::applyObdSettingsUpdate(const ObdSettingsUpdate& update, SettingsPersistMode persistMode) {
    bool changed = false;

    if (update.resetSavedNameOnAddressChange && update.hasSavedAddress &&
        settings_.obdSavedAddress != update.savedAddress && !update.hasSavedName) {
        changed |= assignIfChanged(settings_.obdSavedName, String(""));
    }

    if (update.hasEnabled) {
        changed |= assignIfChanged(settings_.obdEnabled, update.enabled);
        if (update.enabled) {
            // Explicit OBD/standalone mode: V1 Simple owns speed-aware
            // features locally, so disable the companion-app proxy mode.
            changed |= assignIfChanged(settings_.proxyBLE, false);
        }
    }
    if (update.hasMinRssi) {
        const int clampedRssi = std::max(-100, std::min(static_cast<int>(update.minRssi), -40));
        changed |= assignIfChanged(settings_.obdMinRssi, static_cast<int8_t>(clampedRssi));
    }
    if (update.hasObdScanWindowMs) {
        changed |=
            assignIfChanged(settings_.obdScanWindowMs,
                            clampConnectionCycleObdScanWindowMsValue(static_cast<int64_t>(update.obdScanWindowMs)));
    }
    if (update.hasObdRetryIntervalMs) {
        changed |= assignIfChanged(settings_.obdRetryIntervalMs, clampConnectionCycleObdRetryIntervalMsValue(
                                                                     static_cast<int64_t>(update.obdRetryIntervalMs)));
    }
    if (update.hasProxyOpenWindowMs) {
        changed |=
            assignIfChanged(settings_.proxyOpenWindowMs,
                            clampConnectionCycleProxyOpenWindowMsValue(static_cast<int64_t>(update.proxyOpenWindowMs)));
    }
    if (update.hasWifiOpenTimeoutMs) {
        changed |=
            assignIfChanged(settings_.wifiOpenTimeoutMs,
                            clampConnectionCycleWifiOpenTimeoutMsValue(static_cast<int64_t>(update.wifiOpenTimeoutMs)));
    }
    if (update.hasV1SettleQuietMs) {
        changed |=
            assignIfChanged(settings_.v1SettleQuietMs,
                            clampConnectionCycleV1SettleQuietMsValue(static_cast<int64_t>(update.v1SettleQuietMs)));
    }
    if (update.hasV1SettleFallbackMs) {
        changed |= assignIfChanged(settings_.v1SettleFallbackMs, clampConnectionCycleV1SettleFallbackMsValue(
                                                                     static_cast<int64_t>(update.v1SettleFallbackMs)));
    }
    if (update.hasCycleTeardownAckTimeoutMs) {
        changed |= assignIfChanged(
            settings_.cycleTeardownAckTimeoutMs,
            clampConnectionCycleTeardownAckTimeoutMsValue(static_cast<int64_t>(update.cycleTeardownAckTimeoutMs)));
    }
    if (update.hasSavedAddress) {
        if (isValidBleAddress(update.savedAddress)) {
            changed |= assignIfChanged(settings_.obdSavedAddress, update.savedAddress);
        } else {
            Serial.printf("[Settings] WARN: Rejecting invalid OBD address update: '%s'\n", update.savedAddress.c_str());
        }
    }
    if (update.hasSavedName) {
        changed |= assignIfChanged(settings_.obdSavedName, sanitizeObdSavedNameValue(update.savedName));
    }
    if (update.hasSavedAddrType) {
        changed |= assignIfChanged(settings_.obdSavedAddrType, update.savedAddrType);
    }

    if (changed) {
        persistSettingsByMode(*this, persistMode);
    }

    return changed;
}
