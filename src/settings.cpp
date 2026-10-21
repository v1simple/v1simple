/**
 * SECURITY NOTE: WiFi passwords are stored with XOR obfuscation, NOT encryption.
 * This is intentional - it prevents casual viewing in hex dumps but is NOT secure
 * against someone with physical access to the device who is determined to recover them.
 *
 * For this use case (a car accessory on a private network), the trade-off is:
 * - Pro: Simple, no crypto library overhead, recoverable if key changes
 * - Con: Not suitable for high-security applications
 *
 * If stronger security is needed, consider ESP32 NVS encryption (requires flash
 * encryption key management) or storing a hash instead of the actual password.
 */

#include "display_visual_contract.h"
#include "settings_internals.h"
#include "settings_backup_revision.h"
#include "settings_nvs_reader.h"
#include <nvs.h>

const char* SETTINGS_BACKUP_PATH = "/v1simple_backup.json";
const char* SETTINGS_BACKUP_TMP_PATH = "/v1simple_backup.tmp";
const char* SETTINGS_BACKUP_PREV_PATH = "/v1simple_backup.prev";
// Ten maximal schema-v3 profiles plus the complete settings envelope measure
// 120,640 bytes.  The shared 128 KiB transport/journal/SD bound retains more
// than one 4 KiB allocation quantum and rejects unsupported padding before a
// larger PSRAM allocation.
const size_t SETTINGS_BACKUP_MAX_BYTES = 128 * 1024;
const char* WIFI_CLIENT_NS = kSettingsWifiClientNamespace;
const char* WIFI_CLIENT_SD_SECRET_PATH = "/v1wifi_secret.json";
const char* WIFI_CLIENT_SD_SECRET_TYPE = "v1wifi_secret";
const int WIFI_CLIENT_SD_SECRET_VERSION = 2;
const char* const SETTINGS_BACKUP_CANDIDATES[] = {SETTINGS_BACKUP_PATH, SETTINGS_BACKUP_PREV_PATH,
                                                  "/v1simple_settings.json", "/v1settings_backup.json"};
const size_t SETTINGS_BACKUP_CANDIDATES_COUNT =
    sizeof(SETTINGS_BACKUP_CANDIDATES) / sizeof(SETTINGS_BACKUP_CANDIDATES[0]);

VoiceAlertMode clampVoiceAlertModeValue(int raw) {
    int clamped =
        std::max(static_cast<int>(VOICE_MODE_DISABLED), std::min(raw, static_cast<int>(VOICE_MODE_BAND_FREQ)));
    return static_cast<VoiceAlertMode>(clamped);
}

static constexpr size_t MAX_V1_ADDRESS_LEN = 32;
static constexpr size_t MAX_OBD_SAVED_NAME_LEN = 32;
static constexpr uint32_t SETTINGS_DEFERRED_PERSIST_DEBOUNCE_MS = 750;
static constexpr uint32_t SETTINGS_DEFERRED_PERSIST_RETRY_BACKOFF_MS = 1000;

static_assert(kWifiStaSlotCount == kNvsWifiStaSlotCount, "WiFi STA slot model and NVS key arrays must stay in sync");

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
    String value = clampStringLength(raw, MAX_V1_ADDRESS_LEN);
    value.trim();
    value.toUpperCase();
    return value;
}

String sanitizeObdSavedNameValue(const String& raw) {
    String value = clampStringLength(raw, MAX_OBD_SAVED_NAME_LEN);
    value.trim();
    return value;
}

static void migrateLegacyWifiStaSlotNvs(const String& activeNs, const WifiStaSlot& slot0, bool legacySsidKeyPresent) {
    if (activeNs.length() == 0 || slot0.ssid.length() == 0) {
        return;
    }

    if (legacySsidKeyPresent) {
        Preferences settingsPrefs;
        if (settingsPrefs.begin(activeNs.c_str(), false)) {
            // Publish the SSID last so an interrupted copy still loads the legacy
            // slot defaults. Keep its source until every destination write commits.
            const bool copied =
                settingsPrefs.putString(kNvsWifiStaSlotLabel[0], slot0.label) == slot0.label.length() &&
                settingsPrefs.isKey(kNvsWifiStaSlotLabel[0]) &&
                settingsPrefs.getString(kNvsWifiStaSlotLabel[0], "") == slot0.label &&
                settingsPrefs.putUChar(kNvsWifiStaSlotPriority[0], slot0.priority) == sizeof(slot0.priority) &&
                settingsPrefs.putUInt(kNvsWifiStaSlotLastConnected[0], slot0.lastConnectedAtSec) ==
                    sizeof(slot0.lastConnectedAtSec) &&
                settingsPrefs.putString(kNvsWifiStaSlotSsid[0], slot0.ssid) == slot0.ssid.length() &&
                settingsPrefs.getString(kNvsWifiStaSlotSsid[0], "") == slot0.ssid;
            if (copied && settingsPrefs.remove(kNvsWifiClientSsid)) {
                Serial.println("[Settings] Migrated legacy WiFi client SSID into STA slot 0");
            }
            settingsPrefs.end();
        }
    }

    Preferences wifiPrefs;
    if (!wifiPrefs.begin(WIFI_CLIENT_NS, false)) {
        return;
    }
    const bool legacyPasswordKeyPresent = wifiPrefs.isKey(kNvsWifiPassword);
    if (legacyPasswordKeyPresent) {
        // isKey() also reports false when Preferences cannot read a key's type.
        // Only proven absence permits replacing the slot credential.
        nvs_handle_t handle = 0;
        if (nvs_open(WIFI_CLIENT_NS, NVS_READONLY, &handle) != ESP_OK) {
            wifiPrefs.end();
            return;
        }
        size_t destinationBytes = 0;
        const esp_err_t destinationStatus =
            nvs_get_str(handle, kNvsWifiStaSlotPassword[0], nullptr, &destinationBytes);
        nvs_close(handle);
        // Preferences returns the empty default on a failed read too. Check the
        // stored length before copying or retiring either credential source.
        const auto readPassword = [&](const char* key, String& password) {
            const size_t storedBytes = wifiPrefs.getStringLength(key);
            if (storedBytes == 0) return false;
            password = wifiPrefs.getString(key, "");
            return password.length() + 1u == storedBytes;
        };
        String password;
        bool copied = false;
        if (destinationStatus == ESP_OK) {
            // A previously committed slot value remains authoritative.
            copied = readPassword(kNvsWifiStaSlotPassword[0], password);
        } else if (destinationStatus == ESP_ERR_NVS_NOT_FOUND && readPassword(kNvsWifiPassword, password)) {
            // A successful empty-string write returns zero too; verify key presence
            // and readback before removing the legacy open-network credential.
            String readBack;
            copied = wifiPrefs.putString(kNvsWifiStaSlotPassword[0], password) == password.length() &&
                     readPassword(kNvsWifiStaSlotPassword[0], readBack) && readBack == password;
        }
        if (copied && wifiPrefs.remove(kNvsWifiPassword)) {
            Serial.println("[Settings] Migrated legacy WiFi client password into STA slot 0");
        }
    }
    wifiPrefs.end();
}

SettingsManager::SettingsManager(StorageManager& storage, V1ProfileManager& profiles)
    : storage_(&storage), profiles_(&profiles) {}

void SettingsManager::noteNvsCommitWithoutBackupIntent() {
    backupCompletedRevision_ = readBackupRevisionCompleted();
    backupRevisionCounter_ =
        settings_backup_revision::nextFresh(backupRevisionCounter_, backupDueRevision_, backupCompletedRevision_);
}

bool SettingsManager::persistSettingsWithBackupIntent() {
    if (!ensureSettingsLoaded()) return false;
    const uint32_t previousCounter = backupRevisionCounter_;
    const uint32_t previousDue = backupDueRevision_;
    const uint32_t previousCompleted = backupCompletedRevision_;
    backupCompletedRevision_ = readBackupRevisionCompleted();
    backupRevisionCounter_ =
        settings_backup_revision::nextFresh(backupRevisionCounter_, backupDueRevision_, backupCompletedRevision_);
    backupDueRevision_ = backupRevisionCounter_;
    if (persistSettingsAtomically()) {
        return true;
    }
    backupRevisionCounter_ = previousCounter;
    backupDueRevision_ = previousDue;
    backupCompletedRevision_ = previousCompleted;
    return false;
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

    // A read-only Preferences open reports NOT_FOUND for an unused namespace.
    // Materialize the runtime namespace once so "no deletion intent" remains
    // distinguishable from a genuine NVS access failure at boot.
    Preferences v1RuntimeNs;
    if (v1RuntimeNs.begin(kSettingsV1RuntimeNamespace, false)) {
        v1RuntimeNs.end();
    } else {
        Serial.println("[Settings] WARN: Failed to initialize V1 runtime namespace");
    }

    load();
    resolveWifiCredentialTransaction();

    // Note: SD card may not be mounted yet during begin().
    // checkAndRestoreFromSD() should be called after storage is ready.
    // We still try here in case storage was already initialized.
    checkAndRestoreFromSD();
}

// Keep text transforms in the provisional snapshot and check any allocation
// before a shorter/empty result can become an authoritative setting.
static String loadedText(SettingsNvsReader& reader, String value, size_t maxBytes,
                         bool trim = false, bool uppercase = false, const char* emptyDefault = "") {
    if (value.length() > maxBytes) {
        String prefix = value.substring(0, maxBytes);
        reader.requireExactString(prefix.length() == maxBytes &&
                                  std::memcmp(prefix.c_str(), value.c_str(), maxBytes) == 0);
        value = std::move(prefix);
    }
    if (trim) value.trim();
    if (uppercase) value.toUpperCase();
    if (value.length() == 0 && emptyDefault[0]) {
        value = emptyDefault;
        reader.requireExactString(value.length() == std::strlen(emptyDefault));
    }
    return value;
}

static String loadedProfileName(SettingsNvsReader& reader, String raw) {
    raw.trim();
    String canonical;
    const ProfileNameStatus status = canonicalizeProfileName(raw, canonical);
    reader.requireExactString(canonical == raw);
    return status == ProfileNameStatus::Valid ? std::move(canonical) : String();
}

static String loadedApPassword(SettingsNvsReader& reader, String stored, int version) {
    if (version >= 2 && stored.length() > 0) {
        bool validEncoding = true;
        size_t expected = stored.length();
        if (stored.startsWith(OBFUSCATION_HEX_PREFIX)) {
            const size_t prefix = std::strlen(OBFUSCATION_HEX_PREFIX);
            expected = (stored.length() - prefix) / 2;
            validEncoding = (stored.length() - prefix) % 2 == 0;
            for (size_t i = prefix; i < stored.length(); ++i) {
                validEncoding &= std::isxdigit(static_cast<unsigned char>(stored[i])) != 0;
            }
        }
        String decoded = decodeObfuscatedFromStorage(stored);
        if (validEncoding) reader.requireExactString(decoded.length() == expected);
        stored = std::move(decoded);
    }
    stored = loadedText(reader, std::move(stored), MAX_AP_PASSWORD_LEN);
    if (stored.length() < MIN_AP_PASSWORD_LEN) {
        stored = "setupv1simple";
        reader.requireExactString(stored == "setupv1simple");
    }
    return stored;
}

static void loadSignalBarColors(SettingsNvsReader& reader, V1Settings& loaded);

void SettingsManager::load() {
    nvsLoadPending_ = true;
    bool namespacePresent = false;
    const String activeNs = getActiveNamespace(nullptr, &namespacePresent);
    if (activeNs.length() == 0) {
        nvsRecoveryPending_ = true;
        return;
    }
    if (!namespacePresent) {
        // Proven empty NVS is an ordinary first boot, not an access failure.
        nvsLoadPending_ = false;
        return;
    }
    SettingsNvsReader reader(activeNs.c_str());
    std::unique_ptr<V1Settings> snapshot(new (std::nothrow) V1Settings());
    if (!reader.good() || !snapshot) {
        Serial.printf("[Settings] NVS load unavailable for %s; preserving durable copies\n", activeNs.c_str());
        nvsRecoveryPending_ = true;
        return;
    }

    V1Settings& loaded = *snapshot;
    int storedVersion = reader.getInt(kNvsSettingsVer, 1);
    const bool restorePending = reader.getBool(kNvsRestorePending, false);
    const int64_t storedRestoreWatermark = reader.getLong64(kNvsRestoreCommitWatermark, 0);
    const int64_t storedDeleteWatermark = reader.getLong64(kNvsProfileDeleteCommitWatermark, 0);
    const uint32_t wifiCredentialCommitGeneration = reader.getUInt(kNvsWifiCredentialCommitGeneration, 0);
    if (restorePending) {
        Serial.println("[Settings] NVS marked restore-pending; SD backup remains authoritative");
    }

    loaded.apPassword = loadedApPassword(reader, reader.getString(kNvsApPassword, ""), storedVersion);
    if (storedVersion < 2) Serial.println("[Settings] Migrating from v1 to v2 (password obfuscation)");
    loaded.apSSID = loadedText(reader, reader.getString(kNvsApSsid, "V1-Simple"), MAX_WIFI_SSID_LEN,
                              false, false, "V1-Simple");

    const bool wifiClientEnabledKeyPresent = reader.isKey(kNvsWifiClientEnabled);
    const bool wifiClientSsidKeyPresent = reader.isKey(kNvsWifiClientSsid);
    loaded.wifiClientEnabled = reader.getBool(kNvsWifiClientEnabled, false);
    const String legacyWifiClientSsid =
        wifiClientSsidKeyPresent ? loadedText(reader, reader.getString(kNvsWifiClientSsid, ""), MAX_WIFI_SSID_LEN) : "";
    for (size_t i = 0; i < kWifiStaSlotCount; ++i) {
        WifiStaSlot& slot = loaded.wifiStaSlots[i];
        slot.ssid = loadedText(reader, reader.getString(kNvsWifiStaSlotSsid[i], ""), MAX_WIFI_SSID_LEN);
        slot.label = loadedText(reader, reader.getString(kNvsWifiStaSlotLabel[i], ""), MAX_WIFI_STA_LABEL_LEN, true);
        const uint8_t missingPriority = slot.isConfigured() ? static_cast<uint8_t>(i) : slot.priority;
        slot.priority = reader.getUChar(kNvsWifiStaSlotPriority[i], missingPriority);
        // Installed releases stored boot-relative seconds under this key. No
        // clock exists to reconstruct their cross-boot chronology, so retain
        // their numeric order as an opaque migration hint. Every subsequently
        // recorded saved-slot connection advances above the persisted maximum,
        // making new ordering durable across reboot without rewriting compatible data.
        slot.lastConnectedAtSec = reader.getUInt(kNvsWifiStaSlotLastConnected[i], 0);
    }

    if (!loaded.hasConfiguredWifiStaSlot() && legacyWifiClientSsid.length() > 0) {
        loaded.wifiStaSlots[0].ssid = legacyWifiClientSsid;
        loaded.wifiStaSlots[0].label = "Saved";
        reader.requireExactString(loaded.wifiStaSlots[0].ssid == legacyWifiClientSsid &&
                                  loaded.wifiStaSlots[0].label == "Saved");
        loaded.wifiStaSlots[0].priority = 0;
        loaded.wifiStaSlots[0].lastConnectedAtSec = 0;
    }

    // Self-healing for legacy/incomplete NVS only: older settings could contain
    // a saved STA SSID without the explicit enabled flag.  If the flag is
    // present, preserve it exactly so the Web UI's WiFi Client OFF toggle
    // stays OFF across reboot while keeping saved networks.
    if (!wifiClientEnabledKeyPresent && !loaded.wifiClientEnabled && loaded.hasConfiguredWifiStaSlot()) {
        Serial.println("[Settings] HEAL: wifiClientEnabled flag missing but SSID is set — enabling");
        loaded.wifiClientEnabled = true;
    }
    loaded.refreshWifiClientAliasFromSlots();
    const WifiStaSlot* primary = loaded.primaryWifiStaSlot();
    reader.requireExactString(primary ? loaded.wifiClientSSID == primary->ssid : loaded.wifiClientSSID.length() == 0);

    Serial.printf("[Settings] WiFi client keys: enabledKey=%s ssidKey=%s\n", wifiClientEnabledKeyPresent ? "yes" : "no",
                  wifiClientSsidKeyPresent ? "yes" : "no");
    Serial.printf("[Settings] WiFi client: enabled=%s, configured=%s\n", loaded.wifiClientEnabled ? "true" : "false",
                  loaded.wifiClientSSID.length() > 0 ? "yes" : "no");

    loaded.proxyBLE = reader.getBool(kNvsProxyBle, true);
    loaded.proxyName = loadedText(reader, reader.getString(kNvsProxyName, "V1-Proxy"), MAX_PROXY_NAME_LEN, false, false, "V1-Proxy");
    loaded.brightness =
        std::max<uint8_t>(1, reader.getUChar(kNvsBrightness, 200)); // Min 1 to avoid blank screen
    loaded.colorBogey = sanitizeRgb565Color(reader.getUShort(kNvsColorBogey, 0xF800), 0xF800);
    loaded.colorFrequency = sanitizeRgb565Color(reader.getUShort(kNvsColorFreq, 0xF800), 0xF800);
    loaded.colorArrowFront = sanitizeRgb565Color(reader.getUShort(kNvsColorArrowFront, 0xF800), 0xF800);
    loaded.colorArrowSide = sanitizeRgb565Color(reader.getUShort(kNvsColorArrowSide, 0xF800), 0xF800);
    loaded.colorArrowRear = sanitizeRgb565Color(reader.getUShort(kNvsColorArrowRear, 0xF800), 0xF800);
    loaded.colorBandL = sanitizeRgb565Color(reader.getUShort(kNvsColorBandLaser, 0x001F), 0x001F);
    loaded.colorBandKa = sanitizeRgb565Color(reader.getUShort(kNvsColorBandKa, 0xF800), 0xF800);
    loaded.colorBandK = sanitizeRgb565Color(reader.getUShort(kNvsColorBandK, 0x001F), 0x001F);
    loaded.colorBandX = sanitizeRgb565Color(reader.getUShort(kNvsColorBandX, 0x07E0), 0x07E0);
    loaded.colorBandPhoto =
        sanitizeRgb565Color(reader.getUShort(kNvsColorBandPhoto, 0x780F), 0x780F); // Purple (photo radar)
    loaded.colorWiFiConnected = sanitizeRgb565Color(reader.getUShort(kNvsColorWifiConnected, 0x07E0), 0x07E0);
    loaded.colorBleConnected = sanitizeRgb565Color(reader.getUShort(kNvsColorBleConnected, 0x07E0), 0x07E0);
    loaded.colorBleDisconnected =
        sanitizeRgb565Color(reader.getUShort(kNvsColorBleDisconnected, 0x001F), 0x001F);
    loadSignalBarColors(reader, loaded);
    loaded.colorMuted =
        sanitizeRgb565Color(reader.getUShort(kNvsColorMuted, 0x4A49), 0x4A49); // Subdued grey muted color
    loaded.colorPersisted = sanitizeRgb565Color(reader.getUShort(kNvsColorPersisted, 0x4208),
                                                   0x4208); // Subdued grey for persisted alerts
    loaded.colorVolumeMain =
        sanitizeRgb565Color(reader.getUShort(kNvsColorVolumeMain, 0xF800), 0xF800); // Red for main volume
    loaded.colorVolumeMute =
        sanitizeRgb565Color(reader.getUShort(kNvsColorVolumeMute, 0x7BEF), 0x7BEF); // Grey for mute volume
    loaded.colorRssiV1 =
        sanitizeRgb565Color(reader.getUShort(kNvsColorRssiV1, 0x07E0), 0x07E0); // Green for V1 RSSI label
    loaded.colorRssiProxy =
        sanitizeRgb565Color(reader.getUShort(kNvsColorRssiProxy, 0x001F), 0x001F); // Blue for Proxy RSSI label
    loaded.colorObd =
        sanitizeRgb565Color(reader.getUShort(kNvsColorObd, 0x001F), 0x001F); // Blue OBD badge color
    loaded.colorAlpConnected =
        sanitizeRgb565Color(reader.getUShort(kNvsColorAlpConn, 0x07E0), 0x07E0); // Green ALP connected
    loaded.colorAlpDli =
        sanitizeRgb565Color(reader.getUShort(kNvsColorAlpDli, 0xFD20), 0xFD20); // Orange ALP DLI active
    loaded.colorAlpLidActive =
        sanitizeRgb565Color(reader.getUShort(kNvsColorAlpLid, 0x001F), 0x001F); // Blue ALP LID active
    loaded.colorAlpAlert = sanitizeRgb565Color(reader.getUShort(kNvsColorAlpAlert, 0xF800),
                                                  0xF800);                       // Red ALP alert (solid during session)
    loaded.freqUseBandColor = reader.getBool(kNvsFreqBandColor, false); // Use custom freq color by default
    loaded.hideWifiIcon = reader.getBool(kNvsHideWifi, false);
    loaded.hideProfileIndicator = reader.getBool(kNvsHideProfile, false);
    loaded.hideBatteryIcon = reader.getBool(kNvsHideBattery, false);
    loaded.showBatteryPercent = reader.getBool(kNvsBatteryPercent, false);
    loaded.hideBleIcon = reader.getBool(kNvsHideBle, false);
    loaded.hideVolumeIndicator = reader.getBool(kNvsHideVolume, false);
    loaded.hideRssiIndicator = reader.getBool(kNvsHideRssi, false);

    loaded.voiceAlertMode = clampVoiceAlertModeValue(reader.getUChar(kNvsVoiceMode, VOICE_MODE_BAND_FREQ));
    loaded.voiceDirectionEnabled = reader.getBool(kNvsVoiceDirection, true);
    loaded.announceBogeyCount = reader.getBool(kNvsVoiceBogeys, true);
    loaded.muteVoiceIfVolZero = reader.getBool(kNvsMuteVoiceAtVol0, false);
    loaded.voiceVolume = std::min<uint8_t>(100, reader.getUChar(kNvsVoiceVolume, 75));

    loaded.announceSecondaryAlerts = reader.getBool(kNvsSecondaryAlerts, false);
    loaded.secondaryLaser = reader.getBool(kNvsSecondaryLaser, true);
    loaded.secondaryKa = reader.getBool(kNvsSecondaryKa, true);
    loaded.secondaryK = reader.getBool(kNvsSecondaryK, false);
    loaded.secondaryX = reader.getBool(kNvsSecondaryX, false);

    loaded.alertVolumeFadeEnabled = reader.getBool(kNvsVolFadeEnabled, false);
    loaded.alertVolumeFadeDelaySec =
        std::clamp<uint8_t>(reader.getUChar(kNvsVolFadeSeconds, 2), 1, 10); // 1-10 seconds
    loaded.alertVolumeFadeVolume = std::clamp<uint8_t>(reader.getUChar(kNvsVolFadeVolume, 1), 1,
                                                          9); // 1-9 (min 1 prevents V1 mute indicator feedback loop)

    loaded.speedMuteEnabled = reader.getBool(kNvsSpeedMuteEnabled, false);
    loaded.speedMuteThresholdMph = std::clamp<uint8_t>(reader.getUChar(kNvsSpeedMuteThreshold, 25), 5, 60);
    loaded.speedMuteHysteresisMph = std::clamp<uint8_t>(reader.getUChar(kNvsSpeedMuteHysteresis, 3), 1, 10);
    {
        const uint8_t raw = reader.getUChar(kNvsSpeedMuteVolume, 0);
        if (raw == 0xFF) {
            loaded.speedMuteVolume = 0;
            loaded.speedMuteVoice = true;
        } else {
            loaded.speedMuteVolume = (raw <= 9) ? raw : 0;
            loaded.speedMuteVoice = reader.getBool(kNvsSpeedMuteVoice, true);
        }
    }
    loaded.stealthEnabled = reader.getBool(kNvsStealthEnabled, false);

    loaded.autoPushEnabled = reader.getBool(kNvsAutoPush, kDefaultAutoPushEnabled);
    {
        const uint8_t storedProfileSchema = reader.getUChar(kNvsAutoPushProfileSchema, 0);
        loaded.autoPushProfileSchemaVersion =
            isVersionedV1ProfileSchema(storedProfileSchema)
                ? storedProfileSchema
                : 0;
    }
    loaded.activeSlot = reader.getInt(kNvsActiveSlot, 0);
    if (loaded.activeSlot < 0 || loaded.activeSlot > 2) {
        loaded.activeSlot = 0;
    }
    loaded.slot0Name = loadedText(reader, reader.getString(kNvsSlot0Name, "DEFAULT"), MAX_SLOT_NAME_LEN, false, true);
    loaded.slot1Name = loadedText(reader, reader.getString(kNvsSlot1Name, "HIGHWAY"), MAX_SLOT_NAME_LEN, false, true);
    loaded.slot2Name = loadedText(reader, reader.getString(kNvsSlot2Name, "COMFORT"), MAX_SLOT_NAME_LEN, false, true);
    loaded.slot0Color = sanitizeRgb565Color(reader.getUShort(kNvsSlot0Color, 0x400A), 0x400A);
    loaded.slot1Color = sanitizeRgb565Color(reader.getUShort(kNvsSlot1Color, 0x07E0), 0x07E0);
    loaded.slot2Color = sanitizeRgb565Color(reader.getUShort(kNvsSlot2Color, 0x8410), 0x8410);
    loaded.slot0Volume = clampSlotVolumeValue(reader.getUChar(kNvsSlot0Volume, 0xFF));
    loaded.slot1Volume = clampSlotVolumeValue(reader.getUChar(kNvsSlot1Volume, 0xFF));
    loaded.slot2Volume = clampSlotVolumeValue(reader.getUChar(kNvsSlot2Volume, 0xFF));
    loaded.slot0MuteVolume = clampSlotVolumeValue(reader.getUChar(kNvsSlot0MuteVolume, 0xFF));
    loaded.slot1MuteVolume = clampSlotVolumeValue(reader.getUChar(kNvsSlot1MuteVolume, 0xFF));
    loaded.slot2MuteVolume = clampSlotVolumeValue(reader.getUChar(kNvsSlot2MuteVolume, 0xFF));
    sanitizeSlotVolumePair(loaded.slot0Volume, loaded.slot0MuteVolume);
    sanitizeSlotVolumePair(loaded.slot1Volume, loaded.slot1MuteVolume);
    sanitizeSlotVolumePair(loaded.slot2Volume, loaded.slot2MuteVolume);
    loaded.slot0DarkMode = reader.getBool(kNvsSlot0DarkMode, false);
    loaded.slot1DarkMode = reader.getBool(kNvsSlot1DarkMode, false);
    loaded.slot2DarkMode = reader.getBool(kNvsSlot2DarkMode, false);
    loaded.slot0VolumeOverride = reader.getBool(kNvsSlot0VolumeOverride, false);
    loaded.slot1VolumeOverride = reader.getBool(kNvsSlot1VolumeOverride, false);
    loaded.slot2VolumeOverride = reader.getBool(kNvsSlot2VolumeOverride, false);
    loaded.slot0VolumeOverride &= loaded.slot0Volume <= 9 && loaded.slot0MuteVolume <= 9;
    loaded.slot1VolumeOverride &= loaded.slot1Volume <= 9 && loaded.slot1MuteVolume <= 9;
    loaded.slot2VolumeOverride &= loaded.slot2Volume <= 9 && loaded.slot2MuteVolume <= 9;
    loaded.slot0DarkModeOverride = reader.getBool(kNvsSlot0DarkModeOverride, false);
    loaded.slot1DarkModeOverride = reader.getBool(kNvsSlot1DarkModeOverride, false);
    loaded.slot2DarkModeOverride = reader.getBool(kNvsSlot2DarkModeOverride, false);
    loaded.slot0MuteToZero = reader.getBool(kNvsSlot0MuteToZero, false);
    loaded.slot1MuteToZero = reader.getBool(kNvsSlot1MuteToZero, false);
    loaded.slot2MuteToZero = reader.getBool(kNvsSlot2MuteToZero, false);
    loaded.slot0AlertPersist = std::min<uint8_t>(5, reader.getUChar(kNvsSlot0Persistence, 0));
    loaded.slot1AlertPersist = std::min<uint8_t>(5, reader.getUChar(kNvsSlot1Persistence, 0));
    loaded.slot2AlertPersist = std::min<uint8_t>(5, reader.getUChar(kNvsSlot2Persistence, 0));
    loaded.slot0PriorityArrow = reader.getBool(kNvsSlot0PriorityArrow, false);
    loaded.slot1PriorityArrow = reader.getBool(kNvsSlot1PriorityArrow, false);
    loaded.slot2PriorityArrow = reader.getBool(kNvsSlot2PriorityArrow, false);
    loaded.slot0_default.profileName = loadedProfileName(reader, reader.getString(kNvsSlot0Profile, ""));
    loaded.slot0_default.mode = normalizeV1ModeValue(reader.getInt(kNvsSlot0Mode, V1_MODE_UNKNOWN));
    loaded.slot1_highway.profileName = loadedProfileName(reader, reader.getString(kNvsSlot1Profile, ""));
    loaded.slot1_highway.mode = normalizeV1ModeValue(reader.getInt(kNvsSlot1Mode, V1_MODE_UNKNOWN));
    loaded.slot2_comfort.profileName = loadedProfileName(reader, reader.getString(kNvsSlot2Profile, ""));
    loaded.slot2_comfort.mode = normalizeV1ModeValue(reader.getInt(kNvsSlot2Mode, V1_MODE_UNKNOWN));
    loaded.lastV1Address = reader.isKey(kNvsLastV1Address)
                                  ? loadedText(reader, reader.getString(kNvsLastV1Address, ""), MAX_V1_ADDRESS_LEN, true, true)
                                  : "";
    loaded.autoPowerOffMinutes = clampU8(reader.getUChar(kNvsAutoPowerOff, 0), 0, 60);
    loaded.apTimeoutMinutes = clampApTimeoutValue(reader.getUChar(kNvsApTimeout, 0));

    loaded.obdEnabled = reader.getBool(kNvsObdEnabled, false);
    loaded.obdSavedAddress = reader.getString(kNvsObdAddress, "");
    if (!isValidBleAddress(loaded.obdSavedAddress)) {
        Serial.println("[Settings] WARN: Invalid OBD saved address in NVS — clearing");
        loaded.obdSavedAddress = "";
    }
    loaded.obdSavedName = loadedText(reader, reader.getString(kNvsObdName, ""), MAX_OBD_SAVED_NAME_LEN, true);
    loaded.obdSavedAddrType = reader.getUChar(kNvsObdAddressType, 0);
    {
        const int rssi = static_cast<int>(reader.getChar(kNvsObdMinRssi, -90));
        loaded.obdMinRssi = static_cast<int8_t>(std::max(-100, std::min(rssi, -40)));
    }
    loaded.obdScanWindowMs = clampConnectionCycleObdScanWindowMsValue(
        static_cast<int64_t>(reader.getUInt(kNvsCycleObdScanWindow, kConnectionCycleObdScanWindowMsDefault)));
    loaded.obdRetryIntervalMs = clampConnectionCycleObdRetryIntervalMsValue(
        static_cast<int64_t>(reader.getUInt(kNvsCycleObdRetryInt, kConnectionCycleObdRetryIntervalMsDefault)));
    loaded.proxyOpenWindowMs = clampConnectionCycleProxyOpenWindowMsValue(
        static_cast<int64_t>(reader.getUInt(kNvsCycleProxyOpenWindow, kConnectionCycleProxyOpenWindowMsDefault)));
    loaded.v1SettleQuietMs = clampConnectionCycleV1SettleQuietMsValue(
        static_cast<int64_t>(reader.getUInt(kNvsCycleV1SettleQuiet, kConnectionCycleV1SettleQuietMsDefault)));
    loaded.v1SettleFallbackMs = clampConnectionCycleV1SettleFallbackMsValue(static_cast<int64_t>(
        reader.getUInt(kNvsCycleV1SettleFallback, kConnectionCycleV1SettleFallbackMsDefault)));
    loaded.cycleTeardownAckTimeoutMs = clampConnectionCycleTeardownAckTimeoutMsValue(static_cast<int64_t>(
        reader.getUInt(kNvsCycleTeardownAckTimeout, kConnectionCycleTeardownAckTimeoutMsDefault)));

    if (loaded.proxyBLE && loaded.obdEnabled) {
        // Legacy migration: older builds allowed both. OBD was the explicit
        // opt-in setting while proxy defaulted on, so preserve standalone OBD
        // behavior and disable passive proxy to avoid triple-BLE DMA pressure.
        Serial.println("[Settings] HEAL: proxyBLE and obdEnabled both true — keeping OBD, disabling proxy");
        loaded.proxyBLE = false;
    }

    loaded.alpEnabled = reader.getBool(kNvsAlpEnabled, false);
    {
        uint8_t alpPersist = reader.getUChar(kNvsAlpPersistSec, 0);
        if (alpPersist > 5)
            alpPersist = 5;
        loaded.alpAlertPersistSec = alpPersist;
    }
    loaded.alpDisableV1LaserOnPush = reader.getBool(kNvsAlpNoV1Laser, true);

    loaded.gpsEnabled = reader.getBool(kNvsGpsEnabled, false);
    {
        const uint32_t baud = reader.getUInt(kNvsGpsBaud, 9600);
        loaded.gpsBaud = (baud == 9600 || baud == 38400 || baud == 115200) ? baud : 9600;
    }
    const uint32_t backupDueRevision = reader.getUInt(kNvsBackupDueRevision, 0);
    if (!reader.good()) {
        Serial.println("[Settings] NVS field read unavailable; preserving settings and durable copies");
        nvsRecoveryPending_ = true;
        return;
    }
    // Publish only a complete read; getters and recovery must never observe a
    // mixture of saved values and defaults produced by an I/O failure.
    settings_ = std::move(loaded);
    restorePending_ = restorePending;
    restoreCommitWatermark_ = storedRestoreWatermark > 0 ? static_cast<uint64_t>(storedRestoreWatermark) : 0;
    profileDeleteCommitWatermark_ = storedDeleteWatermark > 0 ? static_cast<uint64_t>(storedDeleteWatermark) : 0;
    wifiCredentialCommitGeneration_ = wifiCredentialCommitGeneration;
    backupDueRevision_ = backupDueRevision;
    nvsLoadPending_ = false;
    nvsLoadRetryAtMs_ = 0;
    migrateLegacyWifiStaSlotNvs(activeNs, settings_.wifiStaSlots[0], wifiClientSsidKeyPresent);

    backupCompletedRevision_ = readBackupRevisionCompleted();
    backupRevisionCounter_ = settings_backup_revision::resumeCounter(backupDueRevision_, backupCompletedRevision_);
    if (settings_backup_revision::pending(backupDueRevision_, backupCompletedRevision_)) {
        requestDeferredBackupFromCurrentState();
    }

    Serial.printf("[Settings] OK proxy=%s bright=%d autoPush=%s\n", settings_.proxyBLE ? "on" : "off",
                  settings_.brightness, settings_.autoPushEnabled ? "on" : "off");
}

bool SettingsManager::ensureSettingsLoaded() {
    if (nvsLoadPending_) load();
    if (nvsLoadPending_) return false;
    if ((nvsRecoveryPending_ || profileReferenceRecoveryPending_) && !nvsRecoveryInProgress_) {
        // A setter can be the first caller after NVS recovers. Finish boot
        // reconciliation before it snapshots or changes settings, otherwise
        // its backup could discard a profile still recoverable only from SD.
        // Recovery itself uses the normal guarded settings mutation paths.
        const bool recoveredNvs = nvsRecoveryPending_;
        nvsRecoveryInProgress_ = true;
        const bool restoredSettings = checkAndRestoreFromSD();
        migrateAutoPushProfilesToV2();
        validateProfileReferences(*profiles_);
        nvsRecoveryInProgress_ = false;
        if (profileReferenceRecoveryPending_) return false;
        nvsRecoveryPending_ = false;
        // load() cannot publish a backup while the recovery source is still
        // provisional. Resume its durable intent now, unless reconciliation
        // already completed that revision synchronously.
        backupCompletedRevision_ = readBackupRevisionCompleted();
        if (settings_backup_revision::pending(backupDueRevision_, backupCompletedRevision_)) {
            requestDeferredBackupFromCurrentState();
        }
        // A profile-only repair does not replace the running configuration.
        // Publishing it as an NVS recovery would unnecessarily restart sensors.
        if (recoveredNvs || restoredSettings) ++recoveryRevision_;
    }
    return true;
}

bool SettingsManager::save() {
    if (!persistSettingsWithBackupIntent()) {
        return false;
    }

    clearDeferredPersistState();
    clearLastV1AddressFallback();
    Serial.println("Settings saved atomically");

    if (!backupToSD()) {
        requestDeferredBackupFromCurrentState();
    }
    return true;
}

void SettingsManager::requestDeferredPersist() {
    if (nvsLoadPending_ || ((nvsRecoveryPending_ || profileReferenceRecoveryPending_) && !nvsRecoveryInProgress_)) return;
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
    if (recoveryPending()) {
        if (nvsLoadRetryAtMs_ != 0 && !isDeferredPersistDue(nowMs, nvsLoadRetryAtMs_)) return;
        if (!ensureSettingsLoaded()) {
            nvsLoadRetryAtMs_ = nowMs + SETTINGS_DEFERRED_PERSIST_RETRY_BACKOFF_MS;
            return;
        }
        nvsLoadRetryAtMs_ = 0;
    }
    serviceLastV1AddressFallbackPersist(nowMs);

    if (!deferredPersistPending_) {
        return;
    }

    if (deferredPersistNextAttemptAtMs_ != 0 && !isDeferredPersistDue(nowMs, deferredPersistNextAttemptAtMs_)) {
        return;
    }

    if (!persistSettingsWithBackupIntent()) {
        deferredPersistPending_ = true;
        deferredPersistRetryScheduled_ = true;
        deferredPersistNextAttemptAtMs_ = nowMs + SETTINGS_DEFERRED_PERSIST_RETRY_BACKOFF_MS;
        return;
    }

    clearDeferredPersistState();
    clearLastV1AddressFallback();
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

// Load six physical-segment colours while preserving v11 eight-segment themes.
// Detection remains per-key so an interrupted migration cannot reset a theme.
static void loadSignalBarColors(SettingsNvsReader& reader, V1Settings& loaded) {
    static constexpr const char* kDirectKeys[6] = {
        kNvsLegacyColorBar1, kNvsLegacyColorBar2, kNvsLegacyColorBar3,
        kNvsLegacyColorBar4, kNvsLegacyColorBar5, kNvsLegacyColorBar6,
    };
    static constexpr uint16_t kDirectDefaults[6] = {
        0x07E0, 0x07E0, 0xFFE0, 0xFFE0, 0xF800, 0xF800,
    };
    static constexpr const char* kSegmentKeys[8] = {
        kNvsColorBarSeg1, kNvsColorBarSeg2, kNvsColorBarSeg3, kNvsColorBarSeg4,
        kNvsColorBarSeg5, kNvsColorBarSeg6, kNvsColorBarSeg7, kNvsColorBarSeg8,
    };
    static constexpr uint16_t kSegmentDefaults[8] = {
        0x07E0, 0x07E0, 0x6FE0, 0xFFE0, 0xFFE0, 0xFB80, 0xF800, 0xF800,
    };

    bool haveDirect = false;
    for (int i = 0; i < 6; ++i) {
        if (reader.isKey(kDirectKeys[i])) {
            haveDirect = true;
            break;
        }
    }
    bool haveSegments = false;
    for (int i = 0; i < 8; ++i) {
        if (reader.isKey(kSegmentKeys[i])) {
            haveSegments = true;
            break;
        }
    }

    const int storedVersion = reader.getInt(kNvsSettingsVer, 1);
    if (storedVersion >= 12 && haveDirect) {
        for (int i = 0; i < 6; ++i) {
            loaded.colorBars[i] =
                sanitizeRgb565Color(reader.getUShort(kDirectKeys[i], kDirectDefaults[i]), kDirectDefaults[i]);
        }
        return;
    }

    if (haveSegments) {
        uint16_t segments[8];
        for (int i = 0; i < 8; ++i) {
            segments[i] =
                sanitizeRgb565Color(reader.getUShort(kSegmentKeys[i], kSegmentDefaults[i]), kSegmentDefaults[i]);
        }
        DisplayVisualContract::collapseEightBarColorsToSix(segments, loaded.colorBars);
        return;
    }

    for (int i = 0; i < 6; ++i) {
        loaded.colorBars[i] =
            haveDirect
                ? sanitizeRgb565Color(reader.getUShort(kDirectKeys[i], kDirectDefaults[i]), kDirectDefaults[i])
                : kDirectDefaults[i];
    }
}
