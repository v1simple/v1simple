#include "auto_push_module.h"

#include "../quiet/quiet_coordinator_module.h"
#include "v1_firmware_compat.h"
#include "v1_profile_push_policy.h"

#include <cstdio>
#include <cstring>

namespace {

String jsonEscapeString(const String& value) {
    String escaped;
    escaped.reserve(value.length() + 8);
    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value.charAt(i);
        switch (c) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char hex[7];
                snprintf(hex, sizeof(hex), "\\u%04X", static_cast<unsigned char>(c));
                escaped += hex;
            } else {
                escaped += c;
            }
        }
    }
    return escaped;
}

} // namespace

void AutoPushModule::begin(SettingsManager* settings, V1ProfileManager* profileMgr, V1BLEClient* ble,
                           PacketParser* parser, V1Display* disp, QuietCoordinatorModule* quietCoordinator) {
    settings_ = settings;
    profiles_ = profileMgr;
    bleClient_ = ble;
    parser_ = parser;
    display_ = disp;
    quiet_ = quietCoordinator;
}

void AutoPushModule::setPreApplySnapshot(const V1DetectorSnapshot& snapshot) {
    // A stable callback for a replacement BLE session can arrive before the
    // main loop lets the old operation observe its generation mismatch. End
    // the old transaction first, then preserve the new session's one-shot
    // capture for the queue call that immediately follows the callback.
    if (isActive() && snapshot.sessionGeneration != 0 &&
        snapshot.sessionGeneration != state_.sessionGeneration) {
        failWholePlan(nullptr, Outcome::SESSION_CHANGED, FailureReason::SESSION_CHANGED);
    }
    preApplySnapshot_ = snapshot;
}

void AutoPushModule::armState(int slotIndex, const AutoPushSlot& slot, bool profileLoaded,
                              const V1Profile& profile, bool isPushNow, bool updateProfileIndicator) {
    // This executor owns its observation revisions directly. Clear any legacy
    // component-level verifier/edge so a user-byte packet cannot release the
    // connection cycle before the complete operation is verified.
    if (bleClient_) bleClient_->cancelUserBytesVerification();
    state_ = State{};
    state_.slotIndex = slotIndex;
    state_.slot = slot;
    state_.profile = profileLoaded ? profile : V1Profile{};
    state_.profileLoaded = profileLoaded;
    state_.profileOwned = settings_->get().autoPushProfileSchemaVersion == V1_PROFILE_SCHEMA_VERSION;
    state_.isPushNow = isPushNow;
    state_.updateProfileIndicator = updateProfileIndicator;
    state_.before = preApplySnapshot_;
    // A connect-time observation can authorize only one operation. Later
    // gestures in the same long-lived session must acquire a fresh capture in
    // Phase 5 rather than silently reusing stale before-state.
    preApplySnapshot_ = V1DetectorSnapshot{};
    state_.sessionGeneration = bleClient_->sessionGeneration();
    state_.step = Step::WaitReady;
    state_.nextStepAtMs = static_cast<uint32_t>(millis()) + 100u;

    const uint32_t nextOperationId = status_.operationId + 1;
    status_ = OperationStatus{};
    status_.operationId = nextOperationId;
    status_.result = Result::QUEUED;
    status_.slotIndex = slotIndex;
    status_.profileName = slot.profileName;
    status_.profileLoaded = profileLoaded;

    if (display_ && updateProfileIndicator) display_->drawProfileIndicator(slotIndex);
}

AutoPushModule::QueueResult AutoPushModule::queuePreparedSlot(int slotIndex, const AutoPushSlot& slot,
                                                              bool profileLoaded, const V1Profile& profile,
                                                              bool isPushNow, bool activateSlot,
                                                              bool updateProfileIndicator) {
    if (!settings_ || !profiles_ || !bleClient_ || !parser_ || !display_ || !quiet_) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }
    if (!bleClient_->isConnected()) return QueueResult::V1_NOT_CONNECTED;
    if (isActive()) return QueueResult::ALREADY_IN_PROGRESS;

    const bool profileOwned = settings_->get().autoPushProfileSchemaVersion == V1_PROFILE_SCHEMA_VERSION;
    if (profileOwned && slot.profileName.length() == 0) return QueueResult::NO_PROFILE_CONFIGURED;

    const uint8_t configuredVolume = settings_->getSlotVolume(slotIndex);
    const uint8_t configuredMuteVolume = settings_->getSlotMuteVolume(slotIndex);
    if (!profileOwned && (configuredVolume == 0xFF) != (configuredMuteVolume == 0xFF)) {
        return QueueResult::INVALID_VOLUME_PAIR;
    }

    const int clampedIndex = std::max(0, std::min(2, slotIndex));
    if (activateSlot) settings_->setActiveSlot(clampedIndex);
    armState(clampedIndex, slot, profileLoaded, profile, isPushNow, updateProfileIndicator);
    return QueueResult::QUEUED;
}

AutoPushModule::QueueResult AutoPushModule::queueSlotPush(int slotIndex, bool activateSlot,
                                                          bool updateProfileIndicator) {
    if (!settings_) return QueueResult::PROFILE_LOAD_FAILED;
    const int clampedIndex = std::max(0, std::min(2, slotIndex));
    return queuePreparedSlot(clampedIndex, settings_->getSlot(clampedIndex), false, V1Profile{}, false,
                             activateSlot, updateProfileIndicator);
}

AutoPushModule::QueueResult AutoPushModule::queuePushNow(const PushNowRequest& request) {
    if (!settings_ || !profiles_ || !bleClient_ || !parser_ || !display_ || !quiet_) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }
    if (!bleClient_->isConnected()) return QueueResult::V1_NOT_CONNECTED;
    if (isActive()) return QueueResult::ALREADY_IN_PROGRESS;

    const int clampedIndex = std::max(0, std::min(2, request.slotIndex));
    AutoPushSlot slot = settings_->getSlot(clampedIndex);
    if (request.hasProfileOverride) {
        slot.profileName = request.profileName;
        // In legacy slot-owned configurations, selecting another profile
        // without an explicit mode must not inherit the current slot's mode.
        if (!request.hasModeOverride) slot.mode = V1_MODE_UNKNOWN;
    }
    if (request.hasModeOverride) slot.mode = request.mode;
    if (slot.profileName.length() == 0) return QueueResult::NO_PROFILE_CONFIGURED;

    V1Profile profile;
    const ProfileOperationResult loaded = profiles_->loadProfileResult(slot.profileName, profile, 0);
    if (loaded.status == ProfileStorageStatus::Busy) return QueueResult::PROFILE_BUSY;
    if (!loaded.success()) return QueueResult::PROFILE_LOAD_FAILED;
    return queuePreparedSlot(clampedIndex, slot, true, profile, true, request.activateSlot, true);
}

uint8_t AutoPushModule::modeValueFromObservation(char mode) {
    switch (mode) {
    case 'A':
    case 'C':
    case 'U': return 1;
    case 'l':
    case 'c':
    case 'u': return 2;
    case 'L': return 3;
    default: return 0;
    }
}

bool AutoPushModule::configurePlan() {
    status_.profileLoaded = state_.profileLoaded;
    if (state_.profileOwned) {
        if (!state_.profileLoaded) {
            failWholePlan(&status_.userSettings, Outcome::LOAD_FAILED, FailureReason::PROFILE_LOAD_FAILED);
            return false;
        }
        if (state_.profile.schemaVersion != V1_PROFILE_SCHEMA_VERSION) {
            failWholePlan(&status_.userSettings, Outcome::INVALID, FailureReason::INVALID_PROFILE_SCHEMA);
            return false;
        }

        const V1DetectorConfiguration& detector = state_.profile.detector;
        if (detector.userSettingsPolicy != V1UserSettingsPolicy::Unchanged &&
            detector.userSettingsPolicy != V1UserSettingsPolicy::Value) {
            failWholePlan(&status_.userSettings, Outcome::INVALID, FailureReason::INVALID_POLICY);
            return false;
        }
        if (detector.modePolicy != V1ModePolicy::Unchanged && detector.modePolicy != V1ModePolicy::Value) {
            failWholePlan(&status_.mode, Outcome::INVALID, FailureReason::INVALID_POLICY);
            return false;
        }
        if (detector.displayPolicy != V1DisplayPolicy::Unchanged && detector.displayPolicy != V1DisplayPolicy::On &&
            detector.displayPolicy != V1DisplayPolicy::Off) {
            failWholePlan(&status_.display, Outcome::INVALID, FailureReason::INVALID_POLICY);
            return false;
        }
        if (detector.volumePolicy != V1VolumePolicy::Unchanged &&
            detector.volumePolicy != V1VolumePolicy::Temporary && detector.volumePolicy != V1VolumePolicy::Saved) {
            failWholePlan(&status_.volume, Outcome::INVALID, FailureReason::INVALID_POLICY);
            return false;
        }
        if (detector.bluetoothLedPolicy != V1BluetoothLedPolicy::Unchanged) {
            failWholePlan(&status_.display, Outcome::UNSUPPORTED, FailureReason::UNSUPPORTED_BLUETOOTH_LED);
            return false;
        }
        if (detector.customFrequencyPolicy != V1CustomFrequencyPolicy::Unchanged) {
            failWholePlan(&status_.userSettings, Outcome::UNSUPPORTED,
                          FailureReason::UNSUPPORTED_CUSTOM_FREQUENCIES);
            return false;
        }

        status_.userSettings.requested = detector.userSettingsPolicy == V1UserSettingsPolicy::Value;
        status_.display.requested = detector.displayPolicy != V1DisplayPolicy::Unchanged;
        status_.mode.requested = detector.modePolicy == V1ModePolicy::Value;
        status_.volume.requested = detector.volumePolicy != V1VolumePolicy::Unchanged;
        state_.displayOn = detector.displayPolicy == V1DisplayPolicy::On;
        state_.desiredMode = detector.mode;
        state_.volumePolicy = detector.volumePolicy;
        state_.volume = detector.mainVolume;
        state_.muteVolume = detector.mutedVolume;
        std::memcpy(status_.desiredUserBytes.data(), state_.profile.settings.bytes,
                    status_.desiredUserBytes.size());
    } else {
        status_.userSettings.requested = state_.slot.profileName.length() > 0;
        status_.display.requested = true;
        status_.mode.requested = state_.slot.mode != V1_MODE_UNKNOWN;
        const uint8_t volume = settings_->getSlotVolume(state_.slotIndex);
        const uint8_t muted = settings_->getSlotMuteVolume(state_.slotIndex);
        status_.volume.requested = volume != 0xFF || muted != 0xFF;
        state_.displayOn = !settings_->getSlotDarkMode(state_.slotIndex);
        state_.desiredMode = static_cast<uint8_t>(state_.slot.mode);
        state_.volumePolicy = status_.volume.requested ? V1VolumePolicy::Temporary : V1VolumePolicy::Unchanged;
        state_.volume = volume;
        state_.muteVolume = muted;
        if (state_.profileLoaded) {
            std::memcpy(status_.desiredUserBytes.data(), state_.profile.settings.bytes,
                        status_.desiredUserBytes.size());
            if (settings_->getSlotMuteToZero(state_.slotIndex)) status_.desiredUserBytes[0] &= ~0x10;
            else status_.desiredUserBytes[0] |= 0x10;
        }
    }

    ComponentStatus* components[] = {&status_.userSettings, &status_.display, &status_.mode, &status_.volume};
    for (ComponentStatus* component : components) {
        component->outcome = component->requested ? Outcome::PENDING : Outcome::NOT_REQUESTED;
    }
    return true;
}

bool AutoPushModule::preflight() {
    if (!configurePlan()) return false;
    if (!state_.before.available || state_.before.sessionGeneration == 0) {
        failWholePlan(nullptr, Outcome::INVALID, FailureReason::MISSING_LIVE_SNAPSHOT);
        return false;
    }
    if (static_cast<uint32_t>(millis()) - state_.before.capturedUptimeMs > kPreApplySnapshotMaxAgeMs) {
        failWholePlan(nullptr, Outcome::INVALID, FailureReason::MISSING_LIVE_SNAPSHOT);
        return false;
    }
    if (state_.before.sessionGeneration != state_.sessionGeneration || !liveSessionMatches()) {
        failWholePlan(nullptr, Outcome::SESSION_CHANGED, FailureReason::SESSION_CHANGED);
        return false;
    }
    if (!state_.before.hasFirmwareVersion || state_.before.firmwareVersion == 0 ||
        !bleClient_->hasV1FirmwareVersion()) {
        failWholePlan(nullptr, Outcome::UNSUPPORTED, FailureReason::VERSION_UNKNOWN);
        return false;
    }
    state_.firmwareVersion = state_.before.firmwareVersion;
    if (bleClient_->v1FirmwareVersion() != state_.firmwareVersion ||
        !V1FirmwareCompat::capabilities(state_.firmwareVersion).gen2) {
        failWholePlan(nullptr, Outcome::UNSUPPORTED, FailureReason::UNSUPPORTED_FIRMWARE);
        return false;
    }
    if (bleClient_->isProxyClientConnected()) {
        failWholePlan(nullptr, Outcome::BLOCKED, FailureReason::PROXY_OWNS_DETECTOR);
        return false;
    }

    uint8_t liveUserBytes[6];
    const bool liveUserBytesAvailable = bleClient_->sessionUserBytesRevision() > 0 &&
                                        bleClient_->sessionUserBytesIngressSequence() > 0 &&
                                        bleClient_->copySessionUserBytes(liveUserBytes);
    const bool needsUserBaseline = status_.userSettings.requested ||
                                   (status_.mode.requested && state_.desiredMode == 3);
    if (needsUserBaseline &&
        (!state_.before.hasUserBytes || !liveUserBytesAvailable ||
         std::memcmp(liveUserBytes, state_.before.userBytes.data(), sizeof(liveUserBytes)) != 0)) {
        failWholePlan(status_.userSettings.requested ? &status_.userSettings : &status_.mode,
                      Outcome::INVALID, FailureReason::USER_BYTES_BEFORE_REQUIRED);
        return false;
    }

    if (status_.userSettings.requested) {
        if (!state_.profileLoaded) {
            failWholePlan(&status_.userSettings, Outcome::LOAD_FAILED, FailureReason::PROFILE_LOAD_FAILED);
            return false;
        }
        if (!V1FirmwareCompat::hasWritableUserSettings(state_.firmwareVersion)) {
            failWholePlan(&status_.userSettings, Outcome::UNSUPPORTED, FailureReason::UNSUPPORTED_FIRMWARE);
            return false;
        }
        status_.userSettings.beforeAvailable = true;
        status_.beforeUserBytes = state_.before.userBytes;
        std::array<uint8_t, 6> effectiveDesired = status_.desiredUserBytes;
        status_.alpLaserOverrideActive = V1ProfilePushPolicy::shouldDisableV1Laser(settings_->get());
        V1ProfilePushPolicy::applyBeforePush(settings_->get(), effectiveDesired.data());
        status_.alpLaserOverrideApplied = effectiveDesired[0] != status_.desiredUserBytes[0];
        V1FirmwareCompat::supportedUserByteMasks(state_.firmwareVersion, status_.supportedUserMasks.data());
        V1FirmwareCompat::overlaySupportedUserBytes(status_.beforeUserBytes.data(), effectiveDesired.data(),
                                                    state_.effectiveUserBytes.data(), state_.firmwareVersion);
        status_.effectiveUserBytes = state_.effectiveUserBytes;

        if (!V1FirmwareCompat::hasValidModeledUserSettingValues(state_.effectiveUserBytes.data(),
                                                                state_.firmwareVersion)) {
            failWholePlan(&status_.userSettings, Outcome::INVALID,
                          FailureReason::INVALID_USER_SETTING_VALUE);
            return false;
        }

        // The Euro/USA user bit resets Gen2 custom-frequency definitions.
        // Phase 3 has no definition snapshot/restore, so "unchanged" cannot be
        // honored if that bit would move.
        if (((status_.beforeUserBytes[1] ^ state_.effectiveUserBytes[1]) & 0x01) != 0) {
            failWholePlan(&status_.userSettings, Outcome::UNSUPPORTED,
                          FailureReason::CUSTOM_FREQUENCY_PRESERVATION_REQUIRED);
            return false;
        }
        status_.userSettings.needed = !V1FirmwareCompat::userBytesMatchSupported(
            status_.beforeUserBytes.data(), state_.effectiveUserBytes.data(), state_.firmwareVersion);
        if (!status_.userSettings.needed) {
            status_.userSettings.verified = true;
            status_.userSettings.outcome = Outcome::UNCHANGED;
        }
    }

    if (status_.display.requested) {
        const V1DisplayOnObservation& observed = parser_->displayOnObservation();
        if (!state_.before.hasDisplayOn || !observed.available || observed.revision == 0 ||
            observed.ingressSequence == 0 ||
            observed.value != state_.before.displayOn) {
            failWholePlan(&status_.display, Outcome::INVALID, FailureReason::MISSING_LIVE_SNAPSHOT);
            return false;
        }
        status_.display.beforeAvailable = true;
        status_.beforeDisplayOn = state_.before.displayOn;
        status_.desiredDisplayOn = state_.displayOn;
        status_.display.needed = status_.beforeDisplayOn != state_.displayOn;
        if (!status_.display.needed) {
            status_.display.verified = true;
            status_.display.outcome = Outcome::UNCHANGED;
        }
    }

    if (status_.mode.requested) {
        if (state_.desiredMode < 1 || state_.desiredMode > 3) {
            failWholePlan(&status_.mode, Outcome::INVALID, FailureReason::INVALID_POLICY);
            return false;
        }
        if (!V1FirmwareCompat::capabilities(state_.firmwareVersion).modeObservation) {
            failWholePlan(&status_.mode, Outcome::UNSUPPORTED, FailureReason::UNSUPPORTED_FIRMWARE);
            return false;
        }
        if (state_.desiredMode == 3) {
            if (!state_.before.hasUserBytes) {
                failWholePlan(&status_.mode, Outcome::INVALID, FailureReason::USER_BYTES_BEFORE_REQUIRED);
                return false;
            }
            const uint8_t effectiveRegionByte = status_.userSettings.requested
                                                    ? state_.effectiveUserBytes[1]
                                                    : state_.before.userBytes[1];
            if ((effectiveRegionByte & 0x01) == 0) {
                failWholePlan(&status_.mode, Outcome::INVALID, FailureReason::EURO_ADVANCED_MODE_INVALID);
                return false;
            }
        }
        const V1ModeObservation& observed = parser_->modeObservation();
        if (!state_.before.hasMode || !observed.available || observed.revision == 0 ||
            observed.ingressSequence == 0 ||
            observed.value != state_.before.mode || modeValueFromObservation(state_.before.mode) == 0) {
            failWholePlan(&status_.mode, Outcome::INVALID, FailureReason::MISSING_LIVE_SNAPSHOT);
            return false;
        }
        status_.mode.beforeAvailable = true;
        status_.beforeMode = modeValueFromObservation(state_.before.mode);
        status_.desiredMode = state_.desiredMode;
        status_.mode.needed = status_.beforeMode != state_.desiredMode;
        if (!status_.mode.needed) {
            status_.mode.verified = true;
            status_.mode.outcome = Outcome::UNCHANGED;
        }
    }

    if (status_.volume.requested) {
        if (state_.volumePolicy == V1VolumePolicy::Saved) {
            failWholePlan(&status_.volume, Outcome::UNSUPPORTED, FailureReason::UNSUPPORTED_SAVED_VOLUME);
            return false;
        }
        if (state_.volumePolicy != V1VolumePolicy::Temporary) {
            failWholePlan(&status_.volume, Outcome::INVALID, FailureReason::INVALID_POLICY);
            return false;
        }
        if (state_.volume > 9 || state_.muteVolume > 9) {
            failWholePlan(&status_.volume, Outcome::INVALID, FailureReason::INVALID_VOLUME_PAIR);
            return false;
        }
        // Before 4.1037 aux0=0 retains the older persistent behavior. Phase 3
        // promises temporary volume only, so those versions are unsupported.
        if (state_.firmwareVersion < V1FirmwareCompat::kSavedVolumeVersion) {
            failWholePlan(&status_.volume, Outcome::UNSUPPORTED, FailureReason::UNSUPPORTED_FIRMWARE);
            return false;
        }
        if (!quiet_->canApplyAutoPushVolumeExactly()) {
            failWholePlan(&status_.volume, Outcome::BLOCKED, FailureReason::VOLUME_OWNER_BUSY);
            return false;
        }
        uint8_t observedMain = 0;
        uint8_t observedMuted = 0;
        uint32_t observedIngressSequence = 0;
        if (!state_.before.hasCurrentVolume ||
            !parser_->copyLatestCanonicalCurrentVolume(observedMain, observedMuted, &observedIngressSequence) ||
            observedIngressSequence == 0 ||
            observedMain != state_.before.currentMainVolume || observedMuted != state_.before.currentMutedVolume) {
            failWholePlan(&status_.volume, Outcome::INVALID, FailureReason::MISSING_LIVE_SNAPSHOT);
            return false;
        }
        status_.volume.beforeAvailable = true;
        status_.beforeMainVolume = state_.before.currentMainVolume;
        status_.beforeMutedVolume = state_.before.currentMutedVolume;
        status_.desiredMainVolume = state_.volume;
        status_.desiredMutedVolume = state_.muteVolume;
        status_.volume.needed = status_.beforeMainVolume != state_.volume ||
                                status_.beforeMutedVolume != state_.muteVolume;
        if (!status_.volume.needed) {
            status_.volume.verified = true;
            status_.volume.outcome = Outcome::UNCHANGED;
        }
    }

    if (status_.userSettings.needed) state_.step = Step::UserWrite;
    else if (status_.display.needed) state_.step = Step::DisplayWrite;
    else if (status_.mode.needed) state_.step = Step::ModeWrite;
    else if (status_.volume.needed) state_.step = Step::VolumeWrite;
    else finishOperation();
    state_.nextStepAtMs = static_cast<uint32_t>(millis());
    return true;
}

bool AutoPushModule::liveSessionMatches() const {
    return bleClient_ && bleClient_->isConnected() && bleClient_->sessionGeneration() == state_.sessionGeneration;
}

void AutoPushModule::failComponent(ComponentStatus& component, Outcome outcome, FailureReason reason) {
    component.outcome = outcome;
    component.reason = reason;
    component.verified = false;
    if (status_.reason == FailureReason::NONE) status_.reason = reason;
}

void AutoPushModule::failWholePlan(ComponentStatus* failed, Outcome outcome, FailureReason reason) {
    if (failed) failComponent(*failed, outcome, reason);
    else if (status_.reason == FailureReason::NONE) status_.reason = reason;
    ComponentStatus* components[] = {&status_.userSettings, &status_.display, &status_.mode, &status_.volume};
    for (ComponentStatus* component : components) {
        if (component != failed && component->requested && !component->verified &&
            (component->outcome == Outcome::PENDING || component->outcome == Outcome::SENT)) {
            component->outcome = Outcome::BLOCKED;
            component->reason = reason;
        }
    }
    finishOperation();
}

void AutoPushModule::finishOperation() {
    if (quiet_ && state_.volumeTransactionActive) {
        quiet_->endAutoPushVolumeTransaction();
    }
    const ComponentStatus* components[] = {&status_.userSettings, &status_.display, &status_.mode, &status_.volume};
    bool allVerified = true;
    bool anyVerified = false;
    for (const ComponentStatus* component : components) {
        if (!component->requested) continue;
        allVerified = allVerified && component->verified;
        anyVerified = anyVerified || component->verified;
    }
    const bool candidateSuccess = status_.reason == FailureReason::NONE && allVerified;
    if (candidateSuccess && !liveSessionMatches()) {
        status_.reason = bleClient_ && bleClient_->isConnected() ? FailureReason::SESSION_CHANGED
                                                                 : FailureReason::DISCONNECTED;
    }
    if (candidateSuccess && status_.reason == FailureReason::NONE) status_.result = Result::SUCCEEDED;
    else status_.result = anyVerified ? Result::PARTIAL : Result::FAILED;
    if (bleClient_) {
        bleClient_->cancelUserBytesVerification();
        if (status_.result == Result::SUCCEEDED) {
            bleClient_->publishVerifiedSettingsApplyEdge(state_.sessionGeneration);
        }
    }
    preApplySnapshot_ = V1DetectorSnapshot{};
    state_ = State{};
}

void AutoPushModule::advanceAfterUser(uint32_t nowMs) {
    if (status_.display.needed) state_.step = Step::DisplayWrite;
    else if (status_.mode.needed) state_.step = Step::ModeWrite;
    else if (status_.volume.needed) state_.step = Step::VolumeWrite;
    else {
        finishOperation();
        return;
    }
    state_.nextStepAtMs = nowMs + 30;
}

void AutoPushModule::advanceAfterDisplay(uint32_t nowMs) {
    if (status_.mode.needed) state_.step = Step::ModeWrite;
    else if (status_.volume.needed) state_.step = Step::VolumeWrite;
    else {
        finishOperation();
        return;
    }
    state_.nextStepAtMs = nowMs + 30;
}

void AutoPushModule::advanceAfterMode(uint32_t nowMs) {
    if (status_.volume.needed) {
        state_.step = Step::VolumeWrite;
        state_.nextStepAtMs = nowMs + 30;
    } else {
        finishOperation();
    }
}

void AutoPushModule::process() {
    if (state_.step == Step::Idle) return;

    if (!bleClient_ || !bleClient_->isConnected()) {
        failWholePlan(nullptr, Outcome::DISCONNECTED, FailureReason::DISCONNECTED);
        return;
    }
    if (bleClient_->sessionGeneration() != state_.sessionGeneration) {
        failWholePlan(nullptr, Outcome::SESSION_CHANGED, FailureReason::SESSION_CHANGED);
        return;
    }

    const uint32_t now = static_cast<uint32_t>(millis());
    if (!deadlineReached(now, state_.nextStepAtMs)) return;
    if (status_.result == Result::QUEUED) status_.result = Result::IN_PROGRESS;

    switch (state_.step) {
    case Step::WaitReady:
        state_.step = state_.profileLoaded ? Step::Preflight : Step::LoadProfile;
        state_.nextStepAtMs = now;
        return;

    case Step::LoadProfile:
        if (state_.slot.profileName.length() > 0) {
            V1Profile profile;
            const ProfileOperationResult loaded = profiles_->loadProfileResult(state_.slot.profileName, profile, 0);
            if (!loaded.success()) {
                status_.userSettings.requested = true;
                status_.userSettings.outcome = Outcome::PENDING;
                failWholePlan(&status_.userSettings, Outcome::LOAD_FAILED,
                              loaded.status == ProfileStorageStatus::Busy ? FailureReason::PROFILE_BUSY
                                                                          : FailureReason::PROFILE_LOAD_FAILED);
                return;
            }
            state_.profile = profile;
            state_.profileLoaded = true;
            status_.profileLoaded = true;
        }
        state_.step = Step::Preflight;
        state_.nextStepAtMs = now;
        return;

    case Step::Preflight:
        (void)preflight();
        return;

    case Step::UserWrite:
        if (!bleClient_->writeUserBytesExact(state_.effectiveUserBytes.data())) {
            failWholePlan(&status_.userSettings, Outcome::WRITE_FAILED, FailureReason::USER_BYTES_WRITE_FAILED);
            return;
        }
        status_.userSettings.sent = true;
        status_.userSettings.outcome = Outcome::SENT;
        state_.step = Step::UserRead;
        state_.nextStepAtMs = now + 30;
        return;

    case Step::UserRead:
        // Sample immediately before the focused request. A delayed response
        // from any earlier request that arrived after the write is now part of
        // the baseline and cannot prove this operation.
        state_.observationRevision = bleClient_->sessionUserBytesRevision();
        if (!bleClient_->requestUserBytes()) {
            failWholePlan(&status_.userSettings, Outcome::READ_FAILED, FailureReason::USER_BYTES_READ_FAILED);
            return;
        }
        // Sample only after the request send succeeds. Any callback that
        // entered earlier (including one still queued) is part of the
        // baseline; a very fast response that races this sample is safely
        // excluded rather than falsely accepted.
        state_.observationIngressBoundary = bleClient_->latestV1NotificationIngressSequence();
        state_.step = Step::UserVerify;
        state_.verifyDeadlineMs = now + kVerificationTimeoutMs;
        state_.nextStepAtMs = now;
        return;

    case Step::UserVerify: {
        if (bleClient_->sessionUserBytesRevision() != state_.observationRevision &&
            ingressAfter(bleClient_->sessionUserBytesIngressSequence(), state_.observationIngressBoundary)) {
            uint8_t observed[6];
            if (bleClient_->copySessionUserBytes(observed) &&
                std::memcmp(observed, state_.effectiveUserBytes.data(), sizeof(observed)) == 0) {
                status_.userSettings.verified = true;
                status_.userSettings.outcome = Outcome::VERIFIED;
                advanceAfterUser(now);
            } else {
                failWholePlan(&status_.userSettings, Outcome::MISMATCH, FailureReason::USER_BYTES_MISMATCH);
            }
            return;
        }
        if (deadlineReached(now, state_.verifyDeadlineMs)) {
            failWholePlan(&status_.userSettings, Outcome::TIMEOUT, FailureReason::USER_BYTES_TIMEOUT);
            return;
        }
        state_.nextStepAtMs = now + 10;
        return;
    }

    case Step::DisplayWrite:
        state_.observationRevision = parser_->displayOnObservationRevision();
        state_.sawFreshMismatch = false;
        if (!bleClient_->setDisplayOn(state_.displayOn)) {
            failWholePlan(&status_.display, Outcome::WRITE_FAILED, FailureReason::DISPLAY_WRITE_FAILED);
            return;
        }
        state_.observationIngressBoundary = bleClient_->latestV1NotificationIngressSequence();
        status_.display.sent = true;
        status_.display.outcome = Outcome::SENT;
        state_.step = Step::DisplayVerify;
        state_.verifyDeadlineMs = now + kVerificationTimeoutMs;
        state_.nextStepAtMs = now;
        return;

    case Step::DisplayVerify: {
        const V1DisplayOnObservation& observed = parser_->displayOnObservation();
        if (observed.revision != state_.observationRevision &&
            ingressAfter(observed.ingressSequence, state_.observationIngressBoundary)) {
            if (observed.available && observed.value == state_.displayOn) {
                status_.display.verified = true;
                status_.display.outcome = Outcome::VERIFIED;
                advanceAfterDisplay(now);
                return;
            }
            state_.sawFreshMismatch = true;
            state_.observationRevision = observed.revision;
        }
        if (deadlineReached(now, state_.verifyDeadlineMs)) {
            failWholePlan(&status_.display, state_.sawFreshMismatch ? Outcome::MISMATCH : Outcome::TIMEOUT,
                          state_.sawFreshMismatch ? FailureReason::DISPLAY_MISMATCH : FailureReason::DISPLAY_TIMEOUT);
            return;
        }
        state_.nextStepAtMs = now + 10;
        return;
    }

    case Step::ModeWrite:
        state_.observationRevision = parser_->modeObservationRevision();
        state_.sawFreshMismatch = false;
        if (!bleClient_->setMode(state_.desiredMode)) {
            failWholePlan(&status_.mode, Outcome::WRITE_FAILED, FailureReason::MODE_WRITE_FAILED);
            return;
        }
        state_.observationIngressBoundary = bleClient_->latestV1NotificationIngressSequence();
        status_.mode.sent = true;
        status_.mode.outcome = Outcome::SENT;
        state_.step = Step::ModeVerify;
        state_.verifyDeadlineMs = now + kVerificationTimeoutMs;
        state_.nextStepAtMs = now;
        return;

    case Step::ModeVerify: {
        const V1ModeObservation& observed = parser_->modeObservation();
        if (observed.revision != state_.observationRevision &&
            ingressAfter(observed.ingressSequence, state_.observationIngressBoundary)) {
            const uint8_t observedMode = observed.available ? modeValueFromObservation(observed.value) : 0;
            if (observedMode == state_.desiredMode) {
                status_.mode.verified = true;
                status_.mode.outcome = Outcome::VERIFIED;
                advanceAfterMode(now);
                return;
            }
            state_.sawFreshMismatch = true;
            state_.observationRevision = observed.revision;
        }
        if (deadlineReached(now, state_.verifyDeadlineMs)) {
            failWholePlan(&status_.mode, state_.sawFreshMismatch ? Outcome::MISMATCH : Outcome::TIMEOUT,
                          state_.sawFreshMismatch ? FailureReason::MODE_MISMATCH : FailureReason::MODE_TIMEOUT);
            return;
        }
        state_.nextStepAtMs = now + 10;
        return;
    }

    case Step::VolumeWrite:
        if (!quiet_->beginAutoPushVolumeTransaction()) {
            failWholePlan(&status_.volume, Outcome::BLOCKED, FailureReason::VOLUME_OWNER_BUSY);
            return;
        }
        state_.volumeTransactionActive = true;
        if (!quiet_->sendAutoPushVolume(state_.volume, state_.muteVolume)) {
            failWholePlan(&status_.volume, Outcome::WRITE_FAILED, FailureReason::VOLUME_WRITE_FAILED);
            return;
        }
        status_.volume.sent = true;
        status_.volume.outcome = Outcome::SENT;
        state_.step = Step::VolumeRead;
        state_.nextStepAtMs = now + 30;
        return;

    case Step::VolumeRead:
        state_.observationRevision = parser_->currentVolumeObservationRevision();
        if (!bleClient_->requestCurrentVolume()) {
            failWholePlan(&status_.volume, Outcome::READ_FAILED, FailureReason::VOLUME_READ_FAILED);
            return;
        }
        state_.observationIngressBoundary = bleClient_->latestV1NotificationIngressSequence();
        state_.step = Step::VolumeVerify;
        state_.verifyDeadlineMs = now + kVerificationTimeoutMs;
        state_.nextStepAtMs = now;
        return;

    case Step::VolumeVerify:
        if (parser_->currentVolumeObservation().revision != state_.observationRevision &&
            ingressAfter(parser_->currentVolumeObservation().ingressSequence,
                         state_.observationIngressBoundary)) {
            const V1CurrentVolumeObservation& observed = parser_->currentVolumeObservation();
            if (observed.available && observed.main == state_.volume && observed.muted == state_.muteVolume) {
                status_.volume.verified = true;
                status_.volume.outcome = Outcome::VERIFIED;
                finishOperation();
            } else {
                failWholePlan(&status_.volume, Outcome::MISMATCH, FailureReason::VOLUME_MISMATCH);
            }
            return;
        }
        if (deadlineReached(now, state_.verifyDeadlineMs)) {
            failWholePlan(&status_.volume, Outcome::TIMEOUT, FailureReason::VOLUME_TIMEOUT);
            return;
        }
        state_.nextStepAtMs = now + 10;
        return;

    case Step::Idle:
    default:
        return;
    }
}

String AutoPushModule::getStatusJson() const {
    const auto stepName = [this]() {
        switch (state_.step) {
        case Step::Idle: return "Idle";
        case Step::WaitReady: return "WaitReady";
        case Step::LoadProfile: return "LoadProfile";
        case Step::Preflight: return "Preflight";
        case Step::UserWrite: return "UserWrite";
        case Step::UserRead: return "UserRead";
        case Step::UserVerify: return "UserVerify";
        case Step::DisplayWrite: return "DisplayWrite";
        case Step::DisplayVerify: return "DisplayVerify";
        case Step::ModeWrite: return "ModeWrite";
        case Step::ModeVerify: return "ModeVerify";
        case Step::VolumeWrite: return "VolumeWrite";
        case Step::VolumeRead: return "VolumeRead";
        case Step::VolumeVerify: return "VolumeVerify";
        }
        return "Idle";
    };
    const auto outcomeName = [](Outcome outcome) {
        switch (outcome) {
        case Outcome::NOT_REQUESTED: return "not_requested";
        case Outcome::PENDING: return "pending";
        case Outcome::UNCHANGED: return "unchanged";
        case Outcome::SENT: return "sent";
        case Outcome::VERIFIED: return "verified";
        case Outcome::UNSUPPORTED: return "unsupported";
        case Outcome::INVALID: return "invalid";
        case Outcome::BLOCKED: return "blocked";
        case Outcome::WRITE_FAILED: return "write_failed";
        case Outcome::READ_FAILED: return "read_failed";
        case Outcome::MISMATCH: return "mismatch";
        case Outcome::TIMEOUT: return "timeout";
        case Outcome::DISCONNECTED: return "disconnected";
        case Outcome::SESSION_CHANGED: return "session_changed";
        case Outcome::LOAD_FAILED: return "load_failed";
        }
        return "invalid";
    };
    const auto operationResultName = [](Result result) {
        switch (result) {
        case Result::NONE: return "none";
        case Result::QUEUED: return "queued";
        case Result::IN_PROGRESS: return "in_progress";
        case Result::SUCCEEDED: return "succeeded";
        case Result::PARTIAL: return "partial";
        case Result::FAILED: return "failed";
        }
        return "failed";
    };
    const auto reasonName = [](FailureReason reason) {
        switch (reason) {
        case FailureReason::NONE: return "none";
        case FailureReason::DISCONNECTED: return "disconnected";
        case FailureReason::SESSION_CHANGED: return "session_changed";
        case FailureReason::MISSING_LIVE_SNAPSHOT: return "missing_live_snapshot";
        case FailureReason::VERSION_UNKNOWN: return "version_unknown";
        case FailureReason::UNSUPPORTED_FIRMWARE: return "unsupported_firmware";
        case FailureReason::PROFILE_BUSY: return "profile_busy";
        case FailureReason::PROFILE_LOAD_FAILED: return "profile_load_failed";
        case FailureReason::INVALID_PROFILE_SCHEMA: return "invalid_profile_schema";
        case FailureReason::INVALID_USER_SETTING_VALUE: return "invalid_user_setting_value";
        case FailureReason::INVALID_POLICY: return "invalid_policy";
        case FailureReason::INVALID_VOLUME_PAIR: return "invalid_volume_pair";
        case FailureReason::UNSUPPORTED_SAVED_VOLUME: return "unsupported_saved_volume";
        case FailureReason::UNSUPPORTED_CUSTOM_FREQUENCIES: return "unsupported_custom_frequencies";
        case FailureReason::UNSUPPORTED_BLUETOOTH_LED: return "unsupported_bluetooth_led";
        case FailureReason::CUSTOM_FREQUENCY_PRESERVATION_REQUIRED:
            return "custom_frequency_preservation_required";
        case FailureReason::EURO_ADVANCED_MODE_INVALID: return "euro_advanced_mode_invalid";
        case FailureReason::VOLUME_OWNER_BUSY: return "volume_owner_busy";
        case FailureReason::USER_BYTES_BEFORE_REQUIRED: return "user_bytes_before_required";
        case FailureReason::USER_BYTES_WRITE_FAILED: return "user_bytes_write_failed";
        case FailureReason::USER_BYTES_READ_FAILED: return "user_bytes_read_failed";
        case FailureReason::USER_BYTES_MISMATCH: return "user_bytes_mismatch";
        case FailureReason::USER_BYTES_TIMEOUT: return "user_bytes_timeout";
        case FailureReason::DISPLAY_WRITE_FAILED: return "display_write_failed";
        case FailureReason::DISPLAY_MISMATCH: return "display_mismatch";
        case FailureReason::DISPLAY_TIMEOUT: return "display_timeout";
        case FailureReason::MODE_WRITE_FAILED: return "mode_write_failed";
        case FailureReason::MODE_MISMATCH: return "mode_mismatch";
        case FailureReason::MODE_TIMEOUT: return "mode_timeout";
        case FailureReason::VOLUME_WRITE_FAILED: return "volume_write_failed";
        case FailureReason::VOLUME_READ_FAILED: return "volume_read_failed";
        case FailureReason::VOLUME_MISMATCH: return "volume_mismatch";
        case FailureReason::VOLUME_TIMEOUT: return "volume_timeout";
        case FailureReason::PROXY_OWNS_DETECTOR: return "proxy_owns_detector";
        }
        return "invalid_policy";
    };
    const auto appendBool = [](String& json, bool value) { json += value ? "true" : "false"; };
    const auto appendBytes = [](String& json, const std::array<uint8_t, 6>& bytes) {
        json += '[';
        for (size_t index = 0; index < bytes.size(); ++index) {
            if (index) json += ',';
            json += String(bytes[index]);
        }
        json += ']';
    };
    const auto appendBase = [&](String& json, const ComponentStatus& component) {
        json += "\"requested\":";
        appendBool(json, component.requested);
        json += ",\"beforeAvailable\":";
        appendBool(json, component.beforeAvailable);
        json += ",\"needed\":";
        appendBool(json, component.needed);
        json += ",\"sent\":";
        appendBool(json, component.sent);
        json += ",\"verified\":";
        appendBool(json, component.verified);
        json += ",\"applied\":"; // backward-compatible alias; never means merely sent
        appendBool(json, component.verified);
        json += ",\"outcome\":\"";
        json += outcomeName(component.outcome);
        json += "\",\"reason\":\"";
        json += reasonName(component.reason);
        json += '"';
    };

    String json;
    json.reserve(1500 + status_.profileName.length());
    json += "{\"active\":";
    appendBool(json, state_.step != Step::Idle);
    json += ",\"operationId\":";
    json += String(status_.operationId);
    json += ",\"slot\":";
    json += String(status_.slotIndex);
    json += ",\"step\":\"";
    json += stepName();
    json += "\",\"result\":\"";
    json += operationResultName(status_.result);
    json += "\",\"reason\":\"";
    json += reasonName(status_.reason);
    json += "\",\"profileLoaded\":";
    appendBool(json, status_.profileLoaded);
    json += ",\"profileConfigured\":";
    appendBool(json, status_.profileName.length() > 0);
    json += ",\"profileName\":\"";
    json += jsonEscapeString(status_.profileName);
    json += "\",\"components\":{\"profile\":{";
    appendBase(json, status_.userSettings);
    json += ",\"before\":";
    if (status_.userSettings.beforeAvailable) appendBytes(json, status_.beforeUserBytes);
    else json += "null";
    json += ",\"desired\":";
    appendBytes(json, status_.desiredUserBytes);
    json += ",\"effective\":";
    appendBytes(json, status_.effectiveUserBytes);
    json += ",\"supportedMasks\":";
    appendBytes(json, status_.supportedUserMasks);
    json += ",\"alpLaserOverrideActive\":";
    appendBool(json, status_.alpLaserOverrideActive);
    json += ",\"alpLaserOverrideApplied\":";
    appendBool(json, status_.alpLaserOverrideApplied);
    json += "},\"display\":{";
    appendBase(json, status_.display);
    json += ",\"before\":";
    if (status_.display.beforeAvailable) appendBool(json, status_.beforeDisplayOn);
    else json += "null";
    json += ",\"desired\":";
    appendBool(json, status_.desiredDisplayOn);
    json += "},\"mode\":{";
    appendBase(json, status_.mode);
    json += ",\"before\":";
    if (status_.mode.beforeAvailable) json += String(status_.beforeMode);
    else json += "null";
    json += ",\"desired\":";
    json += String(status_.desiredMode);
    json += "},\"volume\":{";
    appendBase(json, status_.volume);
    json += ",\"before\":";
    if (status_.volume.beforeAvailable) {
        json += "{\"main\":";
        json += String(status_.beforeMainVolume);
        json += ",\"muted\":";
        json += String(status_.beforeMutedVolume);
        json += '}';
    } else json += "null";
    json += ",\"desired\":{\"main\":";
    json += String(status_.desiredMainVolume);
    json += ",\"muted\":";
    json += String(status_.desiredMutedVolume);
    json += "}}}}";
    return json;
}
