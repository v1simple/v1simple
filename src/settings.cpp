/**
 * Settings storage implementation
 *
 * SECURITY NOTE: WiFi passwords are stored with XOR obfuscation, NOT encryption.
 * This is intentional - it prevents casual viewing in hex dumps but is NOT secure
 * against a determined attacker with physical access to the device.
 *
 * For this use case (a car accessory on a private network), the trade-off is:
 * - Pro: Simple, no crypto library overhead, recoverable if key changes
 * - Con: Not suitable for high-security applications
 *
 * If stronger security is needed, consider ESP32 NVS encryption (requires flash
 * encryption key management) or storing a hash instead of the actual password.
 */

#include "settings_internals.h"

// SD backup file path
const char* SETTINGS_BACKUP_PATH = "/v1simple_backup.json";
const char* SETTINGS_BACKUP_TMP_PATH = "/v1simple_backup.tmp";
const char* SETTINGS_BACKUP_PREV_PATH = "/v1simple_backup.prev";
const size_t SETTINGS_BACKUP_MAX_BYTES = 512 * 1024;
const char* WIFI_CLIENT_NS = kSettingsWifiClientNamespace;
const char* WIFI_CLIENT_SD_SECRET_PATH = "/v1wifi_secret.json";
const char* WIFI_CLIENT_SD_SECRET_TYPE = "v1wifi_secret";
const int WIFI_CLIENT_SD_SECRET_VERSION = 2;
const char* const SETTINGS_BACKUP_CANDIDATES[] = {
    SETTINGS_BACKUP_PATH,
    SETTINGS_BACKUP_PREV_PATH,
    "/v1simple_settings.json",
    "/v1settings_backup.json"
};
const size_t SETTINGS_BACKUP_CANDIDATES_COUNT = sizeof(SETTINGS_BACKUP_CANDIDATES) / sizeof(SETTINGS_BACKUP_CANDIDATES[0]);

WiFiModeSetting clampWifiModeValue(int raw) {
    if (raw == static_cast<int>(V1_WIFI_AP)) return V1_WIFI_AP;
    if (raw == static_cast<int>(V1_WIFI_APSTA)) return V1_WIFI_APSTA;
    return V1_WIFI_OFF;
}

VoiceAlertMode clampVoiceAlertModeValue(int raw) {
    int clamped = std::max(static_cast<int>(VOICE_MODE_DISABLED),
                           std::min(raw, static_cast<int>(VOICE_MODE_BAND_FREQ)));
    return static_cast<VoiceAlertMode>(clamped);
}

static constexpr size_t MAX_V1_ADDRESS_LEN = 32;
static constexpr size_t MAX_OBD_SAVED_NAME_LEN = 32;
static constexpr uint32_t SETTINGS_DEFERRED_PERSIST_DEBOUNCE_MS = 750;
static constexpr uint32_t SETTINGS_DEFERRED_PERSIST_RETRY_BACKOFF_MS = 1000;

static_assert(kWifiStaSlotCount == kNvsWifiStaSlotCount,
              "WiFi STA slot model and NVS key arrays must stay in sync");

static bool isDeferredPersistDue(uint32_t nowMs, uint32_t targetMs) {
    return static_cast<int32_t>(nowMs - targetMs) >= 0;
}

String sanitizeApPasswordValue(const String& raw) {
    String value = clampStringLength(raw, MAX_AP_PASSWORD_LEN);
    if (value.length() < MIN_AP_PASSWORD_LEN) {
        return "setupv1simple";
    }
    return value;
}

String sanitizeLastV1AddressValue(const String& raw) {
    return clampStringLength(raw, MAX_V1_ADDRESS_LEN);
}

String sanitizeObdSavedNameValue(const String& raw) {
    String value = clampStringLength(raw, MAX_OBD_SAVED_NAME_LEN);
    value.trim();
    return value;
}

static void migrateLegacyWifiStaSlotNvs(const String& activeNs,
                                        const WifiStaSlot& slot0,
                                        bool legacySsidKeyPresent) {
    if (activeNs.length() == 0) {
        return;
    }

    if (legacySsidKeyPresent) {
        Preferences settingsPrefs;
        if (settingsPrefs.begin(activeNs.c_str(), false)) {
            if (slot0.ssid.length() > 0) {
                settingsPrefs.putString(kNvsWifiStaSlotSsid[0], slot0.ssid);
                settingsPrefs.putString(kNvsWifiStaSlotLabel[0], slot0.label);
                settingsPrefs.putUChar(kNvsWifiStaSlotPriority[0], slot0.priority);
                settingsPrefs.putUInt(kNvsWifiStaSlotLastConnected[0], slot0.lastConnectedAtSec);
            }
            settingsPrefs.remove(kNvsWifiClientSsid);
            settingsPrefs.end();
            Serial.println("[Settings] Migrated legacy WiFi client SSID into STA slot 0");
        }
    }

    Preferences wifiPrefs;
    if (!wifiPrefs.begin(WIFI_CLIENT_NS, false)) {
        return;
    }
    const bool legacyPasswordKeyPresent = wifiPrefs.isKey(kNvsWifiPassword);
    if (legacyPasswordKeyPresent) {
        if (slot0.ssid.length() > 0 && !wifiPrefs.isKey(kNvsWifiStaSlotPassword[0])) {
            wifiPrefs.putString(kNvsWifiStaSlotPassword[0], wifiPrefs.getString(kNvsWifiPassword, ""));
        }
        wifiPrefs.remove(kNvsWifiPassword);
        Serial.println("[Settings] Migrated legacy WiFi client password into STA slot 0");
    }
    wifiPrefs.end();
}


// Global instance
SettingsManager settingsManager;

// XOR_KEY and OBFUSCATION_HEX_PREFIX are defined in settings_backup.cpp.

SettingsManager::SettingsManager() {}

void SettingsManager::bumpBackupRevision() {
    if (backupRevisionCounter_ == UINT32_MAX) {
        backupRevisionCounter_ = 1;
        return;
    }
    backupRevisionCounter_++;
}

void SettingsManager::clearDeferredPersistState() {
    deferredPersistPending_ = false;
    deferredPersistRetryScheduled_ = false;
    deferredPersistNextAttemptAtMs_ = 0;
}

void SettingsManager::markRestorePending(const char* reason) {
    if (!restorePending_) {
        Serial.printf("[Settings] Restore pending: %s\n", reason ? reason : "unspecified");
    }
    restorePending_ = true;
}

void SettingsManager::clearRestorePending() {
    if (restorePending_) {
        Serial.println("[Settings] Restore pending cleared");
    }
    restorePending_ = false;
}

void SettingsManager::begin() {
    // Ensure WiFi client namespace exists so read-only opens do not spam
    // NOT_FOUND on fresh/erased NVS.
    Preferences wifiClientNs;
    if (wifiClientNs.begin(WIFI_CLIENT_NS, false)) {
        wifiClientNs.end();
    } else {
        Serial.println("[Settings] WARN: Failed to initialize WiFi client namespace");
    }

    load();

    // Note: SD card may not be mounted yet during begin().
    // checkAndRestoreFromSD() should be called after storage is ready.
    // We still try here in case storage was already initialized.
    checkAndRestoreFromSD();
}


void SettingsManager::load() {
    String activeNs = getActiveNamespace();
    if (!preferences_.begin(activeNs.c_str(), true)) {
        Serial.printf("[Settings] WARN: Failed to open namespace %s, falling back to legacy\n", activeNs.c_str());
        activeNs = SETTINGS_NS_LEGACY;
        if (!preferences_.begin(activeNs.c_str(), true)) {
            Serial.println("[Settings] ERROR: Failed to open preferences for reading!");
            return;
        }
    }

    // Check settings version for migration
    int storedVersion = preferences_.getInt(kNvsSettingsVer, 1);
    restorePending_ = preferences_.getBool(kNvsRestorePending, false);
    if (restorePending_) {
        Serial.println("[Settings] NVS marked restore-pending; SD backup remains authoritative");
    }

    settings_.enableWifi = preferences_.getBool(kNvsEnableWifi, true);

    // Handle AP password storage - version 1 was plain text, version 2+ is obfuscated
    String storedApPwd = preferences_.getString(kNvsApPassword, "");

    if (storedVersion >= 2) {
        // Passwords are obfuscated - decode and sanitize them.
        settings_.apPassword = sanitizeApPasswordValue(
            storedApPwd.length() > 0 ? decodeObfuscatedFromStorage(storedApPwd) : "setupv1simple");
    } else {
        // Version 1 - passwords stored in plain text, use as-is then sanitize.
        settings_.apPassword = sanitizeApPasswordValue(storedApPwd.length() > 0 ? storedApPwd : "setupv1simple");
        Serial.println("[Settings] Migrating from v1 to v2 (password obfuscation)");
    }

    settings_.apSSID = sanitizeApSsidValue(preferences_.getString(kNvsApSsid, "V1-Simple"));

    // WiFi client (STA) settings
    const bool wifiClientEnabledKeyPresent = preferences_.isKey(kNvsWifiClientEnabled);
    const bool wifiClientSsidKeyPresent = preferences_.isKey(kNvsWifiClientSsid);
    settings_.wifiClientEnabled = preferences_.getBool(kNvsWifiClientEnabled, false);
    const String legacyWifiClientSsid =
        sanitizeWifiClientSsidValue(preferences_.getString(kNvsWifiClientSsid, ""));
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        WifiStaSlot& slot = settings_.wifiStaSlots[i];
        slot.ssid = sanitizeWifiClientSsidValue(preferences_.getString(kNvsWifiStaSlotSsid[i], ""));
        slot.label = sanitizeWifiStaSlotLabelValue(preferences_.getString(kNvsWifiStaSlotLabel[i], ""));
        slot.priority = preferences_.getUChar(kNvsWifiStaSlotPriority[i], static_cast<uint8_t>(i));
        slot.lastConnectedAtSec = preferences_.getUInt(kNvsWifiStaSlotLastConnected[i], 0);
    }

    if (!settings_.hasConfiguredWifiStaSlot() && legacyWifiClientSsid.length() > 0) {
        settings_.wifiStaSlots[0].ssid = legacyWifiClientSsid;
        settings_.wifiStaSlots[0].label = "Saved";
        settings_.wifiStaSlots[0].priority = 0;
        settings_.wifiStaSlots[0].lastConnectedAtSec = 0;
    }

    // Self-healing for legacy/incomplete NVS only: older settings could contain
    // a saved STA SSID without the explicit enabled flag.  If the flag is
    // present, preserve it exactly so the Web UI's WiFi Client OFF toggle
    // stays OFF across reboot while keeping saved networks.
    if (!wifiClientEnabledKeyPresent &&
        !settings_.wifiClientEnabled &&
        settings_.hasConfiguredWifiStaSlot()) {
        Serial.println("[Settings] HEAL: wifiClientEnabled flag missing but SSID is set — enabling");
        settings_.wifiClientEnabled = true;
    }
    settings_.refreshWifiClientAliasFromSlots();

    // Debug: Log WiFi client settings on load
    Serial.printf("[Settings] WiFi client keys: enabledKey=%s ssidKey=%s\n",
                  wifiClientEnabledKeyPresent ? "yes" : "no",
                  wifiClientSsidKeyPresent ? "yes" : "no");
    Serial.printf("[Settings] WiFi client: enabled=%s, SSID='%s'\n",
                  settings_.wifiClientEnabled ? "true" : "false",
                  settings_.wifiClientSSID.c_str());

    settings_.proxyBLE = preferences_.getBool(kNvsProxyBle, true);
    settings_.proxyName = sanitizeProxyNameValue(preferences_.getString(kNvsProxyName, "V1-Proxy"));
    settings_.turnOffDisplay = preferences_.getBool(kNvsDisplayOff, false);
    settings_.brightness = std::max<uint8_t>(1, preferences_.getUChar(kNvsBrightness, 200));  // Min 1 to avoid blank screen
    settings_.colorBogey = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBogey, 0xF800), 0xF800);
    settings_.colorFrequency = sanitizeRgb565Color(preferences_.getUShort(kNvsColorFreq, 0xF800), 0xF800);
    settings_.colorArrowFront = sanitizeRgb565Color(preferences_.getUShort(kNvsColorArrowFront, 0xF800), 0xF800);
    settings_.colorArrowSide = sanitizeRgb565Color(preferences_.getUShort(kNvsColorArrowSide, 0xF800), 0xF800);
    settings_.colorArrowRear = sanitizeRgb565Color(preferences_.getUShort(kNvsColorArrowRear, 0xF800), 0xF800);
    settings_.colorBandL = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBandLaser, 0x001F), 0x001F);
    settings_.colorBandKa = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBandKa, 0xF800), 0xF800);
    settings_.colorBandK = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBandK, 0x001F), 0x001F);
    settings_.colorBandX = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBandX, 0x07E0), 0x07E0);
    settings_.colorBandPhoto = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBandPhoto, 0x780F), 0x780F);  // Purple (photo radar)
    settings_.colorWiFiIcon = sanitizeRgb565Color(preferences_.getUShort(kNvsColorWifi, 0x07FF), 0x07FF);
    settings_.colorWiFiConnected = sanitizeRgb565Color(preferences_.getUShort(kNvsColorWifiConnected, 0x07E0), 0x07E0);
    settings_.colorBleConnected = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBleConnected, 0x07E0), 0x07E0);
    settings_.colorBleDisconnected = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBleDisconnected, 0x001F), 0x001F);
    settings_.colorBar1 = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBar1, 0x07E0), 0x07E0);
    settings_.colorBar2 = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBar2, 0x07E0), 0x07E0);
    settings_.colorBar3 = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBar3, 0xFFE0), 0xFFE0);
    settings_.colorBar4 = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBar4, 0xFFE0), 0xFFE0);
    settings_.colorBar5 = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBar5, 0xF800), 0xF800);
    settings_.colorBar6 = sanitizeRgb565Color(preferences_.getUShort(kNvsColorBar6, 0xF800), 0xF800);
    settings_.colorMuted = sanitizeRgb565Color(preferences_.getUShort(kNvsColorMuted, 0x3186), 0x3186);  // Dark grey muted color
    settings_.colorPersisted = sanitizeRgb565Color(preferences_.getUShort(kNvsColorPersisted, 0x18C3), 0x18C3);  // Darker grey for persisted alerts
    settings_.colorVolumeMain = sanitizeRgb565Color(preferences_.getUShort(kNvsColorVolumeMain, 0xF800), 0xF800);  // Red for main volume
    settings_.colorVolumeMute = sanitizeRgb565Color(preferences_.getUShort(kNvsColorVolumeMute, 0x7BEF), 0x7BEF);  // Grey for mute volume
    settings_.colorRssiV1 = sanitizeRgb565Color(preferences_.getUShort(kNvsColorRssiV1, 0x07E0), 0x07E0);       // Green for V1 RSSI label
    settings_.colorRssiProxy = sanitizeRgb565Color(preferences_.getUShort(kNvsColorRssiProxy, 0x001F), 0x001F);   // Blue for Proxy RSSI label
    settings_.colorObd = sanitizeRgb565Color(preferences_.getUShort(kNvsColorObd, 0x001F), 0x001F);              // Blue OBD badge color
    settings_.colorAlpConnected = sanitizeRgb565Color(preferences_.getUShort(kNvsColorAlpConn, 0x07E0), 0x07E0);  // Green ALP connected
    settings_.colorAlpDli = sanitizeRgb565Color(preferences_.getUShort(kNvsColorAlpDli, 0xFD20), 0xFD20);            // Orange ALP DLI active
    settings_.colorAlpLidActive = sanitizeRgb565Color(preferences_.getUShort(kNvsColorAlpLid, 0x001F), 0x001F);      // Blue ALP LID active
    settings_.colorAlpAlert = sanitizeRgb565Color(preferences_.getUShort(kNvsColorAlpAlert, 0xF800), 0xF800);        // Red ALP alert (solid during session)
    settings_.freqUseBandColor = preferences_.getBool(kNvsFreqBandColor, false);  // Use custom freq color by default
    settings_.hideWifiIcon = preferences_.getBool(kNvsHideWifi, false);
    settings_.hideProfileIndicator = preferences_.getBool(kNvsHideProfile, false);
    settings_.hideBatteryIcon = preferences_.getBool(kNvsHideBattery, false);
    settings_.showBatteryPercent = preferences_.getBool(kNvsBatteryPercent, false);
    settings_.hideBleIcon = preferences_.getBool(kNvsHideBle, false);
    settings_.hideVolumeIndicator = preferences_.getBool(kNvsHideVolume, false);
    settings_.hideRssiIndicator = preferences_.getBool(kNvsHideRssi, false);

    // Voice alert settings
    settings_.voiceAlertMode = clampVoiceAlertModeValue(preferences_.getUChar(kNvsVoiceMode, VOICE_MODE_BAND_FREQ));
    settings_.voiceDirectionEnabled = preferences_.getBool(kNvsVoiceDirection, true);
    settings_.announceBogeyCount = preferences_.getBool(kNvsVoiceBogeys, true);
    settings_.muteVoiceIfVolZero = preferences_.getBool(kNvsMuteVoiceAtVol0, false);
    settings_.voiceVolume = std::min<uint8_t>(100, preferences_.getUChar(kNvsVoiceVolume, 75));

    // Secondary alert settings
    settings_.announceSecondaryAlerts = preferences_.getBool(kNvsSecondaryAlerts, false);
    settings_.secondaryLaser = preferences_.getBool(kNvsSecondaryLaser, true);
    settings_.secondaryKa = preferences_.getBool(kNvsSecondaryKa, true);
    settings_.secondaryK = preferences_.getBool(kNvsSecondaryK, false);
    settings_.secondaryX = preferences_.getBool(kNvsSecondaryX, false);

    // Volume fade settings
    settings_.alertVolumeFadeEnabled = preferences_.getBool(kNvsVolFadeEnabled, false);
    settings_.alertVolumeFadeDelaySec = std::clamp<uint8_t>(preferences_.getUChar(kNvsVolFadeSeconds, 2), 1, 10);  // 1-10 seconds
    settings_.alertVolumeFadeVolume = std::clamp<uint8_t>(preferences_.getUChar(kNvsVolFadeVolume, 1), 1, 9);  // 1-9 (min 1 prevents V1 mute indicator feedback loop)

    // Speed-aware muting settings
    settings_.speedMuteEnabled = preferences_.getBool(kNvsSpeedMuteEnabled, false);
    settings_.speedMuteThresholdMph = std::clamp<uint8_t>(preferences_.getUChar(kNvsSpeedMuteThreshold, 25), 5, 60);
    settings_.speedMuteHysteresisMph = std::clamp<uint8_t>(preferences_.getUChar(kNvsSpeedMuteHysteresis, 3), 1, 10);
    {
        const uint8_t raw = preferences_.getUChar(kNvsSpeedMuteVolume, 0);
        if (raw == 0xFF) {
            settings_.speedMuteVolume = 0;
            settings_.speedMuteVoice = true;
        } else {
            settings_.speedMuteVolume = (raw <= 9) ? raw : 0;
            settings_.speedMuteVoice = preferences_.getBool(kNvsSpeedMuteVoice, true);
        }
    }
    settings_.stealthEnabled = preferences_.getBool(kNvsStealthEnabled, false);

    settings_.autoPushEnabled = preferences_.getBool(kNvsAutoPush, kDefaultAutoPushEnabled);
    settings_.activeSlot = preferences_.getInt(kNvsActiveSlot, 0);
    if (settings_.activeSlot < 0 || settings_.activeSlot > 2) {
        settings_.activeSlot = 0;
    }
    settings_.slot0Name = sanitizeSlotNameValue(preferences_.getString(kNvsSlot0Name, "DEFAULT"));
    settings_.slot1Name = sanitizeSlotNameValue(preferences_.getString(kNvsSlot1Name, "HIGHWAY"));
    settings_.slot2Name = sanitizeSlotNameValue(preferences_.getString(kNvsSlot2Name, "COMFORT"));
    settings_.slot0Color = sanitizeRgb565Color(preferences_.getUShort(kNvsSlot0Color, 0x400A), 0x400A);
    settings_.slot1Color = sanitizeRgb565Color(preferences_.getUShort(kNvsSlot1Color, 0x07E0), 0x07E0);
    settings_.slot2Color = sanitizeRgb565Color(preferences_.getUShort(kNvsSlot2Color, 0x8410), 0x8410);
    settings_.slot0Volume = clampSlotVolumeValue(preferences_.getUChar(kNvsSlot0Volume, 0xFF));
    settings_.slot1Volume = clampSlotVolumeValue(preferences_.getUChar(kNvsSlot1Volume, 0xFF));
    settings_.slot2Volume = clampSlotVolumeValue(preferences_.getUChar(kNvsSlot2Volume, 0xFF));
    settings_.slot0MuteVolume = clampSlotVolumeValue(preferences_.getUChar(kNvsSlot0MuteVolume, 0xFF));
    settings_.slot1MuteVolume = clampSlotVolumeValue(preferences_.getUChar(kNvsSlot1MuteVolume, 0xFF));
    settings_.slot2MuteVolume = clampSlotVolumeValue(preferences_.getUChar(kNvsSlot2MuteVolume, 0xFF));
    settings_.slot0DarkMode = preferences_.getBool(kNvsSlot0DarkMode, false);
    settings_.slot1DarkMode = preferences_.getBool(kNvsSlot1DarkMode, false);
    settings_.slot2DarkMode = preferences_.getBool(kNvsSlot2DarkMode, false);
    settings_.slot0MuteToZero = preferences_.getBool(kNvsSlot0MuteToZero, false);
    settings_.slot1MuteToZero = preferences_.getBool(kNvsSlot1MuteToZero, false);
    settings_.slot2MuteToZero = preferences_.getBool(kNvsSlot2MuteToZero, false);
    settings_.slot0AlertPersist = std::min<uint8_t>(5, preferences_.getUChar(kNvsSlot0Persistence, 0));
    settings_.slot1AlertPersist = std::min<uint8_t>(5, preferences_.getUChar(kNvsSlot1Persistence, 0));
    settings_.slot2AlertPersist = std::min<uint8_t>(5, preferences_.getUChar(kNvsSlot2Persistence, 0));
    settings_.slot0PriorityArrow = preferences_.getBool(kNvsSlot0PriorityArrow, false);
    settings_.slot1PriorityArrow = preferences_.getBool(kNvsSlot1PriorityArrow, false);
    settings_.slot2PriorityArrow = preferences_.getBool(kNvsSlot2PriorityArrow, false);
    settings_.slot0_default.profileName = sanitizeProfileNameValue(preferences_.getString(kNvsSlot0Profile, ""));
    settings_.slot0_default.mode = normalizeV1ModeValue(preferences_.getInt(kNvsSlot0Mode, V1_MODE_UNKNOWN));
    settings_.slot1_highway.profileName = sanitizeProfileNameValue(preferences_.getString(kNvsSlot1Profile, ""));
    settings_.slot1_highway.mode = normalizeV1ModeValue(preferences_.getInt(kNvsSlot1Mode, V1_MODE_UNKNOWN));
    settings_.slot2_comfort.profileName = sanitizeProfileNameValue(preferences_.getString(kNvsSlot2Profile, ""));
    settings_.slot2_comfort.mode = normalizeV1ModeValue(preferences_.getInt(kNvsSlot2Mode, V1_MODE_UNKNOWN));
    settings_.lastV1Address = sanitizeLastV1AddressValue(preferences_.getString(kNvsLastV1Address, ""));
    settings_.autoPowerOffMinutes = clampU8(preferences_.getUChar(kNvsAutoPowerOff, 0), 0, 60);
    settings_.apTimeoutMinutes = clampApTimeoutValue(preferences_.getUChar(kNvsApTimeout, 0));

    // OBD settings
    settings_.obdEnabled = preferences_.getBool(kNvsObdEnabled, false);
    settings_.obdSavedAddress = preferences_.getString(kNvsObdAddress, "");
    if (!isValidBleAddress(settings_.obdSavedAddress)) {
        Serial.printf("[Settings] WARN: Invalid OBD saved address in NVS: '%s' — clearing\n",
                      settings_.obdSavedAddress.c_str());
        settings_.obdSavedAddress = "";
    }
    settings_.obdSavedName = sanitizeObdSavedNameValue(preferences_.getString(kNvsObdName, ""));
    settings_.obdSavedAddrType = preferences_.getUChar(kNvsObdAddressType, 0);
    {
        const int rssi = static_cast<int>(preferences_.getChar(kNvsObdMinRssi, -90));
        settings_.obdMinRssi = static_cast<int8_t>(std::max(-100, std::min(rssi, -40)));
    }
    settings_.obdScanWindowMs = clampConnectionCycleObdScanWindowMsValue(
        static_cast<int64_t>(preferences_.getUInt(kNvsCycleObdScanWindow,
                                                  kConnectionCycleObdScanWindowMsDefault)));
    settings_.obdRetryIntervalMs = clampConnectionCycleObdRetryIntervalMsValue(
        static_cast<int64_t>(preferences_.getUInt(kNvsCycleObdRetryInt,
                                                  kConnectionCycleObdRetryIntervalMsDefault)));
    settings_.proxyOpenWindowMs = clampConnectionCycleProxyOpenWindowMsValue(
        static_cast<int64_t>(preferences_.getUInt(kNvsCycleProxyOpenWindow,
                                                  kConnectionCycleProxyOpenWindowMsDefault)));
    settings_.wifiOpenTimeoutMs = clampConnectionCycleWifiOpenTimeoutMsValue(
        static_cast<int64_t>(preferences_.getUInt(kNvsCycleWifiOpenTimeout,
                                                  kConnectionCycleWifiOpenTimeoutMsDefault)));
    settings_.v1SettleQuietMs = clampConnectionCycleV1SettleQuietMsValue(
        static_cast<int64_t>(preferences_.getUInt(kNvsCycleV1SettleQuiet,
                                                  kConnectionCycleV1SettleQuietMsDefault)));
    settings_.v1SettleFallbackMs = clampConnectionCycleV1SettleFallbackMsValue(
        static_cast<int64_t>(preferences_.getUInt(kNvsCycleV1SettleFallback,
                                                  kConnectionCycleV1SettleFallbackMsDefault)));
    settings_.cycleTeardownAckTimeoutMs = clampConnectionCycleTeardownAckTimeoutMsValue(
        static_cast<int64_t>(preferences_.getUInt(kNvsCycleTeardownAckTimeout,
                                                  kConnectionCycleTeardownAckTimeoutMsDefault)));

    if (settings_.proxyBLE && settings_.obdEnabled) {
        // Legacy migration: older builds allowed both. OBD was the explicit
        // opt-in setting while proxy defaulted on, so preserve standalone OBD
        // behavior and disable passive proxy to avoid triple-BLE DMA pressure.
        Serial.println("[Settings] HEAL: proxyBLE and obdEnabled both true — keeping OBD, disabling proxy");
        settings_.proxyBLE = false;
    }

    // ALP settings
    settings_.alpEnabled = preferences_.getBool(kNvsAlpEnabled, false);
    settings_.alpSdLogEnabled = preferences_.getBool(kNvsAlpSdLog, false);
    {
        uint8_t alpPersist = preferences_.getUChar(kNvsAlpPersistSec, 0);
        if (alpPersist > 5) alpPersist = 5;
        settings_.alpAlertPersistSec = alpPersist;
    }
    settings_.alpDisableV1LaserOnPush = preferences_.getBool(kNvsAlpNoV1Laser, true);

    // Debug / diagnostics
    settings_.powerOffSdLog = preferences_.getBool(kNvsPowerOffSdLog, false);

    // GPS settings
    settings_.gpsEnabled = preferences_.getBool(kNvsGpsEnabled, false);
    {
        const uint32_t baud = preferences_.getUInt(kNvsGpsBaud, 9600);
        settings_.gpsBaud = (baud == 9600 || baud == 38400 || baud == 115200) ? baud : 9600;
    }
    settings_.gpsEnablePinActiveHigh = preferences_.getBool(kNvsGpsEnablePolarity, true);
    settings_.gpsLogUtcToPerf = preferences_.getBool(kNvsGpsLogUtcToPerf, true);
    settings_.gpsLogUtcToAlp  = preferences_.getBool(kNvsGpsLogUtcToAlp,  true);

    preferences_.end();
    migrateLegacyWifiStaSlotNvs(activeNs, settings_.wifiStaSlots[0], wifiClientSsidKeyPresent);

    Serial.printf("[Settings] OK wifi=%s proxy=%s bright=%d autoPush=%s\n",
                  settings_.enableWifi ? "on" : "off",
                  settings_.proxyBLE ? "on" : "off",
                  settings_.brightness,
                  settings_.autoPushEnabled ? "on" : "off");
}

void SettingsManager::save() {
    if (!persistSettingsAtomically()) {
        return;
    }

    clearDeferredPersistState();
    bumpBackupRevision();
    Serial.println("Settings saved atomically");

    // Backup display settings to SD card (survives reflash)
    backupToSD();
}

void SettingsManager::requestDeferredPersist() {
    deferredPersistPending_ = true;
    deferredPersistRetryScheduled_ = false;
    deferredPersistNextAttemptAtMs_ = millis() + SETTINGS_DEFERRED_PERSIST_DEBOUNCE_MS;
}

bool SettingsManager::deferredPersistPending() const {
    return deferredPersistPending_;
}

bool SettingsManager::deferredPersistRetryScheduled() const {
    return deferredPersistRetryScheduled_;
}

uint32_t SettingsManager::deferredPersistNextAttemptAtMs() const {
    return deferredPersistNextAttemptAtMs_;
}

void SettingsManager::serviceDeferredPersist(uint32_t nowMs) {
    if (!deferredPersistPending_) {
        return;
    }

    if (deferredPersistNextAttemptAtMs_ != 0 &&
        !isDeferredPersistDue(nowMs, deferredPersistNextAttemptAtMs_)) {
        return;
    }

    if (!persistSettingsAtomically()) {
        deferredPersistPending_ = true;
        deferredPersistRetryScheduled_ = true;
        deferredPersistNextAttemptAtMs_ = nowMs + SETTINGS_DEFERRED_PERSIST_RETRY_BACKOFF_MS;
        return;
    }

    clearDeferredPersistState();
    bumpBackupRevision();
    Serial.println("Settings saved atomically");
    requestDeferredBackupFromCurrentState();
}

SettingsManager::NvsDiagnostic SettingsManager::getNvsDiagnostic() const {
    NvsDiagnostic diag;
    diag.activeNamespace = const_cast<SettingsManager*>(this)->getActiveNamespace();

    Preferences prefs;
    if (prefs.begin(diag.activeNamespace.c_str(), true)) {
        diag.nvsValidMarker = prefs.getInt(kNvsValid, 0);
        diag.settingsVersion = prefs.getInt(kNvsSettingsVer, 0);
        diag.nvsBrightness = prefs.getUChar(kNvsBrightness, 0);
        diag.nvsProxyBle = prefs.getBool(kNvsProxyBle, false);
        diag.nvsAutoPush = prefs.getBool(kNvsAutoPush, kDefaultAutoPushEnabled);
        prefs.end();
        diag.healthy = (diag.nvsValidMarker == SETTINGS_VERSION);
    }

    return diag;
}

// Check if NVS appears to be in default state (likely erased during reflash)
