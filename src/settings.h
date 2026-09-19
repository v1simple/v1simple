/**
 * Settings storage manager for V1 Gen2 Display.
 *
 * Load/save operations should be called from the main thread.
 */

#pragma once
#ifndef SETTINGS_H
#define SETTINGS_H

#include <ArduinoJson.h>
#include <FS.h>
#include <Preferences.h>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "settings_types.h"

class StorageManager;
class V1ProfileManager;
class V1DeviceStore;
struct ProfileOperationResult;
struct V1DeviceMutationResult;

class SettingsManager {
  public:
    SettingsManager(StorageManager& storage, V1ProfileManager& profiles);

    // Initialize and load settings
    void begin();

    // Get current settings (read-only)
    const V1Settings& get() const { return settings_; }
#ifdef UNIT_TEST
    // Test-only mutable access for fixture seeding.
    V1Settings& mutableSettings() { return settings_; }
    void utSetDisplayConfigurationRevision(uint32_t revision) { displayConfigurationRevision_ = revision; }
    void utInterruptWifiCredentialBeforeSettingsCommit(bool enabled) {
        wifiCredentialInterruptBeforeSettingsCommit_ = enabled;
    }
    void utInterruptRestoreAfterCredentials(bool enabled) {
        restoreInterruptAfterCredentials_ = enabled;
    }
    void utInterruptRestoreAfterProfiles(bool enabled) {
        restoreInterruptAfterProfiles_ = enabled;
    }
    void utInterruptAutoPushMigrationAfterProfiles(bool enabled) {
        autoPushMigrationInterruptAfterProfiles_ = enabled;
    }
    void utInterruptProfileDeleteAfterJournal(bool enabled) {
        profileDeleteInterruptAfterJournal_ = enabled;
    }
    void utInterruptProfileDeleteAfterProfile(bool enabled) {
        profileDeleteInterruptAfterProfile_ = enabled;
    }
    void utInterruptProfileDeleteAfterReferences(bool enabled) {
        profileDeleteInterruptAfterReferences_ = enabled;
    }
    void utLeaveRestoreJournalAfterCommit(bool enabled) {
        leaveRestoreJournalAfterCommit_ = enabled;
    }
    void utLeaveProfileDeleteJournalAfterCommit(bool enabled) {
        leaveProfileDeleteJournalAfterCommit_ = enabled;
    }
    void utFailRestoreApplyStagingAllocation(bool enabled) {
        restoreApplyStagingAllocationFailure_ = enabled;
    }
#endif
    uint32_t backupRevision() const { return backupRevisionCounter_; }
    uint32_t backupDueRevision() const { return backupDueRevision_; }
    // Normal-runtime display policy mutations, independent of NVS success.
    // Saturation means continuity can no longer be established from this value.
    uint32_t displayConfigurationRevision() const { return displayConfigurationRevision_; }

    uint8_t getApTimeoutMinutes() const { return settings_.apTimeoutMinutes; }
    SettingsPersistResult setActiveSlot(int slot,
                                        SettingsPersistMode persistMode = SettingsPersistMode::Immediate);
    SettingsPersistResult setStealthEnabled(bool enabled,
                                            SettingsPersistMode persistMode = SettingsPersistMode::Immediate);
    void setLastV1Address(const String& addr);
    // One-key NVS safety net used only when filesystem-backed V1DeviceStore is
    // unavailable. Writes are deferred through serviceDeferredPersist().
    String loadLastV1AddressFallback();
    void requestLastV1AddressFallbackPersist(const String& addr);
    bool clearLastV1AddressFallback(const String& addressFilter = "");
    // Couple the runtime NVS fallback with the filesystem-backed device row.
    // Once its intent marker is durable, interruption is reported as pending
    // and boot resumes the same idempotent delete before fallback bootstrap.
    V1DeviceMutationResult deleteV1DeviceTransactional(const String& address, V1DeviceStore& devices);
    bool resolvePendingV1DeviceDelete(V1DeviceStore& devices);

    const AutoPushSlot& getSlot(int slotNum) const;

    // Get slot volume settings (returns 0xFF for "no change")
    uint8_t getSlotVolume(int slotNum) const;
    uint8_t getSlotMuteVolume(int slotNum) const;

    // Get slot dark mode and MZ settings
    bool getSlotDarkMode(int slotNum) const;
    bool getSlotMuteToZero(int slotNum) const;
    uint8_t getSlotAlertPersistSec(int slotNum) const;
    bool getSlotPriorityArrowOnly(int slotNum) const;

    // ALP display persistence — global (not per-slot) because ALP is a peer
    // source, independent of V1 auto-push profiles. Clamped 0..5 like V1.
    uint8_t getAlpAlertPersistSec() const { return settings_.alpAlertPersistSec; }

    SettingsPersistResult applyDeviceSettingsUpdate(
        const DeviceSettingsUpdate& update, SettingsPersistMode persistMode = SettingsPersistMode::Immediate);
    SettingsPersistResult applyDisplaySettingsUpdate(
        const DisplaySettingsUpdate& update, SettingsPersistMode persistMode = SettingsPersistMode::Immediate);
    SettingsPersistResult resetDisplaySettings(SettingsPersistMode persistMode = SettingsPersistMode::Immediate);
    SettingsPersistResult applyAudioSettingsUpdate(
        const AudioSettingsUpdate& update, SettingsPersistMode persistMode = SettingsPersistMode::Immediate);
    SettingsPersistResult applyObdSettingsUpdate(
        const ObdSettingsUpdate& update, SettingsPersistMode persistMode = SettingsPersistMode::Immediate);
    bool applyAutoPushSlotUpdate(const AutoPushSlotUpdate& update,
                                 SettingsPersistMode persistMode = SettingsPersistMode::Immediate);
    AutoPushPersistResult applyAutoPushSlotUpdatePersisted(const AutoPushSlotUpdate& update);
    SettingsPersistResult applyAutoPushStateUpdate(
        const AutoPushStateUpdate& update, SettingsPersistMode persistMode = SettingsPersistMode::Immediate);

    // Batch update methods (don't auto-save, call save() after)
    void updateBrightness(uint8_t brightness) { settings_.brightness = brightness; }
    void updateVoiceVolume(uint8_t volume) { settings_.voiceVolume = volume; }
    // Persist settings atomically to NVS, then synchronously back them up to SD.
    // Reserve this for explicit durability boundaries outside latency-sensitive
    // loop paths.
    bool save();

    // Clear all references to a profile and persist the reconciliation before
    // the profile file is deleted. Returns false without changing RAM on NVS failure.
    bool clearProfileReferencesPersisted(const String& canonicalProfileName, bool& changed);
    // Delete a profile and its slot assignments as one recoverable operation.
    // A reset or write failure converges to either the old profile+assignments
    // or the deleted profile+cleared assignments, never a dangling reference.
    ProfileOperationResult deleteProfileAndReferences(const String& canonicalProfileName);
    // Persist settings atomically to NVS, then coalesce a deferred SD backup.
    // Returns false when the NVS persist failed (settings remain RAM-only).
    bool saveDeferredBackup();
    void requestDeferredPersist();
    void serviceDeferredPersist(uint32_t nowMs);
    bool deferredPersistPending() const;
    bool deferredPersistRetryScheduled() const;
    uint32_t deferredPersistNextAttemptAtMs() const;

    // Load settings from flash (public for testing)
    void load();
    // Load six physical-segment colours, migrating the v11 eight-value shape.
    void loadSignalBarColors();

    // WiFi client (STA) settings - connect to external network
    String getWifiClientPassword(); // Retrieves from secure NVS namespace
    String getWifiStaSlotPassword(size_t index);
    SettingsPersistResult setWifiClientEnabled(bool enabled);
    bool setWifiClientCredentials(const String& ssid, const String& password);
    bool setWifiStaSlotCredentials(size_t index, const String& ssid, const String& password, const String& label,
                                   uint8_t priority);
    bool markWifiStaSlotConnected(size_t index);
    bool clearWifiStaSlot(size_t index);
    bool clearWifiClientCredentials(); // Forget saved network
    SettingsPersistResult applyWifiStaPriorityUpdates(const std::vector<WifiStaPriorityUpdate>& updates);

    // SD card backup/restore for display settings
    bool backupToSD();
    void requestDeferredBackupFromCurrentState();
    void serviceDeferredBackup(uint32_t nowMs);
    bool deferredBackupPending() const;
    bool deferredBackupRetryScheduled() const;
    uint32_t deferredBackupNextAttemptAtMs() const;
    SettingsBackupApplyResult applyBackupDocument(const JsonDocument& doc, bool deferBackupRewrite,
                                                  const SettingsRestoreWatchdog& watchdog = SettingsRestoreWatchdog{},
                                                  SettingsBackupScope scope = SettingsBackupScope::Full);
    bool migrateAutoPushProfilesToV2();
    bool restoreFromSD();
    bool checkAndRestoreFromSD(); // Call after storage is mounted to retry restore
    // Before starting a new external mutation, converge every recoverable
    // storage transaction or fail closed while its rollback evidence remains.
    bool resolveStorageTransactionsForMutation();

    // NVS diagnostic info for troubleshooting persistence
    struct NvsDiagnostic {
        String activeNamespace;
        int nvsValidMarker = 0;
        int settingsVersion = 0;
        uint8_t nvsBrightness = 0;
        bool nvsProxyBle = false;
        bool nvsAutoPush = false;
        bool healthy = false;
    };
    NvsDiagnostic getNvsDiagnostic() const;

    // Validate profile references exist - clear invalid ones
    void validateProfileReferences(V1ProfileManager& profileMgr);

  private:
    StorageManager* storage_;
    V1ProfileManager* profiles_;
    V1Settings settings_;
    Preferences preferences_;
    uint32_t displayConfigurationRevision_ = 0;
    uint32_t backupRevisionCounter_ = 1;
    uint32_t backupDueRevision_ = 0;
    uint32_t backupCompletedRevision_ = 0;
    bool deferredPersistPending_ = false;
    bool deferredPersistRetryScheduled_ = false;
    uint32_t deferredPersistNextAttemptAtMs_ = 0;
    String pendingLastV1AddressFallback_;
    bool lastV1AddressFallbackPending_ = false;
    uint32_t lastV1AddressFallbackNextAttemptAtMs_ = 0;
    bool restorePending_ = false;
    uint64_t restoreCommitWatermark_ = 0;
    uint64_t profileDeleteCommitWatermark_ = 0;
#ifdef UNIT_TEST
    bool wifiCredentialInterruptBeforeSettingsCommit_ = false;
    bool restoreInterruptAfterCredentials_ = false;
    bool restoreInterruptAfterProfiles_ = false;
    bool autoPushMigrationInterruptAfterProfiles_ = false;
    bool profileDeleteInterruptAfterJournal_ = false;
    bool profileDeleteInterruptAfterProfile_ = false;
    bool profileDeleteInterruptAfterReferences_ = false;
    bool leaveRestoreJournalAfterCommit_ = false;
    bool leaveProfileDeleteJournalAfterCommit_ = false;
    bool restoreApplyStagingAllocationFailure_ = false;
#endif
    void recoverCriticalSettingsAfterFullRestoreFailure(fs::FS* fs, bool hasSdBackup,
                                                        const JsonDocument& backupDoc);
    void healWifiClientSettings(fs::FS* fs, bool hasSdBackup, const JsonDocument& backupDoc);
    void synchronizeSdBackup(bool hasSdBackup, const char* backupPath, const JsonDocument& backupDoc);
    void noteNvsCommitWithoutBackupIntent();
    bool persistSettingsWithBackupIntent();
    static uint32_t readBackupRevisionCompleted();
    static bool markBackupRevisionCompleted(uint32_t revision);
    static bool markDeferredBackupRevisionCompleted(uint32_t revision, void* context);
    void clearDeferredPersistState();
    SettingsPersistResult finishSettingsMutation(const V1Settings& before, bool changed,
                                                  SettingsPersistMode persistMode);
    void noteDisplayConfigurationMutation();
    void markRestorePending(const char* reason);
    void clearRestorePending();
    bool resolveWifiCredentialTransaction();
    bool resolveRestoreTransaction();
    bool resolveProfileDeleteTransaction();
    bool persistSettingsAtomically();
    bool writeSettingsToNamespace(const char* ns, uint32_t generation);
    bool persistLastV1AddressFallbackNow(const String& addr);
    void serviceLastV1AddressFallbackPersist(uint32_t nowMs);
    String getActiveNamespace(uint32_t* activeGeneration = nullptr);
    String getStagingNamespace(const String& activeNamespace);
    bool checkNeedsRestore(); // Returns true if NVS appears to be default/empty
    void cleanupNamespacesIfNeeded(bool hasSdBackup);
};

#endif // SETTINGS_H
