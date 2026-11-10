#include "display_visual_contract.h"
#include "json_exact_input.h"
#include "settings_internals.h"

#include <cstring>

namespace {

template <typename T> bool assignIfChanged(T& target, const T& value) {
    if (target == value) {
        return false;
    }
    target = value;
    return true;
}

bool exactSettingsStringCopy(const String& source, String& destination) {
    destination = source;
    return destination.length() == source.length() && destination == source;
}

// Arduino String copies can fail silently. A rollback snapshot must be complete
// before any live field changes, including updates that only change scalars.
bool settingsSnapshotStringsMatch(const V1Settings& source, const V1Settings& snapshot) {
    if (source.apSSID != snapshot.apSSID || source.apPassword != snapshot.apPassword ||
        source.wifiClientSSID != snapshot.wifiClientSSID || source.proxyName != snapshot.proxyName ||
        source.lastV1Address != snapshot.lastV1Address || source.obdSavedAddress != snapshot.obdSavedAddress ||
        source.obdSavedName != snapshot.obdSavedName || source.slot0Name != snapshot.slot0Name ||
        source.slot1Name != snapshot.slot1Name || source.slot2Name != snapshot.slot2Name ||
        source.slot0_default.profileName != snapshot.slot0_default.profileName ||
        source.slot1_highway.profileName != snapshot.slot1_highway.profileName ||
        source.slot2_comfort.profileName != snapshot.slot2_comfort.profileName) return false;
    for (size_t index = 0; index < kWifiStaSlotCount; ++index) {
        if (source.wifiStaSlots[index].ssid != snapshot.wifiStaSlots[index].ssid ||
            source.wifiStaSlots[index].label != snapshot.wifiStaSlots[index].label) return false;
    }
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

void SettingsManager::noteDisplayConfigurationMutation() {
    if (displayConfigurationRevision_ != UINT32_MAX) {
        ++displayConfigurationRevision_;
    }
}

SettingsPersistResult SettingsManager::finishSettingsMutation(V1Settings& before, bool changed,
                                                               SettingsPersistMode persistMode) {
    if (!changed) {
        SettingsPersistResult result;
        result.success = true;
        return result;
    }

    const auto previousSlot = before.autoPushSlotView(before.activeSlot);
    const auto currentSlot = settings_.autoPushSlotView(settings_.activeSlot);
    if (before.activeSlot != settings_.activeSlot || before.stealthEnabled != settings_.stealthEnabled ||
        previousSlot.priorityArrow != currentSlot.priorityArrow || previousSlot.alertPersist != currentSlot.alertPersist) {
        // Count the attempt before persistence; a failed write can roll values
        // back, but must not make A -> B -> A look unchanged to a bench capture.
        noteDisplayConfigurationMutation();
    }
    SettingsPersistResult result = persistSettingsByMode(*this, persistMode);
    if (!result.success && persistMode != SettingsPersistMode::Deferred) {
        // The validated snapshot already owns its strings; rollback must not
        // need another allocation while recovering from a failed save.
        settings_ = std::move(before);
        // A failed immediate save leaves any earlier deferred mutation in
        // this snapshot. Preserve its pending/retry state and original due time.
    }
    return result;
}

SettingsPersistResult SettingsManager::setActiveSlot(int slot, SettingsPersistMode persistMode) {
    if (!ensureSettingsLoaded()) return {};
    V1Settings before = settings_;
    if (!settingsSnapshotStringsMatch(settings_, before)) return {};
    const bool changed = assignIfChanged(settings_.activeSlot, std::max(0, std::min(2, slot)));
    return finishSettingsMutation(before, changed, persistMode);
}

SettingsPersistResult SettingsManager::setStealthEnabled(bool enabled, SettingsPersistMode persistMode) {
    if (!ensureSettingsLoaded()) return {};
    V1Settings before = settings_;
    if (!settingsSnapshotStringsMatch(settings_, before)) return {};
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

bool SettingsManager::getSlotVolumeOverride(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).volumeOverride;
}

bool SettingsManager::getSlotDarkModeOverride(int slotNum) const {
    return settings_.autoPushSlotView(slotNum).darkModeOverride;
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

AutoPushPersistResult SettingsManager::applyAutoPushSlotUpdatePersisted(const AutoPushSlotUpdate& update) {
    if (!ensureSettingsLoaded()) return {};
    AutoPushPersistResult result;
    V1Settings::AutoPushSlotView slot = settings_.autoPushSlotView(update.slot);
    const uint16_t colorBefore = slot.color;
    const uint8_t volumeBefore = slot.volume;
    const uint8_t muteVolumeBefore = slot.muteVolume;
    const bool darkModeBefore = slot.darkMode;
    const bool volumeOverrideBefore = slot.volumeOverride;
    const bool darkModeOverrideBefore = slot.darkModeOverride;
    const bool muteToZeroBefore = slot.muteToZero;
    const uint8_t alertPersistBefore = slot.alertPersist;
    const bool priorityArrowBefore = slot.priorityArrow;
    const V1Mode modeBefore = slot.config.mode;
    uint8_t desiredVolume = slot.volume;
    uint8_t desiredMuteVolume = slot.muteVolume;
    if (update.hasVolume) {
        desiredVolume = clampSlotVolumeValue(update.volume);
        desiredMuteVolume = update.hasMuteVolume ? clampSlotVolumeValue(update.muteVolume) : slot.muteVolume;
        sanitizeSlotVolumePair(desiredVolume, desiredMuteVolume);
    } else if (update.hasMuteVolume) {
        desiredMuteVolume = clampSlotVolumeValue(update.muteVolume);
        sanitizeSlotVolumePair(desiredVolume, desiredMuteVolume);
    }
    if ((update.hasVolumeOverride ? update.volumeOverride : slot.volumeOverride) &&
        (desiredVolume > 9 || desiredMuteVolume > 9)) return result;
    // Build the complete candidate exactly once.  The route-owned update
    // remains immutable; no live field changes until both allocation-bearing
    // strings and every scalar policy have been validated.
    String desiredName = slot.name;
    String desiredProfile = slot.config.profileName;
    if (desiredName.length() != slot.name.length() || desiredName != slot.name ||
        desiredProfile.length() != slot.config.profileName.length() || desiredProfile != slot.config.profileName) {
        return result;
    }
    if (update.hasName) {
        if (!ExactJsonInput::validSemanticString(update.name.c_str(), update.name.length())) return result;
        desiredName = sanitizeSlotNameValue(update.name);
        if (desiredName.length() != update.name.length() || desiredName != update.name) return result;
    }
    if (update.hasProfileName) {
        desiredProfile = sanitizeProfileNameValue(update.profileName);
        if (desiredProfile.length() != update.profileName.length() || desiredProfile != update.profileName) {
            return result;
        }
    }
    const bool expectedChange =
        desiredName != slot.name || (update.hasColor && update.color != slot.color) ||
        desiredVolume != slot.volume || desiredMuteVolume != slot.muteVolume ||
        (update.hasDarkMode && update.darkMode != slot.darkMode) ||
        (update.hasVolumeOverride && update.volumeOverride != slot.volumeOverride) ||
        (update.hasDarkModeOverride && update.darkModeOverride != slot.darkModeOverride) ||
        (update.hasMuteToZero && update.muteToZero != slot.muteToZero) ||
        (update.hasAlertPersist && std::min<uint8_t>(5, update.alertPersist) != slot.alertPersist) ||
        (update.hasPriorityArrowOnly && update.priorityArrowOnly != slot.priorityArrow) ||
        desiredProfile != slot.config.profileName ||
        (update.hasMode && normalizeV1ModeValue(static_cast<int>(update.mode)) != slot.config.mode);
    if (!expectedChange) {
        result.success = true;
        return result;
    }

    // Moving the old strings out gives rollback ownership without another
    // allocation. Moving the fully prepared candidates in cannot silently
    // turn a requested non-empty field into an empty successful mutation.
    String nameBefore = std::move(slot.name);
    String profileBefore = std::move(slot.config.profileName);
    slot.name = std::move(desiredName);
    slot.config.profileName = std::move(desiredProfile);
    if (update.hasColor) slot.color = update.color;
    slot.volume = desiredVolume;
    slot.muteVolume = desiredMuteVolume;
    if (update.hasDarkMode) slot.darkMode = update.darkMode;
    if (update.hasVolumeOverride) slot.volumeOverride = update.volumeOverride;
    if (update.hasDarkModeOverride) slot.darkModeOverride = update.darkModeOverride;
    if (update.hasMuteToZero) slot.muteToZero = update.muteToZero;
    if (update.hasAlertPersist) slot.alertPersist = std::min<uint8_t>(5, update.alertPersist);
    if (update.hasPriorityArrowOnly) slot.priorityArrow = update.priorityArrowOnly;
    if (update.hasMode) slot.config.mode = normalizeV1ModeValue(static_cast<int>(update.mode));

    if (V1Settings::normalizeAutoPushSlotIndex(update.slot) ==
            V1Settings::normalizeAutoPushSlotIndex(settings_.activeSlot) &&
        (priorityArrowBefore != slot.priorityArrow || alertPersistBefore != slot.alertPersist)) {
        noteDisplayConfigurationMutation();
    }
    result.changed = true;
    if (saveDeferredBackup()) {
        result.success = true;
        return result;
    }
    slot.name = std::move(nameBefore);
    slot.config.profileName = std::move(profileBefore);
    slot.color = colorBefore;
    slot.volume = volumeBefore;
    slot.muteVolume = muteVolumeBefore;
    slot.darkMode = darkModeBefore;
    slot.volumeOverride = volumeOverrideBefore;
    slot.darkModeOverride = darkModeOverrideBefore;
    slot.muteToZero = muteToZeroBefore;
    slot.alertPersist = alertPersistBefore;
    slot.priorityArrow = priorityArrowBefore;
    slot.config.mode = modeBefore;
    clearDeferredPersistState();
    return result;
}

SettingsPersistResult SettingsManager::applyAutoPushStateUpdate(const AutoPushStateUpdate& update,
                                                                SettingsPersistMode persistMode) {
    if (!ensureSettingsLoaded()) return {};
    V1Settings before = settings_;
    if (!settingsSnapshotStringsMatch(settings_, before)) return {};
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
    if (!ensureSettingsLoaded()) return {};
    V1Settings before = settings_;
    if (!settingsSnapshotStringsMatch(settings_, before)) return {};
    bool seenSlots[kWifiStaSlotCount] = {};
    bool seenPriorities[kWifiStaSlotCount] = {};
    bool changed = false;

    if (updates.empty() || updates.size() > kWifiStaSlotCount) {
        return SettingsPersistResult{};
    }
    for (const WifiStaPriorityUpdate& update : updates) {
        if (update.index >= kWifiStaSlotCount || update.priority >= kWifiStaSlotCount || seenSlots[update.index] ||
            seenPriorities[update.priority] || !settings_.wifiStaSlots[update.index].isConfigured()) {
            settings_ = std::move(before);
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
            settings_ = std::move(before);
            return SettingsPersistResult{};
        }
        finalPriorities[slot.priority] = true;
    }

    settings_.refreshWifiClientAliasFromSlots();
    return finishSettingsMutation(before, changed, SettingsPersistMode::Immediate);
}

void SettingsManager::setLastV1Address(const String& addr) {
    if (!ensureSettingsLoaded()) return;
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
    if (!ensureSettingsLoaded()) return {};
    String preparedApSsid;
    String preparedApPassword;
    String preparedProxyName;
    if (update.hasApCredentials) {
        if (update.apSSID.length() == 0 || update.apSSID.length() > MAX_WIFI_SSID_LEN ||
            update.apPassword.length() < MIN_AP_PASSWORD_LEN ||
            update.apPassword.length() > MAX_AP_PASSWORD_LEN ||
            !ExactJsonInput::validSemanticString(update.apSSID.c_str(), update.apSSID.length()) ||
            !ExactJsonInput::validSemanticString(update.apPassword.c_str(), update.apPassword.length()) ||
            !exactSettingsStringCopy(update.apSSID, preparedApSsid) ||
            !exactSettingsStringCopy(update.apPassword, preparedApPassword)) return {};
    }
    if (update.hasProxyName) {
        if (update.proxyName.length() > MAX_PROXY_NAME_LEN ||
            !ExactJsonInput::validSemanticString(update.proxyName.c_str(), update.proxyName.length())) return {};
        if (update.proxyName.length() == 0) {
            preparedProxyName = "V1-Proxy";
            if (preparedProxyName.length() != std::strlen("V1-Proxy")) return {};
        } else if (!exactSettingsStringCopy(update.proxyName, preparedProxyName)) {
            return {};
        }
    }

    V1Settings before = settings_;
    if (!settingsSnapshotStringsMatch(settings_, before)) return {};
    bool changed = false;

    if (update.hasApCredentials) {
        if (settings_.apSSID != preparedApSsid) {
            settings_.apSSID = std::move(preparedApSsid);
            changed = true;
        }
        if (settings_.apPassword != preparedApPassword) {
            settings_.apPassword = std::move(preparedApPassword);
            changed = true;
        }
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
        if (settings_.proxyName != preparedProxyName) {
            settings_.proxyName = std::move(preparedProxyName);
            changed = true;
        }
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
    if (!ensureSettingsLoaded()) return {};
    V1Settings before = settings_;
    if (!settingsSnapshotStringsMatch(settings_, before)) return {};
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
    if (!ensureSettingsLoaded()) return {};
    V1Settings before = settings_;
    if (!settingsSnapshotStringsMatch(settings_, before)) return {};
    bool changed = false;

    // Partial updates reject black by retaining the current color.
    DisplaySettingsContract::forEachColor([&](const auto& field) {
        if (update.*(field.present)) {
            auto& current = settings_.*(field.value);
            changed |= assignIfChanged(current, sanitizeRgb565Color(update.*(field.incoming), current));
        }
    });
    for (int barIndex = 0; barIndex < SIGNAL_BAR_COLOR_COUNT; ++barIndex) {
        if (update.hasColorBar[barIndex]) {
            const uint16_t sanitized = sanitizeRgb565Color(update.colorBars[barIndex], settings_.colorBars[barIndex]);
            if (settings_.colorBars[barIndex] != sanitized) {
                settings_.colorBars[barIndex] = sanitized;
                changed = true;
            }
        }
    }
    for (const auto& field : DisplaySettingsContract::kFlags) {
        if (update.*(field.present)) {
            changed |= assignIfChanged(settings_.*(field.value), update.*(field.incoming));
        }
    }
    const auto& brightness = DisplaySettingsContract::kBrightness;
    if (update.*(brightness.present)) {
        changed |= assignIfChanged(settings_.*(brightness.value), update.*(brightness.incoming));
    }

    return finishSettingsMutation(before, changed, persistMode);
}

SettingsPersistResult SettingsManager::resetDisplaySettings(SettingsPersistMode persistMode) {
    if (!ensureSettingsLoaded()) return {};
    V1Settings before = settings_;
    if (!settingsSnapshotStringsMatch(settings_, before)) return {};
    DisplaySettingsContract::forEachScalar([&](const auto& field) {
        if (field.resetTheme) settings_.*(field.value) = field.defaultValue;
    });
    for (int index = 0; index < SIGNAL_BAR_COLOR_COUNT; ++index) {
        settings_.colorBars[index] = DisplaySettingsContract::kBarDefaults[index];
    }

    return finishSettingsMutation(before, true, persistMode);
}

SettingsPersistResult SettingsManager::applyObdSettingsUpdate(const ObdSettingsUpdate& update,
                                                              SettingsPersistMode persistMode) {
    if (!ensureSettingsLoaded()) return {};
    V1Settings before = settings_;
    if (!settingsSnapshotStringsMatch(settings_, before)) return {};
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
