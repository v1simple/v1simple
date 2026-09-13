// Verified V1 settings Apply executor.

#pragma once

#include <Arduino.h>
#include <algorithm>
#include <array>

#include "ble_client.h"
#include "display.h"
#include "packet_parser.h"
#include "settings.h"
#include "v1_devices.h"
#include "v1_profiles.h"

class QuietCoordinatorModule;

class AutoPushModule {
  public:
    enum class QueueResult : uint8_t {
        QUEUED = 0,
        V1_NOT_CONNECTED,
        ALREADY_IN_PROGRESS,
        NO_PROFILE_CONFIGURED,
        PROFILE_BUSY,
        PROFILE_LOAD_FAILED,
        INVALID_VOLUME_PAIR,
        UNSUPPORTED_CONFIGURATION,
    };

    struct PushNowRequest {
        int slotIndex = 0;
        bool activateSlot = false;
        bool hasProfileOverride = false;
        String profileName;
        bool hasModeOverride = false;
        V1Mode mode = V1_MODE_UNKNOWN;
    };

    void begin(SettingsManager* settings, V1ProfileManager* profileMgr, V1BLEClient* ble, PacketParser* parser,
               V1Display* disp, QuietCoordinatorModule* quietCoordinator);

    // Supplied by normal runtime immediately before Auto-Push is queued. This
    // is never loaded from durable storage and must match the live BLE session.
    void setPreApplySnapshot(const V1DetectorSnapshot& snapshot);

    QueueResult queueSlotPush(int slotIndex, bool activateSlot = false, bool updateProfileIndicator = true);
    QueueResult queuePushNow(const PushNowRequest& request);
    void process();
    String getStatusJson() const;
    bool isActive() const { return state_.step != Step::Idle; }

  private:
    static constexpr uint32_t kVerificationTimeoutMs = 1500;
    static constexpr uint32_t kPreApplySnapshotMaxAgeMs = 5000;

    enum class Step : uint8_t {
        Idle = 0,
        WaitReady,
        LoadProfile,
        Preflight,
        UserWrite,
        UserRead,
        UserVerify,
        DisplayWrite,
        DisplayVerify,
        ModeWrite,
        ModeVerify,
        VolumeWrite,
        VolumeRead,
        VolumeVerify,
    };

    enum class Result : uint8_t { NONE = 0, QUEUED, IN_PROGRESS, SUCCEEDED, PARTIAL, FAILED };
    enum class Outcome : uint8_t {
        NOT_REQUESTED = 0,
        PENDING,
        UNCHANGED,
        SENT,
        VERIFIED,
        UNSUPPORTED,
        INVALID,
        BLOCKED,
        WRITE_FAILED,
        READ_FAILED,
        MISMATCH,
        TIMEOUT,
        DISCONNECTED,
        SESSION_CHANGED,
        LOAD_FAILED,
    };
    enum class FailureReason : uint8_t {
        NONE = 0,
        DISCONNECTED,
        SESSION_CHANGED,
        MISSING_LIVE_SNAPSHOT,
        VERSION_UNKNOWN,
        UNSUPPORTED_FIRMWARE,
        PROFILE_BUSY,
        PROFILE_LOAD_FAILED,
        INVALID_PROFILE_SCHEMA,
        INVALID_USER_SETTING_VALUE,
        INVALID_POLICY,
        INVALID_VOLUME_PAIR,
        UNSUPPORTED_SAVED_VOLUME,
        UNSUPPORTED_CUSTOM_FREQUENCIES,
        UNSUPPORTED_BLUETOOTH_LED,
        CUSTOM_FREQUENCY_PRESERVATION_REQUIRED,
        EURO_ADVANCED_MODE_INVALID,
        VOLUME_OWNER_BUSY,
        USER_BYTES_BEFORE_REQUIRED,
        USER_BYTES_WRITE_FAILED,
        USER_BYTES_READ_FAILED,
        USER_BYTES_MISMATCH,
        USER_BYTES_TIMEOUT,
        DISPLAY_WRITE_FAILED,
        DISPLAY_MISMATCH,
        DISPLAY_TIMEOUT,
        MODE_WRITE_FAILED,
        MODE_MISMATCH,
        MODE_TIMEOUT,
        VOLUME_WRITE_FAILED,
        VOLUME_READ_FAILED,
        VOLUME_MISMATCH,
        VOLUME_TIMEOUT,
        PROXY_OWNS_DETECTOR,
    };

    struct ComponentStatus {
        bool requested = false;
        bool beforeAvailable = false;
        bool needed = false;
        bool sent = false;
        bool verified = false;
        Outcome outcome = Outcome::NOT_REQUESTED;
        FailureReason reason = FailureReason::NONE;
    };

    struct OperationStatus {
        uint32_t operationId = 0;
        Result result = Result::NONE;
        FailureReason reason = FailureReason::NONE;
        int slotIndex = 0;
        String profileName;
        bool profileLoaded = false;
        ComponentStatus userSettings;
        ComponentStatus display;
        ComponentStatus mode;
        ComponentStatus volume;
        std::array<uint8_t, 6> beforeUserBytes{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
        std::array<uint8_t, 6> desiredUserBytes{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
        std::array<uint8_t, 6> effectiveUserBytes{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
        std::array<uint8_t, 6> supportedUserMasks{{0, 0, 0, 0, 0, 0}};
        bool alpLaserOverrideActive = false;
        bool alpLaserOverrideApplied = false;
        bool beforeDisplayOn = true;
        bool desiredDisplayOn = true;
        uint8_t beforeMode = 0;
        uint8_t desiredMode = 0;
        uint8_t beforeMainVolume = 0;
        uint8_t beforeMutedVolume = 0;
        uint8_t desiredMainVolume = 0;
        uint8_t desiredMutedVolume = 0;
    };

    struct State {
        Step step = Step::Idle;
        uint32_t nextStepAtMs = 0;
        uint32_t verifyDeadlineMs = 0;
        uint32_t sessionGeneration = 0;
        uint32_t observationRevision = 0;
        uint32_t observationIngressBoundary = 0;
        bool sawFreshMismatch = false;
        bool volumeTransactionActive = false;
        int slotIndex = 0;
        AutoPushSlot slot;
        V1Profile profile;
        bool profileLoaded = false;
        bool profileOwned = false;
        bool isPushNow = false;
        bool updateProfileIndicator = true;
        V1DetectorSnapshot before;
        uint32_t firmwareVersion = 0;
        std::array<uint8_t, 6> effectiveUserBytes{{0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}};
        bool displayOn = true;
        uint8_t desiredMode = 0;
        V1VolumePolicy volumePolicy = V1VolumePolicy::Unchanged;
        uint8_t volume = 0xFF;
        uint8_t muteVolume = 0xFF;
    };

    QueueResult queuePreparedSlot(int slotIndex, const AutoPushSlot& slot, bool profileLoaded,
                                  const V1Profile& profile, bool isPushNow, bool activateSlot,
                                  bool updateProfileIndicator);
    void armState(int slotIndex, const AutoPushSlot& slot, bool profileLoaded, const V1Profile& profile,
                  bool isPushNow, bool updateProfileIndicator);
    bool configurePlan();
    bool preflight();
    void advanceAfterUser(uint32_t nowMs);
    void advanceAfterDisplay(uint32_t nowMs);
    void advanceAfterMode(uint32_t nowMs);
    void failComponent(ComponentStatus& component, Outcome outcome, FailureReason reason);
    void failWholePlan(ComponentStatus* component, Outcome outcome, FailureReason reason);
    void finishOperation();
    bool liveSessionMatches() const;
    static uint8_t modeValueFromObservation(char mode);
    static bool ingressAfter(uint32_t observed, uint32_t boundary) {
        return observed != 0 && static_cast<int32_t>(observed - boundary) > 0;
    }
    static bool deadlineReached(uint32_t nowMs, uint32_t deadlineMs) {
        return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
    }

    SettingsManager* settings_ = nullptr;
    V1ProfileManager* profiles_ = nullptr;
    V1BLEClient* bleClient_ = nullptr;
    PacketParser* parser_ = nullptr;
    V1Display* display_ = nullptr;
    QuietCoordinatorModule* quiet_ = nullptr;
    V1DetectorSnapshot preApplySnapshot_;
    State state_;
    OperationStatus status_;
};
