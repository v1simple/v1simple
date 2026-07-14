/**
 * Backup-document helpers for the settings restore paths.
 * Extracted from settings_restore.cpp to keep the SD restore and validation
 * member methods focused. Declarations for src/settings_backup_doc.cpp.
 *
 * loadBestBackupDocument() and parseBoolVariant() are declared in
 * settings_internals.h (other settings TUs already depend on them there).
 */

#pragma once

#include "settings_internals.h"

// Snapshot of the WiFi client keys present in an NVS settings namespace.
struct WifiClientKeyPresence {
    bool enabledKeyPresent = false;
    bool ssidKeyPresent = false;
};

WifiClientKeyPresence readWifiClientKeyPresence(const char* settingsNamespace);

// Snapshot of the WiFi client secret recorded on the SD card.
struct WifiClientSecretPresence {
    bool valid = false;
    String ssid;
};

WifiClientSecretPresence readWifiClientSecretPresence(fs::FS* fs);

bool restoreWifiClientPasswordObfFromBackupDoc(const JsonDocument& doc, const String& expectedSsid);
String legacyWifiClientSsidFromBackupDoc(const JsonDocument& doc);
bool restoreLegacyStationPasswordFromBackupDoc(const JsonDocument& doc, const String& expectedSsid);
bool restoreWifiStaSlotPasswordObfFromBackupSlot(JsonObjectConst slotObj, size_t index);
void clearWifiStaSlotPasswordsForRestore(bool clearSdSecret);
bool restoreWifiStaSlotsFromBackupDoc(const JsonDocument& doc, V1Settings& settings, bool clearSdSecret);

bool backupFieldMatchesBool(const JsonDocument& doc, const char* key, bool expected);
bool backupFieldMatchesInt(const JsonDocument& doc, const char* key, int expected);
bool backupFieldMatchesString(const JsonDocument& doc, const char* key, const String& expected);
bool backupAppearsInSyncWithNvs(const JsonDocument& doc, const V1Settings& current);
