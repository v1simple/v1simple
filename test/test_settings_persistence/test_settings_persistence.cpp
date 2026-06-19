#include <unity.h>

#include <filesystem>
#include <string>

#include <ArduinoJson.h>

#include "../mocks/Arduino.h"
#include "../mocks/Preferences.h"
#include "../mocks/nvs.h"
#include "../mocks/storage_manager.h"
#include "../../src/settings.h"
#include "../../src/v1_profiles.h"

#ifndef ARDUINO
SerialClass Serial;
unsigned long mockMillis = 0;
unsigned long mockMicros = 0;
#endif

namespace ArduinoJson {

inline void convertFromJson(JsonVariantConst src, ::String& dst) {
    const char* raw = src.as<const char*>();
    dst = ::String(raw ? raw : "");
}

inline bool canConvertFromJson(JsonVariantConst src, const ::String&) {
    return src.is<const char*>();
}

}  // namespace ArduinoJson

#include "../../src/v1_profiles.cpp"
#include "../../src/backup_payload_builder.cpp"
#include "../../src/psram_freertos_alloc.cpp"
#include "../../src/settings.cpp"
#include "../../src/settings_setters.cpp"
#include "../../src/settings_nvs.cpp"
#include "../../src/settings_backup.cpp"
#include "../../src/settings_restore.cpp"

namespace {

std::filesystem::path g_tempRoot;
int g_tempRootIndex = 0;

std::filesystem::path nextTempRoot() {
    return std::filesystem::temp_directory_path() /
           ("settings_persistence_" + std::to_string(++g_tempRootIndex));
}

void resetRuntimeState() {
    mock_preferences::reset();
    mock_nvs::reset();
    storageManager.reset();
    StorageManager::resetMockSdLockState();
    v1ProfileManager = V1ProfileManager();
    settingsManager = SettingsManager();
    resetDeferredSettingsBackupStateForTest();
    mockMillis = 1000;
    mockMicros = 1000000;
}

String activeNamespaceOrEmpty() {
    return mock_preferences::getString(SETTINGS_NS_META, "active", "");
}

bool loadJsonFile(fs::FS& fs, const char* path, JsonDocument& doc) {
    File file = fs.open(path, FILE_READ);
    if (!file) {
        return false;
    }
    const DeserializationError err = deserializeJson(doc, file);
    file.close();
    return !err;
}

std::string readFileToString(fs::FS& fs, const char* path) {
    File file = fs.open(path, FILE_READ);
    if (!file) {
        return {};
    }

    std::string output;
    while (file.available()) {
        output.push_back(static_cast<char>(file.read()));
    }
    file.close();
    return output;
}

}  // namespace

void setUp() {
    g_tempRoot = nextTempRoot();
    std::filesystem::remove_all(g_tempRoot);
    std::filesystem::create_directories(g_tempRoot);
    resetRuntimeState();
}

void tearDown() {
    std::filesystem::remove_all(g_tempRoot);
}

void test_absent_auto_push_key_uses_authoritative_default() {
    Preferences active;
    TEST_ASSERT_TRUE(active.begin(SETTINGS_NS_A, false));
    TEST_ASSERT_TRUE(active.clear());
    TEST_ASSERT_GREATER_THAN(0, active.putInt(kNvsValid, SETTINGS_VERSION));
    TEST_ASSERT_GREATER_THAN(0, active.putInt(kNvsSettingsVer, SETTINGS_VERSION));
    TEST_ASSERT_FALSE(active.isKey(kNvsAutoPush));
    active.end();

    Preferences meta;
    TEST_ASSERT_TRUE(meta.begin(SETTINGS_NS_META, false));
    TEST_ASSERT_GREATER_THAN(0, meta.putString(kNvsMetaActive, SETTINGS_NS_A));
    meta.end();

    SettingsManager manager;
    manager.load();

    TEST_ASSERT_EQUAL(kDefaultAutoPushEnabled, manager.get().autoPushEnabled);

    const SettingsManager::NvsDiagnostic diag = manager.getNvsDiagnostic();
    TEST_ASSERT_EQUAL_STRING(SETTINGS_NS_A, diag.activeNamespace.c_str());
    TEST_ASSERT_EQUAL(kDefaultAutoPushEnabled, diag.nvsAutoPush);
    TEST_ASSERT_TRUE(diag.healthy);
}

void test_save_load_and_backup_round_trip_current_shape_fields() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager original;
    V1Settings& settings = original.mutableSettings();
    settings.enableWifi = false;
    settings.wifiMode = V1_WIFI_APSTA;
    settings.apSSID = "RoadRig";
    settings.apPassword = "unit-test-pass";
    settings.wifiClientEnabled = true;
    settings.wifiClientSSID = "GarageNet";
    settings.proxyBLE = false;
    settings.proxyName = "Proxy-Rig";
    settings.turnOffDisplay = true;
    settings.brightness = 123;
    settings.colorBogey = 0x1234;
    settings.colorObd = 0x6789;
    settings.hideWifiIcon = true;
    settings.alertVolumeFadeEnabled = true;
    settings.alertVolumeFadeDelaySec = 4;
    settings.alertVolumeFadeVolume = 2;
    settings.autoPushEnabled = true;
    settings.activeSlot = 2;
    settings.slot0Name = "DEFAULT";
    settings.slot1Name = "HIGHWAY";
    settings.slot2Name = "QUIET";
    settings.slot0Color = 0x1111;
    settings.slot1Color = 0x2222;
    settings.slot2Color = 0x3333;
    settings.slot0Volume = 4;
    settings.slot1MuteVolume = 2;
    settings.slot2DarkMode = true;
    settings.slot0MuteToZero = true;
    settings.slot1AlertPersist = 5;
    settings.slot2PriorityArrow = true;
    settings.slot0_default = AutoPushSlot("City", V1_MODE_LOGIC);
    settings.slot1_highway = AutoPushSlot("Highway", V1_MODE_ALL_BOGEYS);
    settings.slot2_comfort = AutoPushSlot("Quiet", V1_MODE_ADVANCED_LOGIC);
    settings.lastV1Address = "AA:BB:CC:DD:EE:FF";
    settings.autoPowerOffMinutes = 7;
    settings.apTimeoutMinutes = 15;
    settings.obdEnabled = true;
    settings.obdSavedAddress = "11:22:33:44:55:66";
    settings.obdSavedName = "Truck Adapter";
    settings.obdMinRssi = -65;
    settings.obdScanWindowMs = 18000;
    settings.obdRetryIntervalMs = 90000;
    settings.proxyOpenWindowMs = 45000;
    settings.wifiOpenTimeoutMs = 42000;
    settings.v1SettleQuietMs = 700;
    settings.v1SettleFallbackMs = 2200;
    settings.cycleTeardownAckTimeoutMs = 150;
    settings.alpEnabled = true;
    settings.alpSdLogEnabled = true;
    settings.alpAlertPersistSec = 4;
    settings.alpDisableV1LaserOnPush = false;

    original.save();

    TEST_ASSERT_EQUAL_UINT32(2u, original.backupRevision());

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(activeNs.c_str(), "apSSID"));
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(activeNs.c_str(), "volFadeEn"));
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(activeNs.c_str(), "obdMinRssi"));
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(activeNs.c_str(), "obdName"));
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(activeNs.c_str(), "alpNoV1Laser"));
    TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(activeNs.c_str(), "obdVin11"));
    TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(activeNs.c_str(), "obdEotPid"));
    TEST_ASSERT_TRUE(
        mock_preferences::getString(activeNs.c_str(), "apPassword", "").startsWith(OBFUSCATION_HEX_PREFIX));

    SettingsManager reloaded;
    reloaded.load();
    const V1Settings& loaded = reloaded.get();

    TEST_ASSERT_FALSE(loaded.enableWifi);
    TEST_ASSERT_EQUAL_INT(V1_WIFI_APSTA, loaded.wifiMode);
    TEST_ASSERT_EQUAL_STRING("RoadRig", loaded.apSSID.c_str());
    TEST_ASSERT_EQUAL_STRING("unit-test-pass", loaded.apPassword.c_str());
    TEST_ASSERT_TRUE(loaded.wifiClientEnabled);
    TEST_ASSERT_EQUAL_STRING("GarageNet", loaded.wifiClientSSID.c_str());
    TEST_ASSERT_EQUAL_STRING("GarageNet", loaded.wifiStaSlots[0].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Saved", loaded.wifiStaSlots[0].label.c_str());
    TEST_ASSERT_EQUAL_UINT8(0, loaded.wifiStaSlots[0].priority);
    TEST_ASSERT_FALSE(loaded.proxyBLE);
    TEST_ASSERT_EQUAL_STRING("Proxy-Rig", loaded.proxyName.c_str());
    TEST_ASSERT_TRUE(loaded.turnOffDisplay);
    TEST_ASSERT_EQUAL_UINT8(123, loaded.brightness);
    TEST_ASSERT_EQUAL_HEX16(0x1234, loaded.colorBogey);
    TEST_ASSERT_EQUAL_HEX16(0x6789, loaded.colorObd);
    TEST_ASSERT_TRUE(loaded.hideWifiIcon);
    TEST_ASSERT_TRUE(loaded.alertVolumeFadeEnabled);
    TEST_ASSERT_EQUAL_UINT8(4, loaded.alertVolumeFadeDelaySec);
    TEST_ASSERT_EQUAL_UINT8(2, loaded.alertVolumeFadeVolume);
    TEST_ASSERT_TRUE(loaded.autoPushEnabled);
    TEST_ASSERT_EQUAL_INT(2, loaded.activeSlot);
    TEST_ASSERT_EQUAL_STRING("QUIET", loaded.slot2Name.c_str());
    TEST_ASSERT_EQUAL_HEX16(0x1111, loaded.slot0Color);
    TEST_ASSERT_EQUAL_HEX16(0x2222, loaded.slot1Color);
    TEST_ASSERT_EQUAL_HEX16(0x3333, loaded.slot2Color);
    TEST_ASSERT_EQUAL_UINT8(4, loaded.slot0Volume);
    TEST_ASSERT_EQUAL_UINT8(2, loaded.slot1MuteVolume);
    TEST_ASSERT_TRUE(loaded.slot2DarkMode);
    TEST_ASSERT_TRUE(loaded.slot0MuteToZero);
    TEST_ASSERT_EQUAL_UINT8(5, loaded.slot1AlertPersist);
    TEST_ASSERT_TRUE(loaded.slot2PriorityArrow);
    TEST_ASSERT_EQUAL_STRING("City", loaded.slot0_default.profileName.c_str());
    TEST_ASSERT_EQUAL_INT(V1_MODE_LOGIC, loaded.slot0_default.mode);
    TEST_ASSERT_EQUAL_STRING("Quiet", loaded.slot2_comfort.profileName.c_str());
    TEST_ASSERT_EQUAL_INT(V1_MODE_ADVANCED_LOGIC, loaded.slot2_comfort.mode);
    TEST_ASSERT_EQUAL_STRING("AA:BB:CC:DD:EE:FF", loaded.lastV1Address.c_str());
    TEST_ASSERT_EQUAL_UINT8(7, loaded.autoPowerOffMinutes);
    TEST_ASSERT_EQUAL_UINT8(15, loaded.apTimeoutMinutes);
    TEST_ASSERT_TRUE(loaded.obdEnabled);
    TEST_ASSERT_EQUAL_STRING("11:22:33:44:55:66", loaded.obdSavedAddress.c_str());
    TEST_ASSERT_EQUAL_STRING("Truck Adapter", loaded.obdSavedName.c_str());
    TEST_ASSERT_EQUAL_INT8(-65, loaded.obdMinRssi);
    TEST_ASSERT_EQUAL_UINT32(18000u, loaded.obdScanWindowMs);
    TEST_ASSERT_EQUAL_UINT32(90000u, loaded.obdRetryIntervalMs);
    TEST_ASSERT_EQUAL_UINT32(45000u, loaded.proxyOpenWindowMs);
    TEST_ASSERT_EQUAL_UINT32(42000u, loaded.wifiOpenTimeoutMs);
    TEST_ASSERT_EQUAL_UINT32(700u, loaded.v1SettleQuietMs);
    TEST_ASSERT_EQUAL_UINT32(2200u, loaded.v1SettleFallbackMs);
    TEST_ASSERT_EQUAL_UINT32(150u, loaded.cycleTeardownAckTimeoutMs);
    TEST_ASSERT_TRUE(loaded.alpEnabled);
    TEST_ASSERT_TRUE(loaded.alpSdLogEnabled);
    TEST_ASSERT_EQUAL_UINT8(4, loaded.alpAlertPersistSec);
    TEST_ASSERT_FALSE(loaded.alpDisableV1LaserOnPush);

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_EQUAL_STRING("v1simple_sd_backup", backupDoc["_type"].as<const char*>());
    TEST_ASSERT_EQUAL_UINT32(1000u, backupDoc["_timestamp"].as<uint32_t>());
    TEST_ASSERT_FALSE(backupDoc["enableWifi"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("RoadRig", backupDoc["apSSID"].as<const char*>());
    TEST_ASSERT_TRUE(backupDoc["wifiClientEnabled"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("GarageNet", backupDoc["wifiClientSSID"].as<const char*>());
    TEST_ASSERT_TRUE(backupDoc["wifiStaSlots"].is<JsonArrayConst>());
    JsonArrayConst backupStaSlots = backupDoc["wifiStaSlots"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL_UINT(1u, backupStaSlots.size());
    TEST_ASSERT_EQUAL_INT(0, backupStaSlots[0]["index"].as<int>());
    TEST_ASSERT_EQUAL_STRING("GarageNet", backupStaSlots[0]["ssid"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("Saved", backupStaSlots[0]["label"].as<const char*>());
    TEST_ASSERT_TRUE(backupDoc["cameraAlertsEnabled"].isNull());
    TEST_ASSERT_TRUE(backupDoc["cameraAlertRangeCm"].isNull());
    TEST_ASSERT_EQUAL_INT(123, backupDoc["brightness"].as<int>());
    TEST_ASSERT_EQUAL_INT(0x6789, backupDoc["colorObd"].as<int>());
    TEST_ASSERT_TRUE(backupDoc["autoPushEnabled"].as<bool>());
    TEST_ASSERT_EQUAL_STRING("Quiet", backupDoc["slot2ProfileName"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("Truck Adapter", backupDoc["obdSavedName"].as<const char*>());
    TEST_ASSERT_EQUAL_INT(-65, backupDoc["obdMinRssi"].as<int>());
    TEST_ASSERT_EQUAL_INT(18000, backupDoc["obdScanWindowMs"].as<int>());
    TEST_ASSERT_EQUAL_INT(90000, backupDoc["obdRetryIntervalMs"].as<int>());
    TEST_ASSERT_EQUAL_INT(45000, backupDoc["proxyOpenWindowMs"].as<int>());
    TEST_ASSERT_EQUAL_INT(42000, backupDoc["wifiOpenTimeoutMs"].as<int>());
    TEST_ASSERT_EQUAL_INT(700, backupDoc["v1SettleQuietMs"].as<int>());
    TEST_ASSERT_EQUAL_INT(2200, backupDoc["v1SettleFallbackMs"].as<int>());
    TEST_ASSERT_EQUAL_INT(150, backupDoc["cycleTeardownAckTimeoutMs"].as<int>());
    TEST_ASSERT_TRUE(backupDoc["alpEnabled"].as<bool>());
    TEST_ASSERT_TRUE(backupDoc["alpSdLogEnabled"].as<bool>());
    TEST_ASSERT_EQUAL_INT(4, backupDoc["alpAlertPersistSec"].as<int>());
    TEST_ASSERT_FALSE(backupDoc["alpDisableV1LaserOnPush"].as<bool>());
    TEST_ASSERT_TRUE(backupDoc["obdCachedVinPrefix11"].isNull());
    TEST_ASSERT_TRUE(backupDoc["obdCachedEotProfileId"].isNull());
    // apPassword must be present in the backup doc, obfuscated (not plain-text).
    TEST_ASSERT_TRUE(backupDoc["apPassword"].is<const char*>());
    TEST_ASSERT_TRUE(
        String(backupDoc["apPassword"].as<const char*>()).startsWith(OBFUSCATION_HEX_PREFIX));
}

void test_ap_password_restored_from_backup_when_key_present() {
    // Simulate a clean-flash: NVS is empty, so apPassword defaults to "setupv1simple".
    // SD backup contains a user-set password.  After applyBackupDocument the
    // custom password must be restored.
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    // Build a backup document containing an obfuscated apPassword.
    SettingsManager source;
    source.mutableSettings().apPassword = "RoadRig2026";
    JsonDocument backupDoc;
    BackupPayloadBuilder::buildBackupDocument(
        backupDoc,
        source.get(),
        v1ProfileManager,
        BackupPayloadBuilder::BackupTransport::SdBackup,
        1000);
    TEST_ASSERT_TRUE(backupDoc["apPassword"].is<const char*>());

    // Apply to a fresh manager with empty NVS — password should be default.
    SettingsManager target;
    TEST_ASSERT_EQUAL_STRING("setupv1simple", target.get().apPassword.c_str());

    const SettingsBackupApplyResult result = target.applyBackupDocument(backupDoc, true);
    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_EQUAL_STRING("RoadRig2026", target.get().apPassword.c_str());
}

void test_ap_password_preserved_when_backup_lacks_key() {
    // When the backup doc has NO apPassword key (e.g. an older backup written
    // before Gap B was fixed), the existing in-memory password must be kept.
    SettingsManager manager;
    manager.mutableSettings().apPassword = "ExistingPass123";

    JsonDocument doc;
    doc["_type"] = "v1simple_http_backup";
    doc["apSSID"] = "SomeSSID";
    // Intentionally no "apPassword" key

    const SettingsBackupApplyResult result = manager.applyBackupDocument(doc, true);
    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_EQUAL_STRING("ExistingPass123", manager.get().apPassword.c_str());
}

void test_wifi_client_password_is_omitted_by_default_and_optional_for_export() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.setWifiClientCredentials("GarageNet", "garage-secret-2026");

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    TEST_ASSERT_TRUE(backupDoc["wifiClientPasswordObf"].isNull());
    JsonArrayConst backupStaSlots = backupDoc["wifiStaSlots"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL_UINT(1u, backupStaSlots.size());
    TEST_ASSERT_TRUE(backupStaSlots[0]["passwordObf"].isNull());

    JsonDocument exportDoc;
    BackupPayloadBuilder::BuildOptions exportOptions;
    exportOptions.includeWifiPasswords = true;
    BackupPayloadBuilder::buildBackupDocument(
        exportDoc,
        manager.get(),
        v1ProfileManager,
        BackupPayloadBuilder::BackupTransport::HttpDownload,
        2000,
        exportOptions);

    JsonArrayConst exportStaSlots = exportDoc["wifiStaSlots"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL_UINT(1u, exportStaSlots.size());
    TEST_ASSERT_TRUE(exportStaSlots[0]["passwordObf"].is<const char*>());
    const String encoded = exportStaSlots[0]["passwordObf"].as<String>();
    TEST_ASSERT_TRUE(encoded.startsWith(OBFUSCATION_HEX_PREFIX));
    TEST_ASSERT_FALSE(encoded == "garage-secret-2026");
    TEST_ASSERT_EQUAL_STRING("garage-secret-2026",
                             decodeObfuscatedFromStorage(encoded).c_str());
    TEST_ASSERT_TRUE(exportDoc["wifiClientPasswordObf"].isNull());

    JsonDocument httpDoc;
    BackupPayloadBuilder::buildBackupDocument(
        httpDoc,
        manager.get(),
        v1ProfileManager,
        BackupPayloadBuilder::BackupTransport::HttpDownload,
        2000);
    TEST_ASSERT_TRUE(httpDoc["wifiClientPasswordObf"].isNull());
    TEST_ASSERT_TRUE(httpDoc["wifiStaSlots"][0]["passwordObf"].isNull());
}

void test_wifi_client_password_restores_from_sd_backup_to_nvs() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager source;
    source.setWifiClientCredentials("GarageNet", "garage-secret-2026");

    // Simulate clean flash while retaining the SD backup file.
    mock_preferences::reset();

    SettingsManager target;
    TEST_ASSERT_TRUE(target.restoreFromSD());
    TEST_ASSERT_TRUE(target.get().wifiClientEnabled);
    TEST_ASSERT_EQUAL_STRING("GarageNet", target.get().wifiClientSSID.c_str());
    TEST_ASSERT_EQUAL_STRING("garage-secret-2026", target.getWifiClientPassword().c_str());
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[0]));
    TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiPassword));
}

void test_wifi_sta_slot_sd_secret_retains_multiple_passwords() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager source;
    source.setWifiStaSlotCredentials(0, "ghost", "ghost-secret", "Garage", 0);
    source.setWifiStaSlotCredentials(1, "ghost-e", "extender-secret", "Extender", 1);

    JsonDocument secretDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, WIFI_CLIENT_SD_SECRET_PATH, secretDoc));
    TEST_ASSERT_EQUAL_INT(WIFI_CLIENT_SD_SECRET_VERSION, secretDoc["_version"].as<int>());
    TEST_ASSERT_TRUE(secretDoc["secrets"].is<JsonArrayConst>());
    JsonArrayConst secrets = secretDoc["secrets"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL_UINT(2u, secrets.size());

    bool foundGhost = false;
    bool foundExtender = false;
    for (JsonObjectConst entry : secrets) {
        const int index = entry["index"] | -1;
        const String ssid = entry["ssid"] | "";
        const String encoded = entry["password_obf"] | "";
        if (index == 0 && ssid == "ghost") {
            foundGhost = true;
            TEST_ASSERT_EQUAL_STRING("ghost-secret",
                                     decodeObfuscatedFromStorage(encoded).c_str());
        } else if (index == 1 && ssid == "ghost-e") {
            foundExtender = true;
            TEST_ASSERT_EQUAL_STRING("extender-secret",
                                     decodeObfuscatedFromStorage(encoded).c_str());
        }
    }
    TEST_ASSERT_TRUE(foundGhost);
    TEST_ASSERT_TRUE(foundExtender);

    // Simulate clean flash while retaining SD files.  The main settings backup
    // restores slot metadata, and the separate secret mirror must recover both
    // per-slot passwords.
    mock_preferences::reset();

    SettingsManager restored;
    TEST_ASSERT_TRUE(restored.restoreFromSD());
    TEST_ASSERT_TRUE(restored.get().wifiClientEnabled);
    TEST_ASSERT_EQUAL_STRING("ghost", restored.get().wifiStaSlots[0].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("ghost-e", restored.get().wifiStaSlots[1].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("ghost-secret", restored.getWifiStaSlotPassword(0).c_str());
    TEST_ASSERT_EQUAL_STRING("extender-secret", restored.getWifiStaSlotPassword(1).c_str());
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[0]));
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[1]));
}

void test_wifi_sta_slot_sd_secret_upgrade_preserves_legacy_top_level_secret() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    JsonDocument legacySecret;
    legacySecret["_type"] = WIFI_CLIENT_SD_SECRET_TYPE;
    legacySecret["_version"] = 1;
    legacySecret["ssid"] = "ghost-e";
    legacySecret["password_obf"] = encodeObfuscatedForStorage("extender-secret");
    legacySecret["timestamp"] = 42;
    File legacyFile = fs.open(WIFI_CLIENT_SD_SECRET_PATH, FILE_WRITE);
    TEST_ASSERT_TRUE(static_cast<bool>(legacyFile));
    serializeJson(legacySecret, legacyFile);
    legacyFile.close();

    SettingsManager manager;
    manager.setWifiStaSlotCredentials(0, "ghost", "ghost-secret", "Garage", 0);

    JsonDocument secretDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, WIFI_CLIENT_SD_SECRET_PATH, secretDoc));
    TEST_ASSERT_TRUE(secretDoc["secrets"].is<JsonArrayConst>());
    JsonArrayConst secrets = secretDoc["secrets"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL_UINT(2u, secrets.size());

    bool foundSaved = false;
    bool foundLegacy = false;
    for (JsonObjectConst entry : secrets) {
        const String ssid = entry["ssid"] | "";
        const String encoded = entry["password_obf"] | "";
        if (ssid == "ghost") {
            foundSaved = true;
            TEST_ASSERT_EQUAL_STRING("ghost-secret",
                                     decodeObfuscatedFromStorage(encoded).c_str());
        } else if (ssid == "ghost-e") {
            foundLegacy = true;
            TEST_ASSERT_EQUAL_STRING("extender-secret",
                                     decodeObfuscatedFromStorage(encoded).c_str());
        }
    }

    TEST_ASSERT_TRUE(foundSaved);
    TEST_ASSERT_TRUE(foundLegacy);
}

void test_wifi_client_password_export_restores_to_slot_nvs_when_secret_file_missing() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager source;
    source.setWifiClientCredentials("GarageNet", "garage-secret-2026");

    JsonDocument exportDoc;
    BackupPayloadBuilder::BuildOptions exportOptions;
    exportOptions.includeWifiPasswords = true;
    BackupPayloadBuilder::buildBackupDocument(
        exportDoc,
        source.get(),
        v1ProfileManager,
        BackupPayloadBuilder::BackupTransport::HttpDownload,
        2000,
        exportOptions);

    SettingsManager target;
    mock_preferences::reset();
    fs.remove(WIFI_CLIENT_SD_SECRET_PATH);
    const SettingsBackupApplyResult result = target.applyBackupDocument(exportDoc, true);

    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_TRUE(target.get().wifiClientEnabled);
    TEST_ASSERT_EQUAL_STRING("GarageNet", target.get().wifiStaSlots[0].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("garage-secret-2026", target.getWifiClientPassword().c_str());
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[0]));
}

void test_wifi_sta_slots_restore_without_passwords_clears_stale_secrets() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.setWifiStaSlotCredentials(0, "OldGarage", "old-secret", "Old", 0);
    manager.setWifiStaSlotCredentials(1, "OldPhone", "old-phone-secret", "Phone", 1);
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[0]));
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[1]));
    TEST_ASSERT_TRUE(fs.exists(WIFI_CLIENT_SD_SECRET_PATH));

    JsonDocument doc;
    doc["_type"] = "v1simple_http_backup";
    doc["wifiClientEnabled"] = true;
    JsonArray slots = doc["wifiStaSlots"].to<JsonArray>();
    JsonObject slot0 = slots.add<JsonObject>();
    slot0["index"] = 0;
    slot0["ssid"] = "GarageNet";
    slot0["label"] = "Garage";
    slot0["priority"] = 1;
    slot0["lastConnectedAtSec"] = 77;
    JsonObject slot1 = slots.add<JsonObject>();
    slot1["index"] = 1;
    slot1["ssid"] = "CarHotspot";
    slot1["label"] = "Car";
    slot1["priority"] = 0;

    const SettingsBackupApplyResult result = manager.applyBackupDocument(doc, true);

    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_TRUE(manager.get().wifiClientEnabled);
    TEST_ASSERT_EQUAL_STRING("GarageNet", manager.get().wifiStaSlots[0].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("CarHotspot", manager.get().wifiStaSlots[1].ssid.c_str());
    TEST_ASSERT_EQUAL_UINT32(77u, manager.get().wifiStaSlots[0].lastConnectedAtSec);
    TEST_ASSERT_EQUAL_STRING("", manager.getWifiStaSlotPassword(0).c_str());
    TEST_ASSERT_EQUAL_STRING("", manager.getWifiStaSlotPassword(1).c_str());
    TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[0]));
    TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[1]));
    TEST_ASSERT_FALSE(fs.exists(WIFI_CLIENT_SD_SECRET_PATH));
}

void test_legacy_station_backup_restores_to_slot0() {
    SettingsManager manager;

    JsonDocument doc;
    doc["_type"] = "v1simple_backup";
    doc["wifiClientEnabled"] = true;
    doc["stationSSID"] = "LegacyStation";
    doc["stationPassword"] = "legacy-station-secret";

    const SettingsBackupApplyResult result = manager.applyBackupDocument(doc, true);

    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_TRUE(manager.get().wifiClientEnabled);
    TEST_ASSERT_EQUAL_STRING("LegacyStation", manager.get().wifiStaSlots[0].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Saved", manager.get().wifiStaSlots[0].label.c_str());
    TEST_ASSERT_EQUAL_STRING("LegacyStation", manager.get().wifiClientSSID.c_str());
    TEST_ASSERT_EQUAL_STRING("legacy-station-secret", manager.getWifiClientPassword().c_str());
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[0]));
}

void test_wifi_sta_slots_round_trip_and_primary_alias() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager original;
    V1Settings& settings = original.mutableSettings();
    settings.wifiClientEnabled = true;
    settings.wifiStaSlots[0].ssid = "GarageNet";
    settings.wifiStaSlots[0].label = "Garage";
    settings.wifiStaSlots[0].priority = 1;
    settings.wifiStaSlots[0].lastConnectedAtSec = 100;
    settings.wifiStaSlots[1].ssid = "CarHotspot";
    settings.wifiStaSlots[1].label = "Car";
    settings.wifiStaSlots[1].priority = 0;
    settings.wifiStaSlots[1].lastConnectedAtSec = 50;
    settings.wifiStaSlots[2].ssid = "Phone";
    settings.wifiStaSlots[2].label = "Phone";
    settings.wifiStaSlots[2].priority = 0;
    settings.wifiStaSlots[2].lastConnectedAtSec = 250;
    settings.refreshWifiClientAliasFromSlots();

    TEST_ASSERT_EQUAL_STRING("Phone", settings.wifiClientSSID.c_str());

    original.save();

    SettingsManager reloaded;
    reloaded.load();
    const V1Settings& loaded = reloaded.get();
    TEST_ASSERT_TRUE(loaded.wifiClientEnabled);
    TEST_ASSERT_EQUAL_STRING("Phone", loaded.wifiClientSSID.c_str());
    TEST_ASSERT_EQUAL_STRING("GarageNet", loaded.wifiStaSlots[0].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Garage", loaded.wifiStaSlots[0].label.c_str());
    TEST_ASSERT_EQUAL_UINT8(1, loaded.wifiStaSlots[0].priority);
    TEST_ASSERT_EQUAL_UINT32(100u, loaded.wifiStaSlots[0].lastConnectedAtSec);
    TEST_ASSERT_EQUAL_STRING("CarHotspot", loaded.wifiStaSlots[1].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Phone", loaded.wifiStaSlots[2].ssid.c_str());

    JsonDocument backupDoc;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, backupDoc));
    JsonArrayConst backupStaSlots = backupDoc["wifiStaSlots"].as<JsonArrayConst>();
    TEST_ASSERT_EQUAL_UINT(3u, backupStaSlots.size());
    TEST_ASSERT_EQUAL_STRING("GarageNet", backupStaSlots[0]["ssid"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("CarHotspot", backupStaSlots[1]["ssid"].as<const char*>());
    TEST_ASSERT_EQUAL_STRING("Phone", backupStaSlots[2]["ssid"].as<const char*>());
}

void test_wifi_client_disabled_flag_survives_reboot_with_saved_slots() {
    SettingsManager original;
    V1Settings& settings = original.mutableSettings();
    settings.wifiClientEnabled = false;
    settings.wifiStaSlots[0].ssid = "GarageNet";
    settings.wifiStaSlots[0].label = "Garage";
    settings.wifiStaSlots[0].priority = 0;
    settings.refreshWifiClientAliasFromSlots();

    TEST_ASSERT_FALSE(settings.wifiClientEnabled);
    TEST_ASSERT_EQUAL_INT(V1_WIFI_AP, settings.wifiMode);
    TEST_ASSERT_EQUAL_STRING("GarageNet", settings.wifiClientSSID.c_str());

    original.save();

    SettingsManager reloaded;
    reloaded.load();

    TEST_ASSERT_FALSE(reloaded.get().wifiClientEnabled);
    TEST_ASSERT_EQUAL_INT(V1_WIFI_AP, reloaded.get().wifiMode);
    TEST_ASSERT_EQUAL_STRING("GarageNet", reloaded.get().wifiClientSSID.c_str());
    TEST_ASSERT_EQUAL_STRING("GarageNet", reloaded.get().wifiStaSlots[0].ssid.c_str());
}

void test_legacy_wifi_client_enabled_missing_still_heals_saved_slots() {
    SettingsManager original;
    V1Settings& settings = original.mutableSettings();
    settings.wifiClientEnabled = false;
    settings.wifiStaSlots[0].ssid = "LegacyGarage";
    settings.wifiStaSlots[0].label = "Garage";
    settings.wifiStaSlots[0].priority = 0;
    settings.refreshWifiClientAliasFromSlots();
    original.save();

    const String activeNs = activeNamespaceOrEmpty();
    Preferences prefs;
    TEST_ASSERT_TRUE(prefs.begin(activeNs.c_str(), false));
    TEST_ASSERT_TRUE(prefs.remove(kNvsWifiClientEnabled));
    prefs.end();

    SettingsManager reloaded;
    reloaded.load();

    TEST_ASSERT_TRUE(reloaded.get().wifiClientEnabled);
    TEST_ASSERT_EQUAL_INT(V1_WIFI_APSTA, reloaded.get().wifiMode);
    TEST_ASSERT_EQUAL_STRING("LegacyGarage", reloaded.get().wifiClientSSID.c_str());
}

void test_backup_restore_preserves_explicit_wifi_client_disabled_with_saved_slots() {
    SettingsManager manager;

    JsonDocument doc;
    doc["_type"] = "v1simple_http_backup";
    doc["wifiClientEnabled"] = false;
    JsonArray slots = doc["wifiStaSlots"].to<JsonArray>();
    JsonObject slot0 = slots.add<JsonObject>();
    slot0["index"] = 0;
    slot0["ssid"] = "GarageNet";
    slot0["label"] = "Garage";
    slot0["priority"] = 0;

    const SettingsBackupApplyResult result = manager.applyBackupDocument(doc, true);

    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_FALSE(manager.get().wifiClientEnabled);
    TEST_ASSERT_EQUAL_INT(V1_WIFI_AP, manager.get().wifiMode);
    TEST_ASSERT_EQUAL_STRING("GarageNet", manager.get().wifiClientSSID.c_str());
    TEST_ASSERT_EQUAL_STRING("GarageNet", manager.get().wifiStaSlots[0].ssid.c_str());
}

void test_backup_restore_heals_missing_wifi_client_enabled_with_saved_slots() {
    SettingsManager manager;

    JsonDocument doc;
    doc["_type"] = "v1simple_http_backup";
    JsonArray slots = doc["wifiStaSlots"].to<JsonArray>();
    JsonObject slot0 = slots.add<JsonObject>();
    slot0["index"] = 0;
    slot0["ssid"] = "LegacyGarage";
    slot0["label"] = "Garage";
    slot0["priority"] = 0;

    const SettingsBackupApplyResult result = manager.applyBackupDocument(doc, true);

    TEST_ASSERT_TRUE(result.success);
    TEST_ASSERT_TRUE(manager.get().wifiClientEnabled);
    TEST_ASSERT_EQUAL_INT(V1_WIFI_APSTA, manager.get().wifiMode);
    TEST_ASSERT_EQUAL_STRING("LegacyGarage", manager.get().wifiClientSSID.c_str());
}

void test_legacy_wifi_client_credentials_migrate_to_slot0() {
    SettingsManager seed;
    V1Settings& settings = seed.mutableSettings();
    settings.wifiClientEnabled = true;
    settings.wifiClientSSID = "LegacyNet";
    seed.save();

    Preferences legacyPrefs;
    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(legacyPrefs.begin(activeNs.c_str(), false));
    legacyPrefs.remove(kNvsWifiStaSlotSsid[0]);
    legacyPrefs.remove(kNvsWifiStaSlotLabel[0]);
    legacyPrefs.remove(kNvsWifiStaSlotPriority[0]);
    legacyPrefs.remove(kNvsWifiStaSlotLastConnected[0]);
    legacyPrefs.putString(kNvsWifiClientSsid, "LegacyNet");
    legacyPrefs.end();

    Preferences wifiPrefs;
    TEST_ASSERT_TRUE(wifiPrefs.begin(WIFI_CLIENT_NS, false));
    wifiPrefs.putString(kNvsWifiPassword, encodeObfuscatedForStorage("legacy-password"));
    wifiPrefs.remove(kNvsWifiStaSlotPassword[0]);
    wifiPrefs.end();

    SettingsManager migrated;
    migrated.load();

    TEST_ASSERT_TRUE(migrated.get().wifiClientEnabled);
    TEST_ASSERT_EQUAL_STRING("LegacyNet", migrated.get().wifiStaSlots[0].ssid.c_str());
    TEST_ASSERT_EQUAL_STRING("Saved", migrated.get().wifiStaSlots[0].label.c_str());
    TEST_ASSERT_EQUAL_STRING("LegacyNet", migrated.get().wifiClientSSID.c_str());
    TEST_ASSERT_EQUAL_STRING("legacy-password", migrated.getWifiClientPassword().c_str());
    TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(activeNs.c_str(), kNvsWifiClientSsid));
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(activeNs.c_str(), kNvsWifiStaSlotSsid[0]));
    TEST_ASSERT_FALSE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiPassword));
    TEST_ASSERT_TRUE(mock_preferences::namespaceHasKey(WIFI_CLIENT_NS, kNvsWifiStaSlotPassword[0]));
}

void test_apply_backup_document_unifies_restore_field_coverage_and_profile_restore() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    manager.mutableSettings().apPassword = "preserved-pass";

    JsonDocument doc;
    doc["_type"] = "v1simple_http_backup";
    doc["apSSID"] = "RestoredSSID";
    doc["brightness"] = 77;
    doc["colorObd"] = 0x2468;
    doc["obdEnabled"] = true;
    doc["obdSavedName"] = "Restored Adapter";
    doc["obdMinRssi"] = -61;
    doc["obdScanWindowMs"] = 20000;
    doc["obdRetryIntervalMs"] = 150000;
    doc["proxyOpenWindowMs"] = 50000;
    doc["wifiOpenTimeoutMs"] = 28000;
    doc["v1SettleQuietMs"] = 800;
    doc["v1SettleFallbackMs"] = 2500;
    doc["cycleTeardownAckTimeoutMs"] = 125;
    doc["slot0ProfileName"] = "Road";
    doc["slot0Mode"] = static_cast<int>(V1_MODE_LOGIC);

    JsonObject profile = doc["profiles"].to<JsonArray>().add<JsonObject>();
    profile["name"] = "Road";
    profile["description"] = "Restored profile";
    profile["displayOn"] = true;
    JsonArray bytes = profile["bytes"].to<JsonArray>();
    for (int i = 0; i < 6; ++i) {
        bytes.add(static_cast<uint8_t>(i + 1));
    }

    const SettingsBackupApplyResult applyResult = manager.applyBackupDocument(doc, true);

    TEST_ASSERT_TRUE(applyResult.success);
    TEST_ASSERT_EQUAL_INT(1, applyResult.profilesRestored);
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
    TEST_ASSERT_TRUE(manager.deferredBackupPending());

    const V1Settings& restored = manager.get();
    TEST_ASSERT_EQUAL_STRING("RestoredSSID", restored.apSSID.c_str());
    TEST_ASSERT_EQUAL_STRING("preserved-pass", restored.apPassword.c_str());
    TEST_ASSERT_EQUAL_UINT8(77, restored.brightness);
    TEST_ASSERT_EQUAL_HEX16(0x2468, restored.colorObd);
    TEST_ASSERT_TRUE(restored.obdEnabled);
    TEST_ASSERT_EQUAL_STRING("Restored Adapter", restored.obdSavedName.c_str());
    TEST_ASSERT_EQUAL_INT8(-61, restored.obdMinRssi);
    TEST_ASSERT_EQUAL_UINT32(20000u, restored.obdScanWindowMs);
    TEST_ASSERT_EQUAL_UINT32(150000u, restored.obdRetryIntervalMs);
    TEST_ASSERT_EQUAL_UINT32(50000u, restored.proxyOpenWindowMs);
    TEST_ASSERT_EQUAL_UINT32(28000u, restored.wifiOpenTimeoutMs);
    TEST_ASSERT_EQUAL_UINT32(800u, restored.v1SettleQuietMs);
    TEST_ASSERT_EQUAL_UINT32(2500u, restored.v1SettleFallbackMs);
    TEST_ASSERT_EQUAL_UINT32(125u, restored.cycleTeardownAckTimeoutMs);
    TEST_ASSERT_EQUAL_STRING("Road", restored.slot0_default.profileName.c_str());
    TEST_ASSERT_EQUAL_INT(V1_MODE_LOGIC, restored.slot0_default.mode);

    V1Profile restoredProfile;
    TEST_ASSERT_TRUE(v1ProfileManager.loadProfile("Road", restoredProfile));
    TEST_ASSERT_EQUAL_STRING("Restored profile", restoredProfile.description.c_str());
    TEST_ASSERT_EQUAL_UINT8(1, restoredProfile.settings.bytes[0]);
    TEST_ASSERT_EQUAL_UINT8(6, restoredProfile.settings.bytes[5]);
}

void test_serialized_backup_payload_matches_builder_and_writes_same_json() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager manager;
    V1Settings& settings = manager.mutableSettings();
    settings.apSSID = "PayloadTest";
    settings.brightness = 77;
    settings.proxyBLE = false;

    V1Profile profile("Road");
    profile.description = "Serialized";
    ProfileSaveResult saveResult = v1ProfileManager.saveProfile(profile);
    TEST_ASSERT_TRUE(saveResult.success);

    JsonDocument expectedDoc;
    const BackupPayloadBuilder::BuildResult buildResult =
        BackupPayloadBuilder::buildBackupDocument(expectedDoc,
                                                  settings,
                                                  v1ProfileManager,
                                                  BackupPayloadBuilder::BackupTransport::SdBackup,
                                                  4321);

    SerializedSettingsBackupPayload payload;
    TEST_ASSERT_TRUE(buildSerializedSdBackupPayload(payload, settings, v1ProfileManager, 4321));
    TEST_ASSERT_EQUAL_UINT32(4321u, payload.snapshotMs);
    TEST_ASSERT_EQUAL_INT(buildResult.profilesBackedUp, payload.profilesBackedUp);
    TEST_ASSERT_NOT_NULL(payload.data);
    TEST_ASSERT_TRUE(payload.length > 0);

    std::string expected;
    serializeJson(expectedDoc, expected);
    TEST_ASSERT_EQUAL_UINT(expected.size(), payload.length);
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), payload.data, payload.length);

    TEST_ASSERT_TRUE(writeBackupAtomically(&fs, payload));
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), readFileToString(fs, SETTINGS_BACKUP_PATH).c_str());

    releaseSerializedSettingsBackupPayload(payload);
    TEST_ASSERT_NULL(payload.data);
}

void test_device_batch_update_skips_noop_persist_and_saves_once_on_change() {
    SettingsManager manager;

    DeviceSettingsUpdate emptyUpdate;
    manager.applyDeviceSettingsUpdate(emptyUpdate);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    DeviceSettingsUpdate sameValueUpdate;
    sameValueUpdate.hasProxyBLE = true;
    sameValueUpdate.proxyBLE = manager.get().proxyBLE;
    manager.applyDeviceSettingsUpdate(sameValueUpdate);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    DeviceSettingsUpdate changedUpdate;
    changedUpdate.hasProxyBLE = true;
    changedUpdate.proxyBLE = !manager.get().proxyBLE;
    manager.applyDeviceSettingsUpdate(changedUpdate);
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
    TEST_ASSERT_TRUE(activeNamespaceOrEmpty().length() > 0);
}

void test_proxy_mode_disables_obd_setting() {
    SettingsManager manager;
    manager.mutableSettings().proxyBLE = false;
    manager.mutableSettings().obdEnabled = true;

    DeviceSettingsUpdate update;
    update.hasProxyBLE = true;
    update.proxyBLE = true;
    manager.applyDeviceSettingsUpdate(update);

    TEST_ASSERT_TRUE(manager.get().proxyBLE);
    TEST_ASSERT_FALSE(manager.get().obdEnabled);
}

void test_obd_mode_disables_proxy_setting() {
    SettingsManager manager;
    manager.mutableSettings().proxyBLE = true;
    manager.mutableSettings().obdEnabled = false;

    ObdSettingsUpdate update;
    update.hasEnabled = true;
    update.enabled = true;
    manager.applyObdSettingsUpdate(update);

    TEST_ASSERT_TRUE(manager.get().obdEnabled);
    TEST_ASSERT_FALSE(manager.get().proxyBLE);
}

void test_load_heals_legacy_proxy_obd_conflict_to_obd_mode() {
    SettingsManager original;
    original.mutableSettings().proxyBLE = true;
    original.mutableSettings().obdEnabled = true;
    original.save();

    SettingsManager reloaded;
    reloaded.load();

    TEST_ASSERT_TRUE(reloaded.get().obdEnabled);
    TEST_ASSERT_FALSE(reloaded.get().proxyBLE);
}

void test_backup_restore_heals_legacy_proxy_obd_conflict_to_obd_mode() {
    SettingsManager manager;

    JsonDocument doc;
    doc["proxyBLE"] = true;
    doc["obdEnabled"] = true;

    const SettingsBackupApplyResult applyResult = manager.applyBackupDocument(doc, false);

    TEST_ASSERT_TRUE(applyResult.success);
    TEST_ASSERT_TRUE(manager.get().obdEnabled);
    TEST_ASSERT_FALSE(manager.get().proxyBLE);
}

void test_quiet_batch_update_skips_noop_persist_and_saves_once_on_change() {
    SettingsManager manager;

    QuietSettingsUpdate emptyUpdate;
    manager.applyQuietSettingsUpdate(emptyUpdate);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    QuietSettingsUpdate sameValueUpdate;
    sameValueUpdate.hasAlertVolumeFadeEnabled = true;
    sameValueUpdate.alertVolumeFadeEnabled = manager.get().alertVolumeFadeEnabled;
    manager.applyQuietSettingsUpdate(sameValueUpdate);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    QuietSettingsUpdate changedUpdate;
    changedUpdate.hasAlertVolumeFadeEnabled = true;
    changedUpdate.alertVolumeFadeEnabled = !manager.get().alertVolumeFadeEnabled;
    manager.applyQuietSettingsUpdate(changedUpdate);
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
    TEST_ASSERT_TRUE(activeNamespaceOrEmpty().length() > 0);
}

void test_display_batch_update_skips_noop_persist_and_saves_once_on_change() {
    SettingsManager manager;

    DisplaySettingsUpdate emptyUpdate;
    manager.applyDisplaySettingsUpdate(emptyUpdate);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    DisplaySettingsUpdate sameValueUpdate;
    sameValueUpdate.hasColorBogey = true;
    sameValueUpdate.colorBogey = manager.get().colorBogey;
    manager.applyDisplaySettingsUpdate(sameValueUpdate);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    DisplaySettingsUpdate changedUpdate;
    changedUpdate.hasColorBogey = true;
    changedUpdate.colorBogey = static_cast<uint16_t>(manager.get().colorBogey ^ 0x00FFu);
    manager.applyDisplaySettingsUpdate(changedUpdate);
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
    TEST_ASSERT_TRUE(activeNamespaceOrEmpty().length() > 0);
}

void test_immediate_nvs_deferred_backup_mode_survives_reboot_before_backup_writer() {
    SettingsManager manager;

    DisplaySettingsUpdate update;
    update.hasBrightness = true;
    update.brightness = 77;
    manager.applyDisplaySettingsUpdate(update, SettingsPersistMode::ImmediateNvsDeferredBackup);

    const String activeNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(activeNs.length() > 0);
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
    TEST_ASSERT_TRUE(manager.deferredBackupPending());
    TEST_ASSERT_EQUAL_UINT8(77, mock_preferences::getUnsigned(activeNs.c_str(), kNvsBrightness, 0));

    SettingsManager reloaded;
    reloaded.load();

    TEST_ASSERT_EQUAL_UINT8(77, reloaded.get().brightness);
}

void test_obd_batch_update_skips_noop_persist_and_defers_one_save_on_change() {
    SettingsManager manager;

    ObdSettingsUpdate emptyUpdate;
    manager.applyObdSettingsUpdate(emptyUpdate, SettingsPersistMode::Deferred);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    ObdSettingsUpdate sameValueUpdate;
    sameValueUpdate.hasEnabled = true;
    sameValueUpdate.enabled = manager.get().obdEnabled;
    manager.applyObdSettingsUpdate(sameValueUpdate, SettingsPersistMode::Deferred);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    ObdSettingsUpdate changedUpdate;
    changedUpdate.hasEnabled = true;
    changedUpdate.enabled = true;
    changedUpdate.hasObdScanWindowMs = true;
    changedUpdate.obdScanWindowMs = 20000;
    changedUpdate.hasObdRetryIntervalMs = true;
    changedUpdate.obdRetryIntervalMs = 150000;
    changedUpdate.hasWifiOpenTimeoutMs = true;
    changedUpdate.wifiOpenTimeoutMs = 28000;
    manager.applyObdSettingsUpdate(changedUpdate, SettingsPersistMode::Deferred);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_TRUE(manager.deferredPersistPending());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());
    TEST_ASSERT_EQUAL_UINT32(20000u, manager.get().obdScanWindowMs);
    TEST_ASSERT_EQUAL_UINT32(150000u, manager.get().obdRetryIntervalMs);
    TEST_ASSERT_EQUAL_UINT32(28000u, manager.get().wifiOpenTimeoutMs);

    manager.serviceDeferredPersist(manager.deferredPersistNextAttemptAtMs());
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
    TEST_ASSERT_FALSE(manager.deferredPersistPending());
    TEST_ASSERT_TRUE(activeNamespaceOrEmpty().length() > 0);
}

void test_autopush_slot_batch_update_skips_noop_persist_and_saves_once_on_change() {
    SettingsManager manager;

    AutoPushSlotUpdate emptyUpdate;
    manager.applyAutoPushSlotUpdate(emptyUpdate);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    const V1Settings::ConstAutoPushSlotView slot = manager.get().autoPushSlotView(1);

    AutoPushSlotUpdate sameValueUpdate;
    sameValueUpdate.slot = 1;
    sameValueUpdate.hasProfileName = true;
    sameValueUpdate.profileName = slot.config.profileName;
    sameValueUpdate.hasMode = true;
    sameValueUpdate.mode = slot.config.mode;
    manager.applyAutoPushSlotUpdate(sameValueUpdate);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    AutoPushSlotUpdate changedUpdate;
    changedUpdate.slot = 1;
    changedUpdate.hasProfileName = true;
    changedUpdate.profileName = "Road";
    changedUpdate.hasMode = true;
    changedUpdate.mode = V1_MODE_LOGIC;
    manager.applyAutoPushSlotUpdate(changedUpdate);
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
    TEST_ASSERT_TRUE(activeNamespaceOrEmpty().length() > 0);
}

void test_autopush_state_batch_update_skips_noop_persist_and_saves_once_on_change() {
    SettingsManager manager;

    AutoPushStateUpdate emptyUpdate;
    manager.applyAutoPushStateUpdate(emptyUpdate);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    AutoPushStateUpdate sameValueUpdate;
    sameValueUpdate.hasActiveSlot = true;
    sameValueUpdate.activeSlot = manager.get().activeSlot;
    sameValueUpdate.hasEnabled = true;
    sameValueUpdate.enabled = manager.get().autoPushEnabled;
    manager.applyAutoPushStateUpdate(sameValueUpdate);
    TEST_ASSERT_EQUAL_UINT32(1u, manager.backupRevision());
    TEST_ASSERT_EQUAL_STRING("", activeNamespaceOrEmpty().c_str());

    AutoPushStateUpdate changedUpdate;
    changedUpdate.hasActiveSlot = true;
    changedUpdate.activeSlot = 2;
    changedUpdate.hasEnabled = true;
    changedUpdate.enabled = true;
    manager.applyAutoPushStateUpdate(changedUpdate);
    TEST_ASSERT_EQUAL_UINT32(2u, manager.backupRevision());
    TEST_ASSERT_TRUE(activeNamespaceOrEmpty().length() > 0);
}

void test_partial_recovery_restores_both_speed_mute_fields() {
    // The partial-recovery branch in checkAndRestoreFromSD previously restored
    // speedMuteThresholdMph but not speedMuteHysteresisMph.  Verify both fields
    // are recovered when restoreFromSD fails (mutex blocked) but a valid SD
    // backup exists.
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    // Build and write a backup with non-default speed-mute values.
    SettingsManager source;
    V1Settings& src = source.mutableSettings();
    src.speedMuteEnabled = true;
    src.speedMuteThresholdMph = 45;
    src.speedMuteHysteresisMph = 7;

    SerializedSettingsBackupPayload payload;
    TEST_ASSERT_TRUE(buildSerializedSdBackupPayload(
        payload, source.get(), v1ProfileManager, 5000));
    TEST_ASSERT_TRUE(writeBackupAtomically(&fs, payload));
    releaseSerializedSettingsBackupPayload(payload);

    // Fresh target: NVS empty → checkNeedsRestore() returns true.
    // Block the SD mutex so restoreFromSD() fails and partial-recovery runs.
    StorageManager::resetMockSdLockState();
    StorageManager::mockSdLockState.failNextBlockingLock = true;

    SettingsManager target;
    target.checkAndRestoreFromSD();

    TEST_ASSERT_EQUAL_UINT8(45, target.get().speedMuteThresholdMph);
    TEST_ASSERT_EQUAL_UINT8(7, target.get().speedMuteHysteresisMph);
}

void test_restore_pending_survives_no_sd_save_and_sd_backup_wins_next_boot() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager source;
    V1Settings& sourceSettings = source.mutableSettings();
    sourceSettings.apSSID = "SD-Rig";
    sourceSettings.proxyName = "SD-Proxy";
    sourceSettings.brightness = 88;
    sourceSettings.autoPushEnabled = true;
    sourceSettings.slot0_default = AutoPushSlot("Road", V1_MODE_LOGIC);

    SerializedSettingsBackupPayload payload;
    TEST_ASSERT_TRUE(buildSerializedSdBackupPayload(
        payload, source.get(), v1ProfileManager, 5000));
    TEST_ASSERT_TRUE(writeBackupAtomically(&fs, payload));
    releaseSerializedSettingsBackupPayload(payload);

    JsonDocument originalBackup;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, originalBackup));
    TEST_ASSERT_EQUAL_INT(88, originalBackup["brightness"].as<int>());
    TEST_ASSERT_EQUAL_STRING("SD-Rig", originalBackup["apSSID"].as<const char*>());

    // Clean-flash + no SD: settings restore is detected, but there is no SD
    // available yet.  A later shutdown/user-save must not promote factory
    // defaults to authoritative NVS.
    mock_preferences::reset();
    storageManager.reset();
    v1ProfileManager = V1ProfileManager();

    SettingsManager noSdBoot;
    noSdBoot.begin();
    noSdBoot.save();

    const String provisionalNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(provisionalNs.length() > 0);
    TEST_ASSERT_TRUE(
        mock_preferences::namespaceHasKey(provisionalNs.c_str(), kNvsRestorePending));
    TEST_ASSERT_TRUE(
        mock_preferences::getUnsigned(provisionalNs.c_str(), kNvsRestorePending, 0) != 0);

    // If SD appears while restore is still pending, a backup write from the
    // provisional/default NVS must refuse to overwrite the user's SD backup.
    storageManager.setFilesystem(&fs, true);
    SettingsManager pendingWriter;
    pendingWriter.load();
    pendingWriter.mutableSettings().apSSID = "Factory-Default";
    pendingWriter.mutableSettings().brightness = 200;
    TEST_ASSERT_FALSE(pendingWriter.backupToSD());

    JsonDocument preservedBackup;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, preservedBackup));
    TEST_ASSERT_EQUAL_INT(88, preservedBackup["brightness"].as<int>());
    TEST_ASSERT_EQUAL_STRING("SD-Rig", preservedBackup["apSSID"].as<const char*>());

    // Next boot with SD follows the real order: settings load before storage,
    // then checkAndRestoreFromSD runs after the SD/profile layer is mounted.
    storageManager.reset();
    v1ProfileManager = V1ProfileManager();

    SettingsManager bootWithSd;
    bootWithSd.begin();

    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));
    TEST_ASSERT_TRUE(bootWithSd.checkAndRestoreFromSD());

    const V1Settings& restored = bootWithSd.get();
    TEST_ASSERT_EQUAL_STRING("SD-Rig", restored.apSSID.c_str());
    TEST_ASSERT_EQUAL_STRING("SD-Proxy", restored.proxyName.c_str());
    TEST_ASSERT_EQUAL_UINT8(88, restored.brightness);
    TEST_ASSERT_TRUE(restored.autoPushEnabled);
    TEST_ASSERT_EQUAL_STRING("Road", restored.slot0_default.profileName.c_str());
    TEST_ASSERT_EQUAL_INT(V1_MODE_LOGIC, restored.slot0_default.mode);

    const String restoredNs = activeNamespaceOrEmpty();
    TEST_ASSERT_TRUE(restoredNs.length() > 0);
    TEST_ASSERT_FALSE(
        mock_preferences::namespaceHasKey(restoredNs.c_str(), kNvsRestorePending));

    JsonDocument refreshedBackup;
    TEST_ASSERT_TRUE(loadJsonFile(fs, SETTINGS_BACKUP_PATH, refreshedBackup));
    TEST_ASSERT_EQUAL_INT(88, refreshedBackup["brightness"].as<int>());
    TEST_ASSERT_EQUAL_STRING("SD-Rig", refreshedBackup["apSSID"].as<const char*>());
}

void test_gps_fields_round_trip_through_backup_and_restore() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager source;
    V1Settings& src = source.mutableSettings();
    src.gpsEnabled             = true;
    src.gpsBaud                = 38400;
    src.gpsEnablePinActiveHigh = false;
    src.gpsLogUtcToPerf        = false;
    src.gpsLogUtcToAlp         = false;

    SerializedSettingsBackupPayload payload;
    TEST_ASSERT_TRUE(buildSerializedSdBackupPayload(
        payload, source.get(), v1ProfileManager, 5000));
    TEST_ASSERT_TRUE(writeBackupAtomically(&fs, payload));
    releaseSerializedSettingsBackupPayload(payload);

    SettingsManager target;
    TEST_ASSERT_TRUE(target.restoreFromSD());

    TEST_ASSERT_TRUE(target.get().gpsEnabled);
    TEST_ASSERT_EQUAL_UINT32(38400u, target.get().gpsBaud);
    TEST_ASSERT_FALSE(target.get().gpsEnablePinActiveHigh);
    TEST_ASSERT_FALSE(target.get().gpsLogUtcToPerf);
    TEST_ASSERT_FALSE(target.get().gpsLogUtcToAlp);
}

void test_gps_fields_partial_recovery() {
    fs::FS fs(g_tempRoot);
    storageManager.setFilesystem(&fs, true);
    TEST_ASSERT_TRUE(v1ProfileManager.begin(&fs));

    SettingsManager source;
    V1Settings& src = source.mutableSettings();
    src.gpsEnabled             = true;
    src.gpsBaud                = 115200;
    src.gpsEnablePinActiveHigh = true;
    src.gpsLogUtcToPerf        = true;
    src.gpsLogUtcToAlp         = false;

    SerializedSettingsBackupPayload payload;
    TEST_ASSERT_TRUE(buildSerializedSdBackupPayload(
        payload, source.get(), v1ProfileManager, 5000));
    TEST_ASSERT_TRUE(writeBackupAtomically(&fs, payload));
    releaseSerializedSettingsBackupPayload(payload);

    StorageManager::resetMockSdLockState();
    StorageManager::mockSdLockState.failNextBlockingLock = true;

    SettingsManager target;
    target.checkAndRestoreFromSD();

    TEST_ASSERT_TRUE(target.get().gpsEnabled);
    TEST_ASSERT_EQUAL_UINT32(115200u, target.get().gpsBaud);
    TEST_ASSERT_TRUE(target.get().gpsEnablePinActiveHigh);
    TEST_ASSERT_TRUE(target.get().gpsLogUtcToPerf);
    TEST_ASSERT_FALSE(target.get().gpsLogUtcToAlp);
}

void test_gps_device_batch_update_saves_all_fields() {
    SettingsManager manager;

    DeviceSettingsUpdate update{};
    update.hasGpsEnabled             = true;  update.gpsEnabled             = true;
    update.hasGpsBaud                = true;  update.gpsBaud                = 38400;
    update.hasGpsEnablePinActiveHigh = true;  update.gpsEnablePinActiveHigh = false;
    update.hasGpsLogUtcToPerf        = true;  update.gpsLogUtcToPerf        = false;
    update.hasGpsLogUtcToAlp         = true;  update.gpsLogUtcToAlp         = false;
    manager.applyDeviceSettingsUpdate(update, SettingsPersistMode::Deferred);

    TEST_ASSERT_TRUE(manager.get().gpsEnabled);
    TEST_ASSERT_EQUAL_UINT32(38400u, manager.get().gpsBaud);
    TEST_ASSERT_FALSE(manager.get().gpsEnablePinActiveHigh);
    TEST_ASSERT_FALSE(manager.get().gpsLogUtcToPerf);
    TEST_ASSERT_FALSE(manager.get().gpsLogUtcToAlp);
}

// M-04: applyDisplaySettingsUpdate must sanitize incoming color values the same
// way NVS-load and SD-restore do.  A zero value (0x0000 — display-blackout)
// submitted via the HTTP PUT path must NOT reach the settings store; the current
// stored color must be retained instead.
void test_display_update_rejects_zero_color_keeps_current() {
    SettingsManager manager;
    const uint16_t originalBogey = manager.get().colorBogey;
    TEST_ASSERT_NOT_EQUAL(0x0000u, originalBogey);  // default must be non-zero

    DisplaySettingsUpdate update;
    update.hasColorBogey = true;
    update.colorBogey = 0x0000;  // zero — should be rejected
    manager.applyDisplaySettingsUpdate(update);

    TEST_ASSERT_EQUAL_HEX16(originalBogey, manager.get().colorBogey);
}

void test_display_update_accepts_nonzero_color() {
    SettingsManager manager;

    DisplaySettingsUpdate update;
    update.hasColorBandKa = true;
    update.colorBandKa = 0x07E0;  // green — valid
    manager.applyDisplaySettingsUpdate(update);

    TEST_ASSERT_EQUAL_HEX16(0x07E0u, manager.get().colorBandKa);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_absent_auto_push_key_uses_authoritative_default);
    RUN_TEST(test_save_load_and_backup_round_trip_current_shape_fields);
    RUN_TEST(test_ap_password_restored_from_backup_when_key_present);
    RUN_TEST(test_ap_password_preserved_when_backup_lacks_key);
    RUN_TEST(test_wifi_client_password_is_omitted_by_default_and_optional_for_export);
    RUN_TEST(test_wifi_client_password_restores_from_sd_backup_to_nvs);
    RUN_TEST(test_wifi_sta_slot_sd_secret_retains_multiple_passwords);
    RUN_TEST(test_wifi_sta_slot_sd_secret_upgrade_preserves_legacy_top_level_secret);
    RUN_TEST(test_wifi_client_password_export_restores_to_slot_nvs_when_secret_file_missing);
    RUN_TEST(test_wifi_sta_slots_restore_without_passwords_clears_stale_secrets);
    RUN_TEST(test_legacy_station_backup_restores_to_slot0);
    RUN_TEST(test_wifi_sta_slots_round_trip_and_primary_alias);
    RUN_TEST(test_wifi_client_disabled_flag_survives_reboot_with_saved_slots);
    RUN_TEST(test_legacy_wifi_client_enabled_missing_still_heals_saved_slots);
    RUN_TEST(test_backup_restore_preserves_explicit_wifi_client_disabled_with_saved_slots);
    RUN_TEST(test_backup_restore_heals_missing_wifi_client_enabled_with_saved_slots);
    RUN_TEST(test_legacy_wifi_client_credentials_migrate_to_slot0);
    RUN_TEST(test_apply_backup_document_unifies_restore_field_coverage_and_profile_restore);
    RUN_TEST(test_serialized_backup_payload_matches_builder_and_writes_same_json);
    RUN_TEST(test_device_batch_update_skips_noop_persist_and_saves_once_on_change);
    RUN_TEST(test_proxy_mode_disables_obd_setting);
    RUN_TEST(test_obd_mode_disables_proxy_setting);
    RUN_TEST(test_load_heals_legacy_proxy_obd_conflict_to_obd_mode);
    RUN_TEST(test_backup_restore_heals_legacy_proxy_obd_conflict_to_obd_mode);
    RUN_TEST(test_quiet_batch_update_skips_noop_persist_and_saves_once_on_change);
    RUN_TEST(test_display_batch_update_skips_noop_persist_and_saves_once_on_change);
    RUN_TEST(test_immediate_nvs_deferred_backup_mode_survives_reboot_before_backup_writer);
    RUN_TEST(test_obd_batch_update_skips_noop_persist_and_defers_one_save_on_change);
    RUN_TEST(test_autopush_slot_batch_update_skips_noop_persist_and_saves_once_on_change);
    RUN_TEST(test_autopush_state_batch_update_skips_noop_persist_and_saves_once_on_change);
    RUN_TEST(test_partial_recovery_restores_both_speed_mute_fields);
    RUN_TEST(test_restore_pending_survives_no_sd_save_and_sd_backup_wins_next_boot);
    RUN_TEST(test_gps_fields_round_trip_through_backup_and_restore);
    RUN_TEST(test_gps_fields_partial_recovery);
    RUN_TEST(test_gps_device_batch_update_saves_all_fields);
    RUN_TEST(test_display_update_rejects_zero_color_keeps_current);
    RUN_TEST(test_display_update_accepts_nonzero_color);
    return UNITY_END();
}
