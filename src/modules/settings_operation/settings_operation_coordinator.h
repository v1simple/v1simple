#pragma once

#include "v1_settings_operation.h"

class AutoPushModule;
class V1BLEClient;
struct V1DetectorSnapshot;

// Owns transient detector-operation progress; the store owns durable admission
// and AutoPush owns command execution and verification.
class SettingsOperationCoordinator {
  public:
    enum class StableAdmission { AllowOrdinaryAutoPush, OwnedOrBlocked };
    enum class SnapshotPersistResult { Ok, StoreUnavailable, FlushFailed };
    struct Effects {
        void* context;
        bool (*readConnectedAddress)(String&, void*);
        V1DetectorSnapshot (*captureSnapshot)(uint32_t ingressBoundary, void*);
        SnapshotPersistResult (*persistSnapshot)(const String&, const V1DetectorSnapshot&,
                                                 bool flushImmediately, void*);
        int (*readActiveSlot)(void*);
        void (*presentQueuedProfile)(int slot, void*);
        bool (*requestMaintenanceRestart)(void*);
    };

    SettingsOperationCoordinator(V1SettingsOperationStore& store, AutoPushModule& autoPush,
                                 V1BLEClient& ble, Effects effects)
        : settingsOperations_(store), autoPush_(autoPush), ble_(ble), effects_(effects) {}

    void beginNormalBoot();
    void process(uint32_t nowMs);
    StableAdmission onStableConnection();
    bool allowsOrdinaryAutoPush() const;
    bool beginSlotApply(int slot);
    bool acknowledgePendingMaintenanceReturn();

  private:
    void serviceOperation(uint32_t nowMs);
    void queueSettingsApply(const V1SettingsOperationStore::Snapshot& operation,
                            const V1DetectorSnapshot& preApply);
    void attemptFactoryReset(const V1SettingsOperationStore::Snapshot& operation);
    void finishSettingsRecapture(const V1SettingsOperationStore::Snapshot& operation,
                                 uint32_t nowMs);

    V1SettingsOperationStore& settingsOperations_;
    AutoPushModule& autoPush_;
    V1BLEClient& ble_;
    const Effects effects_;

    bool settingsOperationBootRecoveryPending_ = false;
    uint32_t settingsOperationBootRecoveryAttemptMs_ = 0;
    uint32_t observedSettingsOperationId_ = 0;
    uint32_t settingsOperationStartedMs_ = 0;
    uint32_t settingsRecaptureIngressBoundary_ = 0;
    V1SettingsOperationPolicy::ReturnToMaintenanceRetry settingsReturnRetry_;
    uint32_t settingsOperationNextActionMs_ = 0;
    V1SettingsOperationStore::State observedSettingsOperationState_ =
        V1SettingsOperationStore::State::None;
    bool settingsRecaptureStarted_ = false;
    V1FreshObservationGate settingsFreshObservationGate_;
    V1DestructiveSendLatch factoryResetSendLatch_;
    bool factoryResetSummaryPersistedThisBoot_ = false;
    bool settingsWrongDetectorDisconnectPending_ = false;
    std::array<V1SettingsOperationStore::ComponentSummary,
               V1SettingsOperationStore::kComponentCount> factoryResetSentComponents_{};

};
