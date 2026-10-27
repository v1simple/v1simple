#pragma once

#include <cstdint>

enum class SettingsSaveObservation : uint8_t { Unknown, Succeeded, Failed };
enum class BackupWriteObservation : uint8_t { Unknown, Written, Preserved, Failed, Superseded, Unavailable };

// Observations from this boot, not a media-integrity or complete-device health
// verdict. Reading this value must never inspect storage or attempt recovery.
struct SettingsRecoveryStatus {
    SettingsSaveObservation internalSave = SettingsSaveObservation::Unknown;
    BackupWriteObservation lastAttempt = BackupWriteObservation::Unknown;
    bool internalSavePending = false;
    bool storageReady = false;
    bool sdMounted = false;
    bool recoveryPending = false;
    bool restorePending = false;
    bool backupQueued = false;
    bool backupWriting = false;
    bool backupRetryPending = false;
    bool hasWrittenSnapshot = false;
    bool lastWriteMatchesCurrent = false;
    uint32_t currentRevision = 0;
    uint32_t lastWrittenRevision = 0;
};
