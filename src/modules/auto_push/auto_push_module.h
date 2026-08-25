// Auto-push profile module
// Encapsulates the V1 profile push state machine.

#pragma once

#include <Arduino.h>
#include <algorithm>

#include "settings.h"    // AutoPushSlot
#include "v1_profiles.h" // V1ProfileManager, V1Profile
#include "ble_client.h"  // V1BLEClient
#include "display.h"     // V1Display
class QuietCoordinatorModule;

class AutoPushModule {
  public:
    enum class QueueResult : uint8_t {
        QUEUED = 0,
        V1_NOT_CONNECTED,
        ALREADY_IN_PROGRESS,
        NO_PROFILE_CONFIGURED,
        PROFILE_LOAD_FAILED,
        INVALID_VOLUME_PAIR,
    };

    struct PushNowRequest {
        int slotIndex = 0;
        bool activateSlot = false;
        bool hasProfileOverride = false;
        String profileName;
        bool hasModeOverride = false;
        V1Mode mode = V1_MODE_UNKNOWN;
    };

    AutoPushModule() = default;

    void begin(SettingsManager* settings, V1ProfileManager* profileMgr, V1BLEClient* ble, V1Display* disp,
               QuietCoordinatorModule* quietCoordinator);

    // Queue a slot-driven auto-push through the shared executor.
    QueueResult queueSlotPush(int slotIndex, bool activateSlot = false, bool updateProfileIndicator = true);

    // Queue an explicit push-now request through the shared executor.
    QueueResult queuePushNow(const PushNowRequest& request);

    // Drive state machine; call from loop().
    void process();

    // Status for web API/debugging
    String getStatusJson() const;

    bool isActive() const { return state_.step != Step::Idle; }

  private:
    static constexpr uint8_t kMaxProfileWriteRetries = 5;
    static constexpr uint8_t kMaxPushNowCommandRetries = 8;
    static constexpr uint32_t kProfileVerificationTimeoutMs = 1500;

    enum class Step : uint8_t {
        Idle = 0,
        WaitReady,
        Profile,
        ProfileReadback,
        ProfileVerify,
        Display,
        Mode,
        Volume,
    };

    struct State {
        Step step = Step::Idle;
        unsigned long nextStepAtMs = 0;
        int slotIndex = 0;
        AutoPushSlot slot;
        V1Profile profile;
        bool profileLoaded = false;
        uint8_t profileWriteRetries = 0;
        uint8_t commandRetries = 0;
        bool isPushNow = false;
        bool displayOn = true;
        bool muteToZero = false;
        uint8_t volume = 0xFF;
        uint8_t muteVolume = 0xFF;
        uint32_t verifyDeadlineMs = 0;
    };

    enum class Result : uint8_t { NONE = 0, QUEUED, IN_PROGRESS, SUCCEEDED, PARTIAL, FAILED };
    enum class FailureReason : uint8_t {
        NONE = 0,
        DISCONNECTED,
        PROFILE_LOAD_FAILED,
        PROFILE_WRITE_FAILED,
        PROFILE_READ_REQUEST_FAILED,
        PROFILE_VERIFY_MISMATCH,
        PROFILE_VERIFY_TIMEOUT,
        DISPLAY_FAILED,
        MODE_FAILED,
        VOLUME_FAILED,
    };

    struct OperationStatus {
        uint32_t operationId = 0;
        Result result = Result::NONE;
        FailureReason reason = FailureReason::NONE;
        int slotIndex = 0;
        String profileName;
        bool profileRequested = false;
        bool profileLoaded = false;
        bool profileApplied = false;
        bool displayRequested = false;
        bool displayApplied = false;
        bool modeRequested = false;
        bool modeApplied = false;
        bool volumeRequested = false;
        bool volumeApplied = false;
        bool anyFailed = false;
    };

    void applySlotMuteToZero(V1UserSettings& settings, bool slotMuteToZero);
    QueueResult queuePreparedSlot(int slotIndex, const AutoPushSlot& slot, bool profileLoaded, const V1Profile& profile,
                                  bool isPushNow, bool activateSlot, bool updateProfileIndicator);
    void armState(int slotIndex, const AutoPushSlot& slot, bool profileLoaded, const V1Profile& profile, bool isPushNow,
                  bool updateProfileIndicator);
    void markFailure(FailureReason reason);
    void finishOperation();
    void scheduleNextAfterProfile(uint32_t nowMs);
    void retryOrFailProfile(uint32_t nowMs, FailureReason reason);

    SettingsManager* settings_ = nullptr;
    V1ProfileManager* profiles_ = nullptr;
    V1BLEClient* bleClient_ = nullptr;
    V1Display* display_ = nullptr;
    QuietCoordinatorModule* quiet_ = nullptr;

    State state_;
    OperationStatus status_;
};
