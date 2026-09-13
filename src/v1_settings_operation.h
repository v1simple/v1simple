#pragma once

#include <Arduino.h>

#include <array>
#include <cstdint>

// RAM-only one-shot guard for a destructive detector command. Once the BLE
// transport reports SENT, persistence retries may continue but the command
// itself cannot be emitted again during that boot.
class V1DestructiveSendLatch {
  public:
    void reset() { sent_ = false; }
    bool shouldSend() const { return !sent_; }
    bool sent() const { return sent_; }
    void noteSent() { sent_ = true; }

  private:
    bool sent_ = false;
};

// Durable hand-off for the one detector mutation that must cross the
// maintenance/normal-runtime boot boundary. This is intentionally not a
// generic job framework: it only represents a profile Apply or a V1 factory
// reset, bound to one canonical detector address.
class V1SettingsOperationStore {
  public:
    enum class Kind : uint8_t { None = 0, ApplySlot = 1, ApplyProfile = 2, FactoryReset = 3 };
    enum class Source : uint8_t { MaintenanceUi = 1, TripleTap = 2 };
    enum class State : uint8_t {
        None = 0,
        PendingNormalBoot,
        WaitingForDetector,
        Preparing,
        Running,
        Recapturing,
        Succeeded,
        Partial,
        Failed,
    };
    enum class Reason : uint8_t {
        None = 0,
        DetectorTimeout,
        WrongDetector,
        QueueRejected,
        DetectorDisconnected,
        ExecutorBusy,
        NoProfileConfigured,
        ProfileBusy,
        ProfileLoadFailed,
        InvalidConfiguration,
        UnsupportedConfiguration,
        ActiveSlotPersistFailed,
        StagingUnavailable,
        ApplyPartial,
        ApplyFailed,
        FactoryResetSendFailed,
        FactoryResetScopeUnverified,
        FactoryResetDefaultsMismatch,
        RecaptureStartFailed,
        RecaptureTimedOut,
        SnapshotStoreUnavailable,
        SnapshotPersistFailed,
        Interrupted,
        StorageUnavailable,
        InvalidRecord,
    };
    enum class LoadStatus : uint8_t { Ready = 0, Unavailable, Corrupt };
    enum class StartStatus : uint8_t { Started = 0, Active, Invalid, IdentityExhausted, StorageUnavailable };
    enum class Component : uint8_t {
        UserSettings = 0,
        Display,
        Mode,
        Volume,
        CustomFrequencies,
        FactoryReset,
        Count,
    };
    enum class ComponentOutcome : uint8_t {
        NotRequested = 0,
        Pending,
        Unchanged,
        Sent,
        Verified,
        Unsupported,
        Invalid,
        Blocked,
        WriteFailed,
        ReadFailed,
        Mismatch,
        Timeout,
        Disconnected,
        SessionChanged,
        LoadFailed,
    };
    enum class ComponentReason : uint8_t {
        None = 0,
        Disconnected,
        SessionChanged,
        MissingLiveSnapshot,
        VersionUnknown,
        UnsupportedFirmware,
        ProfileBusy,
        ProfileLoadFailed,
        InvalidProfileSchema,
        InvalidUserSettingValue,
        InvalidPolicy,
        InvalidVolumePair,
        UnsupportedSavedVolume,
        UnsupportedCustomFrequencies,
        UnsupportedBluetoothLed,
        CustomFrequencyPreservationRequired,
        EuroAdvancedModeInvalid,
        VolumeOwnerBusy,
        UserBytesBeforeRequired,
        UserBytesWriteFailed,
        UserBytesReadFailed,
        UserBytesMismatch,
        UserBytesTimeout,
        DisplayWriteFailed,
        DisplayMismatch,
        DisplayTimeout,
        ModeWriteFailed,
        ModeMismatch,
        ModeTimeout,
        VolumeWriteFailed,
        VolumeReadFailed,
        VolumeMismatch,
        VolumeTimeout,
        CustomConfigurationInvalid,
        CustomWriteFailed,
        CustomCommitRejected,
        CustomCommitTimeout,
        CustomReadFailed,
        CustomReadbackInvalid,
        CustomReadbackTimeout,
        FactoryDefaultUnverified,
        FactoryUserDefaultsMismatch,
        FactoryResetWriteFailed,
        ProxyOwnsDetector,
    };
    static constexpr size_t kComponentCount = static_cast<size_t>(Component::Count);

    struct ComponentSummary {
        bool requested = false;
        bool sent = false;
        bool verified = false;
        ComponentOutcome outcome = ComponentOutcome::NotRequested;
        ComponentReason reason = ComponentReason::None;
    };

    struct Snapshot {
        bool available = false;
        bool valid = true;
        uint32_t operationId = 0;
        uint32_t executorOperationId = 0;
        Kind kind = Kind::None;
        Source source = Source::MaintenanceUi;
        State state = State::None;
        Reason reason = Reason::None;
        int8_t slot = -1;
        bool returnToMaintenance = false;
        char targetAddress[18] = {0};
        char profileName[65] = {0};
        std::array<ComponentSummary, kComponentCount> components{};
    };

    struct StartResult {
        StartStatus status = StartStatus::StorageUnavailable;
        uint32_t operationId = 0;
    };

    LoadStatus begin();
    const Snapshot& snapshot() const { return snapshot_; }

    StartResult startApply(int slot, const char* canonicalAddress, Source source,
                           bool pendingNormalBoot, bool returnToMaintenance);
    StartResult startProfileApply(const char* profileName, const char* canonicalAddress,
                                  bool pendingNormalBoot, bool returnToMaintenance);
    StartResult startFactoryReset(const char* canonicalAddress, bool pendingNormalBoot,
                                  bool returnToMaintenance);

    // Normal boot recovery is fail-closed. A write may have occurred before an
    // interrupted Running/Recapturing state, so it resumes at recapture and can
    // never be promoted to success merely because the device restarted.
    bool beginNormalBoot();
    bool markPreparing();
    bool markRunning(uint32_t executorOperationId = 0);
    bool updateComponents(const std::array<ComponentSummary, kComponentCount>& components,
                          uint32_t executorOperationId);
    bool markRecapturing(Reason reason = Reason::None);
    bool finish(State terminalState, Reason reason);
    bool acknowledgeReturnToMaintenance();

    bool isActive() const;
    bool isTerminal() const;
    bool targetMatches(const char* canonicalAddress) const;

    static bool isCanonicalAddress(const char* address);
    static bool hasDurableFactoryResetSend(const Snapshot& snapshot);
    static bool factoryUserDefaultsMatch(const std::array<uint8_t, 6>& userBytes,
                                         bool hasUserBytes);
    static uint32_t requiredPreApplyIngressBoundary(State state, uint32_t recaptureBoundary);
    static bool nextOperationId(uint32_t current, uint32_t& next);
    static const char* kindName(Kind kind);
    static const char* sourceName(Source source);
    static const char* stateName(State state);
    static const char* reasonName(Reason reason);
    static const char* componentName(Component component);
    static const char* componentOutcomeName(ComponentOutcome outcome);
    static const char* componentReasonName(ComponentReason reason);

#ifdef UNIT_TEST
    static constexpr const char* namespaceForTest() { return "v1setjob"; }
    static constexpr const char* recordKeyForTest() { return "record"; }
#endif

  private:
    struct Record;

    StartResult start(Kind kind, int slot, const char* profileName,
                      const char* canonicalAddress, Source source,
                      bool pendingNormalBoot, bool returnToMaintenance);
    bool transition(State state, Reason reason, uint32_t executorOperationId);
    bool writeRecord(const Snapshot& snapshot);
    static uint32_t calculateCrc(const uint8_t* data, size_t length);
    static bool decodeRecord(const Record& record, Snapshot& snapshot);
    static Record encodeRecord(const Snapshot& snapshot);

    Snapshot snapshot_{};
    LoadStatus loadStatus_ = LoadStatus::Ready;
};
