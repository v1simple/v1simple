/**
 * Settings property accessors, slot accessors, and reset behavior.
 */

#include "display_visual_contract.h"
#include "settings_internals.h"

namespace {

template <typename T> bool assignIfChanged(T& target, const T& value) {
    if (target == value) {
        return false;
    }
    target = value;
    return true;
}

SettingsPersistResult persistSettingsByMode(SettingsManager& manager, SettingsPersistMode persistMode) {
    SettingsPersistResult result;
    result.changed = true;
    if (persistMode == SettingsPersistMode::Deferred) {
        manager.requestDeferredPersist();
        result.success = true;
        return result;
    }
    if (persistMode == SettingsPersistMode::ImmediateNvsDeferredBackup) {
        result.success = manager.saveDeferredBackup();
        result.deferred = result.success && manager.deferredBackupPending();
        return result;
    }
    result.success = manager.save();
    result.deferred = result.success && manager.deferredBackupPending();
    return result;
}

} // namespace

SettingsPersistResult SettingsManager::finishSettingsMutation(const V1Settings& before, bool changed,
                                                               SettingsPersistMode persistMode) {
    if (!changed) {
        SettingsPersistResult result;
        result.success = true;
        return result;
    }

    SettingsPersistResult result = persistSettingsByMode(*this, persistMode);
    if (!result.success && persistMode != SettingsPersistMode::Deferred) {
        settings_ = before;
        clearDeferredPersistState();
    }
    return result;
}

SettingsPersistResult SettingsManager::setActiveSlot(int slot, SettingsPersistMode persistMode) {
    const V1Settings before = settings_;
    const bool changed = assignIfChanged(settings_.activeSlot, std::max(0, std::min(2, slot)));
    return finishSettingsMutation(before, changed, persistMode);
}

SettingsPersistResult SettingsManager::setStealthEnabled(bool enabled, SettingsPersistMode persistMode) {
    const V1Settings before = settings_;
    const bool changed = assignIfChanged(settings_.stealthEnabled, enabled);
    return finishSettingsMutation(before, changed, persistMode);
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

bool SettingsManager::getSlotPriorityArrowOnly(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).priorityArrow;
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
        uint8_t volume = clampSlotVolumeValue(update.volume);
        uint8_t muteVolume = update.hasMuteVolume ? clampSlotVolumeValue(update.muteVolume) : slot.muteVolume;
        sanitizeSlotVolumePair(volume, muteVolume);
        changed |= assignIfChanged(slot.volume, volume);
        changed |= assignIfChanged(slot.muteVolume, muteVolume);
    } else if (update.hasMuteVolume) {
        uint8_t volume = slot.volume;
        uint8_t muteVolume = clampSlotVolumeValue(update.muteVolume);
        sanitizeSlotVolumePair(volume, muteVolume);
        changed |= assignIfChanged(slot.volume, volume);
        changed |= assignIfChanged(slot.muteVolume, muteVolume);
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

AutoPushPersistResult SettingsManager::applyAutoPushSlotUpdatePersisted(const AutoPushSlotUpdate& update) {
    const V1Settings before = settings_;
    AutoPushPersistResult result;
    result.changed = applyAutoPushSlotUpdate(update, SettingsPersistMode::Deferred);
    if (!result.changed) {
        result.success = true;
        return result;
    }
    if (saveDeferredBackup()) {
        result.success = true;
        return result;
    }
    settings_ = before;
    clearDeferredPersistState();
    return result;
}

bool SettingsManager::clearProfileReferencesPersisted(const String& canonicalProfileName, bool& changed) {
    changed = false;
    const V1Settings before = settings_;
    for (int slotIndex = 0; slotIndex < 3; ++slotIndex) {
        V1Settings::AutoPushSlotView slot = settings_.autoPushSlotView(slotIndex);
        if (slot.config.profileName == canonicalProfileName) {
            slot.config.profileName = "";
            changed = true;
        }
    }
    if (!changed) {
        return true;
    }
    if (saveDeferredBackup()) {
        return true;
    }
    settings_ = before;
    clearDeferredPersistState();
    return false;
}

SettingsPersistResult SettingsManager::applyAutoPushStateUpdate(const AutoPushStateUpdate& update,
                                                                SettingsPersistMode persistMode) {
    const V1Settings before = settings_;
    bool changed = false;

    if (update.hasActiveSlot) {
        changed |= assignIfChanged(settings_.activeSlot,
                                   static_cast<int>(V1Settings::normalizeAutoPushSlotIndex(update.activeSlot)));
    }
    if (update.hasEnabled) {
        changed |= assignIfChanged(settings_.autoPushEnabled, update.enabled);
    }

    return finishSettingsMutation(before, changed, persistMode);
}

SettingsPersistResult
SettingsManager::applyWifiStaPriorityUpdates(const std::vector<WifiStaPriorityUpdate>& updates) {
    const V1Settings before = settings_;
    bool seenSlots[kWifiStaSlotCount] = {};
    bool seenPriorities[kWifiStaSlotCount] = {};
    bool changed = false;

    if (updates.empty() || updates.size() > kWifiStaSlotCount) {
        return SettingsPersistResult{};
    }
    for (const WifiStaPriorityUpdate& update : updates) {
        if (update.index >= kWifiStaSlotCount || update.priority >= kWifiStaSlotCount || seenSlots[update.index] ||
            seenPriorities[update.priority] || !settings_.wifiStaSlots[update.index].isConfigured()) {
            settings_ = before;
            return SettingsPersistResult{};
        }
        seenSlots[update.index] = true;
        seenPriorities[update.priority] = true;
        changed |= assignIfChanged(settings_.wifiStaSlots[update.index].priority, update.priority);
    }

    bool finalPriorities[kWifiStaSlotCount] = {};
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        const WifiStaSlot& slot = settings_.wifiStaSlots[index];
        if (!slot.isConfigured()) {
            continue;
        }
        if (slot.priority >= kWifiStaSlotCount || finalPriorities[slot.priority]) {
            settings_ = before;
            return SettingsPersistResult{};
        }
        finalPriorities[slot.priority] = true;
    }

    settings_.refreshWifiClientAliasFromSlots();
    return finishSettingsMutation(before, changed, SettingsPersistMode::Immediate);
}

void SettingsManager::setLastV1Address(const String& addr) {
    String safeAddr = sanitizeLastV1AddressValue(addr);
    if (safeAddr != settings_.lastV1Address) {
        settings_.lastV1Address = safeAddr;
        // The connected-device path records this address in V1DeviceStore,
        // which owns runtime durability. A full settings transaction rewrites
        // every NVS key and builds an SD backup, so do not schedule that work
        // solely for this compatibility/fallback field while BLE is active.
        // A confirmed connection uses one dedicated NVS fallback key when the
        // filesystem store is unavailable. Explicit settings saves and
        // graceful shutdown still capture the compatibility field as well.
        Serial.println("Updated runtime V1 address");
    }
}

SettingsPersistResult SettingsManager::applyDeviceSettingsUpdate(const DeviceSettingsUpdate& update,
                                                                 SettingsPersistMode persistMode) {
    const V1Settings before = settings_;
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
    if (update.hasAlpAlertPersistSec) {
        changed |= assignIfChanged(settings_.alpAlertPersistSec, std::min<uint8_t>(5, update.alpAlertPersistSec));
    }
    if (update.hasAlpDisableV1LaserOnPush) {
        changed |= assignIfChanged(settings_.alpDisableV1LaserOnPush, update.alpDisableV1LaserOnPush);
    }
    if (update.hasGpsEnabled) {
        changed |= assignIfChanged(settings_.gpsEnabled, update.gpsEnabled);
    }
    if (update.hasGpsBaud) {
        changed |= assignIfChanged(settings_.gpsBaud, sanitizeGpsBaudValue(update.gpsBaud));
    }
    return finishSettingsMutation(before, changed, persistMode);
}

SettingsPersistResult SettingsManager::applyAudioSettingsUpdate(const AudioSettingsUpdate& update,
                                                                SettingsPersistMode persistMode) {
    const V1Settings before = settings_;
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

    return finishSettingsMutation(before, changed, persistMode);
}

SettingsPersistResult SettingsManager::applyDisplaySettingsUpdate(const DisplaySettingsUpdate& update,
                                                                  SettingsPersistMode persistMode) {
    const V1Settings before = settings_;
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
    if (update.hasColorWiFiConnected)
        APPLY_COLOR(colorWiFiConnected, update.colorWiFiConnected);
    if (update.hasColorBleConnected)
        APPLY_COLOR(colorBleConnected, update.colorBleConnected);
    if (update.hasColorBleDisconnected)
        APPLY_COLOR(colorBleDisconnected, update.colorBleDisconnected);
    for (int barIndex = 0; barIndex < SIGNAL_BAR_COLOR_COUNT; ++barIndex) {
        if (update.hasColorBar[barIndex]) {
            const uint16_t sanitized = sanitizeRgb565Color(update.colorBars[barIndex], settings_.colorBars[barIndex]);
            if (settings_.colorBars[barIndex] != sanitized) {
                settings_.colorBars[barIndex] = sanitized;
                changed = true;
            }
        }
    }
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

    return finishSettingsMutation(before, changed, persistMode);
}

SettingsPersistResult SettingsManager::resetDisplaySettings(SettingsPersistMode persistMode) {
    const V1Settings before = settings_;
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
    settings_.colorWiFiConnected = 0x07E0;
    settings_.colorBleConnected = 0x07E0;
    settings_.colorBleDisconnected = 0x001F;
    static constexpr uint16_t kBarDefaults[SIGNAL_BAR_COLOR_COUNT] = {
        0x07E0, 0x07E0, 0xFFE0, 0xFFE0, 0xF800, 0xF800,
    };
    for (int barIndex = 0; barIndex < SIGNAL_BAR_COLOR_COUNT; ++barIndex) {
        settings_.colorBars[barIndex] = kBarDefaults[barIndex];
    }
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

    return finishSettingsMutation(before, true, persistMode);
}

SettingsPersistResult SettingsManager::applyObdSettingsUpdate(const ObdSettingsUpdate& update,
                                                              SettingsPersistMode persistMode) {
    const V1Settings before = settings_;
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
        if (update.savedAddress.length() == 0 || isValidBleAddress(update.savedAddress)) {
            changed |= assignIfChanged(settings_.obdSavedAddress, update.savedAddress);
        } else {
            Serial.println("[Settings] WARN: Rejecting invalid OBD address update");
        }
    }
    if (update.hasSavedName) {
        changed |= assignIfChanged(settings_.obdSavedName, sanitizeObdSavedNameValue(update.savedName));
    }
    if (update.hasSavedAddrType) {
        changed |= assignIfChanged(settings_.obdSavedAddrType, update.savedAddrType);
    }

    return finishSettingsMutation(before, changed, persistMode);
}
