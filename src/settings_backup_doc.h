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

enum class WifiClientSecretReadStatus : uint8_t {
    NotFound,
    Valid,
    Invalid,
    Unavailable,
};

// Snapshot of the WiFi client secret recorded on the SD card. The status is
// retained so transient PSRAM or I/O failure cannot be mistaken for absence
// and trigger a destructive healing decision.
struct WifiClientSecretPresence {
    WifiClientSecretReadStatus status = WifiClientSecretReadStatus::NotFound;
    String ssid;

    bool valid() const { return status == WifiClientSecretReadStatus::Valid; }
};

WifiClientSecretPresence readWifiClientSecretPresence(fs::FS* fs);

class StorageManager;
bool clearWifiStaSlotPasswordsForRestore(StorageManager& storage, bool clearSdSecret);

enum class BackupRestoreScope : uint8_t {
    CriticalRecovery,
    Full,
};

bool applyBackupNetworkFields(const JsonDocument& doc, V1Settings& settings, StorageManager& storage,
                              BackupRestoreScope scope, bool clearSdSecret);
void applyBackupDisplayFields(const JsonDocument& doc, V1Settings& settings, BackupRestoreScope scope);
void applyBackupAudioFields(const JsonDocument& doc, V1Settings& settings, BackupRestoreScope scope);
bool applyBackupProfileSlotFields(const JsonDocument& doc, V1Settings& settings, BackupRestoreScope scope);
void applyBackupAlpAndGpsFields(const JsonDocument& doc, V1Settings& settings);
void healBackupRestoreConflicts(V1Settings& settings, const char* context);
bool applyBackupCriticalFieldsAtomically(const JsonDocument& doc, V1Settings& settings,
                                         StorageManager& storage,
                                         bool (*persist)(void* ctx) = nullptr,
                                         void* persistCtx = nullptr);
bool applyBackupWifiClientHealingAtomically(const JsonDocument& doc, V1Settings& settings,
                                            StorageManager& storage, bool& recovered,
                                            bool (*persist)(void* ctx) = nullptr,
                                            void* persistCtx = nullptr);

// Full transaction preflight used by both direct restore and the boot-time
// critical-recovery fallback. It performs no mutation.
bool backupDocumentCanApply(const JsonDocument& doc, const V1Settings& current, V1ProfileManager& profiles);

bool backupFieldMatchesBool(const JsonDocument& doc, const char* key, bool expected);
bool backupFieldMatchesInt(const JsonDocument& doc, const char* key, int expected);
bool backupFieldMatchesString(const JsonDocument& doc, const char* key, const String& expected);
bool backupAppearsInSyncWithNvs(const JsonDocument& doc, const V1Settings& current);
