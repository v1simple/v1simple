/**
 * Backup-document helpers for settings restore paths.
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
class StorageManager;
bool clearWifiStaSlotPasswordsForRestore(StorageManager& storage, bool clearSdSecret);
bool restoreWifiStaSlotsFromBackupDoc(const JsonDocument& doc, V1Settings& settings, StorageManager& storage,
                                      bool clearSdSecret);

enum class BackupRestoreScope : uint8_t {
    CriticalRecovery,
    Full,
};

bool applyBackupNetworkFields(const JsonDocument& doc, V1Settings& settings, StorageManager& storage,
                              BackupRestoreScope scope, bool clearSdSecret);
void applyBackupDisplayFields(const JsonDocument& doc, V1Settings& settings, BackupRestoreScope scope);
void applyBackupAudioFields(const JsonDocument& doc, V1Settings& settings, BackupRestoreScope scope);
void applyBackupProfileSlotFields(const JsonDocument& doc, V1Settings& settings, BackupRestoreScope scope);
void applyBackupObdFields(const JsonDocument& doc, V1Settings& settings, BackupRestoreScope scope);
void applyBackupAlpAndGpsFields(const JsonDocument& doc, V1Settings& settings);
void healBackupRestoreConflicts(V1Settings& settings, const char* context);

// Full transaction preflight used by both direct restore and the boot-time
// critical-recovery fallback. It performs no mutation.
bool backupDocumentCanApply(const JsonDocument& doc, const V1Settings& current, V1ProfileManager& profiles);

bool backupFieldMatchesBool(const JsonDocument& doc, const char* key, bool expected);
bool backupFieldMatchesInt(const JsonDocument& doc, const char* key, int expected);
bool backupFieldMatchesString(const JsonDocument& doc, const char* key, const String& expected);
bool backupAppearsInSyncWithNvs(const JsonDocument& doc, const V1Settings& current);
