#include "auto_push_module.h"

#include "../quiet/quiet_coordinator_module.h"
#include "v1_profile_push_policy.h"

#include <cstdio>

namespace {

String jsonEscapeString(const String& value) {
    String escaped;
    escaped.reserve(value.length() + 8);

    for (size_t i = 0; i < value.length(); ++i) {
        const char c = value.charAt(i);
        switch (c) {
        case '"':
            escaped += "\\\"";
            break;
        case '\\':
            escaped += "\\\\";
            break;
        case '\b':
            escaped += "\\b";
            break;
        case '\f':
            escaped += "\\f";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char hex[7];
                snprintf(hex, sizeof(hex), "\\u%04X", static_cast<unsigned char>(c));
                escaped += hex;
            } else {
                escaped += c;
            }
            break;
        }
    }

    return escaped;
}

} // namespace

void AutoPushModule::begin(SettingsManager* settings, V1ProfileManager* profileMgr, V1BLEClient* ble, V1Display* disp,
                           QuietCoordinatorModule* quietCoordinator) {
    settings_ = settings;
    profiles_ = profileMgr;
    bleClient_ = ble;
    display_ = disp;
    quiet_ = quietCoordinator;
}

void AutoPushModule::armState(int slotIndex, const AutoPushSlot& slot, bool profileLoaded, const V1Profile& profile,
                              bool isPushNow, bool updateProfileIndicator) {
    state_ = State{};
    state_.slotIndex = slotIndex;
    state_.slot = slot;
    state_.profile = profileLoaded ? profile : V1Profile{};
    state_.profileLoaded = profileLoaded;
    state_.step = Step::WaitReady;
    state_.nextStepAtMs = static_cast<uint32_t>(millis()) + 100u;
    state_.isPushNow = isPushNow;
    state_.displayOn = !settings_->getSlotDarkMode(slotIndex);
    state_.muteToZero = settings_->getSlotMuteToZero(slotIndex);
    state_.volume = settings_->getSlotVolume(slotIndex);
    state_.muteVolume = settings_->getSlotMuteVolume(slotIndex);

    const uint32_t nextOperationId = status_.operationId + 1;
    status_ = OperationStatus{};
    status_.operationId = nextOperationId;
    status_.result = Result::QUEUED;
    status_.slotIndex = slotIndex;
    status_.profileName = slot.profileName;
    status_.profileRequested = slot.profileName.length() > 0;
    status_.profileLoaded = profileLoaded;
    status_.displayRequested = true;
    status_.modeRequested = slot.mode != V1_MODE_UNKNOWN;
    status_.volumeRequested = state_.volume != 0xFF || state_.muteVolume != 0xFF;

    if (display_ && updateProfileIndicator) {
        display_->drawProfileIndicator(slotIndex);
    }
}

AutoPushModule::QueueResult AutoPushModule::queuePreparedSlot(int slotIndex, const AutoPushSlot& slot,
                                                              bool profileLoaded, const V1Profile& profile,
                                                              bool isPushNow, bool activateSlot,
                                                              bool updateProfileIndicator) {
    if (!settings_ || !profiles_ || !bleClient_ || !display_) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }
    if (!bleClient_->isConnected()) {
        return QueueResult::V1_NOT_CONNECTED;
    }
    if (isActive()) {
        return QueueResult::ALREADY_IN_PROGRESS;
    }

    const uint8_t configuredVolume = settings_->getSlotVolume(slotIndex);
    const uint8_t configuredMuteVolume = settings_->getSlotMuteVolume(slotIndex);
    if ((configuredVolume == 0xFF) != (configuredMuteVolume == 0xFF)) {
        return QueueResult::INVALID_VOLUME_PAIR;
    }

    const int clampedIndex = std::max(0, std::min(2, slotIndex));
    if (activateSlot) {
        settings_->setActiveSlot(clampedIndex);
    }

    armState(clampedIndex, slot, profileLoaded, profile, isPushNow, updateProfileIndicator);
    return QueueResult::QUEUED;
}

AutoPushModule::QueueResult AutoPushModule::queueSlotPush(int slotIndex, bool activateSlot,
                                                          bool updateProfileIndicator) {
    if (!settings_) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }

    const int clampedIndex = std::max(0, std::min(2, slotIndex));
    const AutoPushSlot slot = settings_->getSlot(clampedIndex);
    return queuePreparedSlot(clampedIndex, slot, false, V1Profile{}, false, activateSlot, updateProfileIndicator);
}

AutoPushModule::QueueResult AutoPushModule::queuePushNow(const PushNowRequest& request) {
    if (!settings_ || !profiles_ || !bleClient_ || !display_) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }
    if (!bleClient_->isConnected()) {
        return QueueResult::V1_NOT_CONNECTED;
    }
    if (isActive()) {
        return QueueResult::ALREADY_IN_PROGRESS;
    }

    const int clampedIndex = std::max(0, std::min(2, request.slotIndex));
    AutoPushSlot slot = settings_->getSlot(clampedIndex);
    if (request.hasProfileOverride) {
        slot.profileName = request.profileName;
        slot.mode = request.hasModeOverride ? request.mode : V1_MODE_UNKNOWN;
    } else if (request.hasModeOverride) {
        slot.mode = request.mode;
    }

    if (slot.profileName.length() == 0) {
        return QueueResult::NO_PROFILE_CONFIGURED;
    }

    V1Profile profile;
    const ProfileOperationResult loaded = profiles_->loadProfileResult(slot.profileName, profile, 0);
    if (loaded.status == ProfileStorageStatus::Busy) {
        return QueueResult::PROFILE_BUSY;
    }
    if (!loaded.success()) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }

    return queuePreparedSlot(clampedIndex, slot, true, profile, true, request.activateSlot, true);
}

void AutoPushModule::applySlotMuteToZero(V1UserSettings& userSettings, bool slotMuteToZero) {
    if (slotMuteToZero) {
        userSettings.bytes[0] &= ~0x10;
    } else {
        userSettings.bytes[0] |= 0x10;
    }
}

void AutoPushModule::markFailure(FailureReason reason) {
    status_.anyFailed = true;
    if (status_.reason == FailureReason::NONE) {
        status_.reason = reason;
    }
}

void AutoPushModule::finishOperation() {
    const bool anyApplied =
        status_.profileApplied || status_.displayApplied || status_.modeApplied || status_.volumeApplied;
    if (!status_.anyFailed) {
        status_.result = Result::SUCCEEDED;
    } else if (anyApplied) {
        status_.result = Result::PARTIAL;
    } else {
        status_.result = Result::FAILED;
    }
    state_ = State{};
}

void AutoPushModule::scheduleNextAfterProfile(uint32_t nowMs) {
    state_.commandRetries = 0;
    state_.step = Step::Display;
    state_.nextStepAtMs = nowMs + 30;
}

void AutoPushModule::retryOrFailProfile(uint32_t nowMs, FailureReason reason) {
    if (bleClient_) {
        bleClient_->cancelUserBytesVerification();
    }
    if (state_.profileWriteRetries < kMaxProfileWriteRetries) {
        state_.profileWriteRetries++;
        state_.step = Step::Profile;
        state_.nextStepAtMs = nowMs + 30;
        return;
    }

    markFailure(reason);
    scheduleNextAfterProfile(nowMs);
}

void AutoPushModule::process() {
    if (state_.step == Step::Idle) {
        return;
    }

    if (!bleClient_ || !bleClient_->isConnected()) {
        if (bleClient_) {
            bleClient_->cancelUserBytesVerification();
        }
        markFailure(FailureReason::DISCONNECTED);
        finishOperation();
        return;
    }

    const uint32_t now = static_cast<uint32_t>(millis());
    if (!deadlineReached(now, state_.nextStepAtMs)) {
        return;
    }

    if (status_.result == Result::QUEUED) {
        status_.result = Result::IN_PROGRESS;
    }

    auto schedulePushNowRetry = [&]() {
        if (!state_.isPushNow) {
            return false;
        }
        if (state_.commandRetries < kMaxPushNowCommandRetries) {
            state_.commandRetries++;
            state_.nextStepAtMs = now + 30;
            return true;
        }
        return false;
    };

    switch (state_.step) {
    case Step::WaitReady:
        state_.step = Step::Profile;
        state_.nextStepAtMs = now;
        return;

    case Step::Profile: {
        const AutoPushSlot& slot = state_.slot;
        if (!state_.profileLoaded) {
            if (slot.profileName.length() > 0) {
                V1Profile profile;
                const ProfileOperationResult loaded = profiles_
                                                          ? profiles_->loadProfileResult(slot.profileName, profile, 0)
                                                          : ProfileOperationResult{};
                if (loaded.success()) {
                    state_.profile = profile;
                    state_.profileLoaded = true;
                    status_.profileLoaded = true;
                    state_.commandRetries = 0;
                } else if (loaded.status == ProfileStorageStatus::Busy && state_.commandRetries < 5) {
                    state_.commandRetries++;
                    state_.nextStepAtMs = now + 30;
                    return;
                } else {
                    markFailure(loaded.status == ProfileStorageStatus::Busy ? FailureReason::PROFILE_BUSY
                                                                           : FailureReason::PROFILE_LOAD_FAILED);
                }
            }
        }

        if (state_.profileLoaded) {
            V1UserSettings modifiedSettings = state_.profile.settings;
            applySlotMuteToZero(modifiedSettings, state_.muteToZero);
            V1ProfilePushPolicy::applyBeforePushToUserSettings(settings_->get(), modifiedSettings);

            if (bleClient_->writeUserBytes(modifiedSettings.bytes)) {
                bleClient_->startUserBytesVerification(modifiedSettings.bytes);
                state_.commandRetries = 0;
                state_.step = Step::ProfileReadback;
                state_.nextStepAtMs = now + 30;
                return;
            }
            retryOrFailProfile(now, FailureReason::PROFILE_WRITE_FAILED);
            return;
        }

        scheduleNextAfterProfile(now);
        return;
    }

    case Step::ProfileReadback:
        if (!bleClient_->requestUserBytes()) {
            retryOrFailProfile(now, FailureReason::PROFILE_READ_REQUEST_FAILED);
            return;
        }
        state_.commandRetries = 0;
        state_.step = Step::ProfileVerify;
        state_.verifyDeadlineMs = now + kProfileVerificationTimeoutMs;
        state_.nextStepAtMs = now;
        return;

    case Step::ProfileVerify: {
        const auto verifyStatus = bleClient_->userBytesVerificationStatus();
        if (verifyStatus == V1BLEClient::UserBytesVerificationStatus::MATCH) {
            status_.profileApplied = true;
            scheduleNextAfterProfile(now);
            return;
        }
        if (verifyStatus == V1BLEClient::UserBytesVerificationStatus::MISMATCH) {
            retryOrFailProfile(now, FailureReason::PROFILE_VERIFY_MISMATCH);
            return;
        }
        if (deadlineReached(now, state_.verifyDeadlineMs)) {
            retryOrFailProfile(now, FailureReason::PROFILE_VERIFY_TIMEOUT);
            return;
        }
        state_.nextStepAtMs = now + 10;
        return;
    }

    case Step::Display: {
        if (!bleClient_->setDisplayOn(state_.displayOn)) {
            if (schedulePushNowRetry()) {
                return;
            }
            markFailure(FailureReason::DISPLAY_FAILED);
        } else {
            status_.displayApplied = true;
        }
        state_.commandRetries = 0;
        state_.step = Step::Mode;
        state_.nextStepAtMs = now + (state_.slot.mode != V1_MODE_UNKNOWN ? 30 : 0);
        return;
    }

    case Step::Mode: {
        if (state_.slot.mode != V1_MODE_UNKNOWN) {
            if (!bleClient_->setMode(static_cast<uint8_t>(state_.slot.mode))) {
                if (schedulePushNowRetry()) {
                    return;
                }
                markFailure(FailureReason::MODE_FAILED);
            } else {
                status_.modeApplied = true;
            }
        }

        state_.commandRetries = 0;
        state_.step = Step::Volume;
        state_.nextStepAtMs = now + (status_.volumeRequested ? 30 : 0);
        return;
    }

    case Step::Volume: {
        if (status_.volumeRequested) {
            const bool volumePairValid = state_.volume <= 9 && state_.muteVolume <= 9;
            const bool volumeSent =
                volumePairValid && quiet_ && quiet_->sendAutoPushVolume(state_.volume, state_.muteVolume);
            if (!volumeSent) {
                if (schedulePushNowRetry()) {
                    return;
                }
                markFailure(FailureReason::VOLUME_FAILED);
            } else {
                status_.volumeApplied = true;
            }
        }
        finishOperation();
        return;
    }

    case Step::Idle:
    default:
        markFailure(FailureReason::PROFILE_WRITE_FAILED);
        finishOperation();
        return;
    }
}

String AutoPushModule::getStatusJson() const {
    const char* stepName = "Idle";
    switch (state_.step) {
    case Step::Idle:
        stepName = "Idle";
        break;
    case Step::WaitReady:
        stepName = "WaitReady";
        break;
    case Step::Profile:
        stepName = "Profile";
        break;
    case Step::ProfileReadback:
        stepName = "ProfileReadback";
        break;
    case Step::ProfileVerify:
        stepName = "ProfileVerify";
        break;
    case Step::Display:
        stepName = "Display";
        break;
    case Step::Mode:
        stepName = "Mode";
        break;
    case Step::Volume:
        stepName = "Volume";
        break;
    }

    const char* resultName = "none";
    switch (status_.result) {
    case Result::NONE:
        resultName = "none";
        break;
    case Result::QUEUED:
        resultName = "queued";
        break;
    case Result::IN_PROGRESS:
        resultName = "in_progress";
        break;
    case Result::SUCCEEDED:
        resultName = "succeeded";
        break;
    case Result::PARTIAL:
        resultName = "partial";
        break;
    case Result::FAILED:
        resultName = "failed";
        break;
    }

    const char* reasonName = "none";
    switch (status_.reason) {
    case FailureReason::NONE:
        break;
    case FailureReason::DISCONNECTED:
        reasonName = "disconnected";
        break;
    case FailureReason::PROFILE_BUSY:
        reasonName = "profile_busy";
        break;
    case FailureReason::PROFILE_LOAD_FAILED:
        reasonName = "profile_load_failed";
        break;
    case FailureReason::PROFILE_WRITE_FAILED:
        reasonName = "profile_write_failed";
        break;
    case FailureReason::PROFILE_READ_REQUEST_FAILED:
        reasonName = "profile_read_request_failed";
        break;
    case FailureReason::PROFILE_VERIFY_MISMATCH:
        reasonName = "profile_verify_mismatch";
        break;
    case FailureReason::PROFILE_VERIFY_TIMEOUT:
        reasonName = "profile_verify_timeout";
        break;
    case FailureReason::DISPLAY_FAILED:
        reasonName = "display_failed";
        break;
    case FailureReason::MODE_FAILED:
        reasonName = "mode_failed";
        break;
    case FailureReason::VOLUME_FAILED:
        reasonName = "volume_failed";
        break;
    }

    const bool hasProfile = status_.profileName.length() > 0;
    const String profileName = hasProfile ? jsonEscapeString(status_.profileName) : String("");

    String json;
    json.reserve(320 + profileName.length());
    json += "{\"active\":";
    json += state_.step == Step::Idle ? "false" : "true";
    json += ",\"operationId\":";
    json += String(status_.operationId);
    json += ",\"slot\":";
    json += String(status_.slotIndex);
    json += ",\"step\":\"";
    json += stepName;
    json += "\",\"result\":\"";
    json += resultName;
    json += "\",\"reason\":\"";
    json += reasonName;
    json += "\",\"profileLoaded\":";
    json += status_.profileLoaded ? "true" : "false";
    json += ",\"profileConfigured\":";
    json += hasProfile ? "true" : "false";
    json += ",\"profileName\":\"";
    json += profileName;
    json += "\",\"components\":{";
    json += "\"profile\":{\"requested\":";
    json += status_.profileRequested ? "true" : "false";
    json += ",\"applied\":";
    json += status_.profileApplied ? "true" : "false";
    json += "},\"display\":{\"requested\":";
    json += status_.displayRequested ? "true" : "false";
    json += ",\"applied\":";
    json += status_.displayApplied ? "true" : "false";
    json += "},\"mode\":{\"requested\":";
    json += status_.modeRequested ? "true" : "false";
    json += ",\"applied\":";
    json += status_.modeApplied ? "true" : "false";
    json += "},\"volume\":{\"requested\":";
    json += status_.volumeRequested ? "true" : "false";
    json += ",\"applied\":";
    json += status_.volumeApplied ? "true" : "false";
    json += "}}}";
    return json;
}
