#include "auto_push_module.h"

#include "../quiet/quiet_coordinator_module.h"
#include "perf_metrics.h"
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
    state_.nextStepAtMs = millis() + 100;
    state_.isPushNow = isPushNow;

    if (display_ && updateProfileIndicator) {
        display_->drawProfileIndicator(slotIndex);
    }
}

AutoPushModule::QueueResult AutoPushModule::queuePreparedSlot(int slotIndex, const AutoPushSlot& slot,
                                                              bool profileLoaded, const V1Profile& profile,
                                                              bool isPushNow, bool activateSlot,
                                                              bool countAutoPushStart, bool updateProfileIndicator) {
    if (!settings_ || !profiles_ || !bleClient_ || !display_) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }
    if (!bleClient_->isConnected()) {
        return QueueResult::V1_NOT_CONNECTED;
    }
    if (isActive()) {
        return QueueResult::ALREADY_IN_PROGRESS;
    }

    const int clampedIndex = std::max(0, std::min(2, slotIndex));
    if (activateSlot) {
        settings_->setActiveSlot(clampedIndex);
    }
    if (countAutoPushStart) {
        PERF_INC(autoPushStarts);
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
    return queuePreparedSlot(clampedIndex, slot, false, V1Profile{}, false, activateSlot, true, updateProfileIndicator);
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
    if (!profiles_->loadProfile(slot.profileName, profile)) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }

    return queuePreparedSlot(clampedIndex, slot, true, profile, true, request.activateSlot, false, true);
}

void AutoPushModule::applySlotMuteToZero(V1UserSettings& userSettings, bool slotMuteToZero) {
    if (slotMuteToZero) {
        userSettings.bytes[0] &= ~0x10;
    } else {
        userSettings.bytes[0] |= 0x10;
    }
}

void AutoPushModule::process() {
    if (state_.step == Step::Idle) {
        return;
    }

    if (!bleClient_ || !bleClient_->isConnected()) {
        if (!state_.isPushNow) {
            PERF_INC(autoPushDisconnectAbort);
        }
        state_ = State{};
        return;
    }

    const unsigned long now = millis();
    if (now < state_.nextStepAtMs) {
        return;
    }

    auto schedulePushNowRetry = [&]() -> bool {
        if (!state_.isPushNow) {
            return false;
        }
        if (state_.commandRetries < kMaxPushNowCommandRetries) {
            state_.commandRetries++;
            PERF_INC(pushNowRetries);
            state_.nextStepAtMs = now + 30;
            return true;
        }

        PERF_INC(pushNowFailures);
        state_ = State{};
        return true;
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
                if (profiles_ && profiles_->loadProfile(slot.profileName, profile)) {
                    state_.profile = profile;
                    state_.profileLoaded = true;
                } else {
                    if (!state_.isPushNow) {
                        PERF_INC(autoPushProfileLoadFail);
                    }
                }
            } else {
                if (!state_.isPushNow) {
                    PERF_INC(autoPushNoProfile);
                }
            }
        }

        if (state_.profileLoaded) {
            const bool slotMuteToZero = settings_->getSlotMuteToZero(state_.slotIndex);
            V1UserSettings modifiedSettings = state_.profile.settings;
            applySlotMuteToZero(modifiedSettings, slotMuteToZero);
            V1ProfilePushPolicy::applyBeforePushToUserSettings(settings_->get(), modifiedSettings);

            if (bleClient_->writeUserBytes(modifiedSettings.bytes)) {
                bleClient_->startUserBytesVerification(modifiedSettings.bytes);
                state_.profileWriteRetries = 0;
                state_.commandRetries = 0;
                state_.step = Step::ProfileReadback;
                state_.nextStepAtMs = now + 30;
                return;
            }

            if (!state_.isPushNow) {
                PERF_INC(autoPushBusyRetries);
            }
            if (schedulePushNowRetry()) {
                return;
            }

            if (state_.profileWriteRetries < kMaxProfileWriteRetries) {
                state_.profileWriteRetries++;
                state_.step = Step::Profile;
                state_.nextStepAtMs = now + 30;
                return;
            }

            PERF_INC(autoPushProfileWriteFail);
        }

        state_.commandRetries = 0;
        state_.step = Step::Display;
        state_.nextStepAtMs = now + 30;
        return;
    }

    case Step::ProfileReadback:
        bleClient_->requestUserBytes();
        state_.commandRetries = 0;
        state_.step = Step::Display;
        state_.nextStepAtMs = now + 30;
        return;

    case Step::Display: {
        const bool displayOn = !settings_->getSlotDarkMode(state_.slotIndex);
        if (!bleClient_->setDisplayOn(displayOn) && schedulePushNowRetry()) {
            return;
        }
        state_.commandRetries = 0;
        state_.step = Step::Mode;
        state_.nextStepAtMs = now + (state_.slot.mode != V1_MODE_UNKNOWN ? 30 : 0);
        return;
    }

    case Step::Mode: {
        if (state_.slot.mode != V1_MODE_UNKNOWN && !bleClient_->setMode(static_cast<uint8_t>(state_.slot.mode))) {
            if (schedulePushNowRetry()) {
                return;
            }
            if (!state_.isPushNow) {
                PERF_INC(autoPushModeFail);
            }
        }

        state_.commandRetries = 0;
        const bool volumeChangeNeeded = (settings_->getSlotVolume(state_.slotIndex) != 0xFF ||
                                         settings_->getSlotMuteVolume(state_.slotIndex) != 0xFF);
        state_.step = Step::Volume;
        state_.nextStepAtMs = now + (volumeChangeNeeded ? 30 : 0);
        return;
    }

    case Step::Volume: {
        const uint8_t mainVol = settings_->getSlotVolume(state_.slotIndex);
        const uint8_t muteVol = settings_->getSlotMuteVolume(state_.slotIndex);
        const bool volumeChangeNeeded = (mainVol != 0xFF || muteVol != 0xFF);
        const bool volumeSent =
            !volumeChangeNeeded || (quiet_ && quiet_->sendVolume(QuietOwner::AutoPush, mainVol, muteVol));
        if (!volumeSent) {
            if (schedulePushNowRetry()) {
                return;
            }
            if (!state_.isPushNow) {
                PERF_INC(autoPushVolumeFail);
            }
        }

        if (!state_.isPushNow) {
            PERF_INC(autoPushCompletes);
        }
        state_ = State{};
        return;
    }

    case Step::Idle:
    default:
        state_ = State{};
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

    const bool hasProfile = state_.slot.profileName.length() > 0;
    const String profileName = hasProfile ? jsonEscapeString(state_.slot.profileName) : String("");

    String json;
    json.reserve(128 + profileName.length());
    json += "{\"active\":";
    json += state_.step == Step::Idle ? "false" : "true";
    json += ",\"slot\":";
    json += String(state_.slotIndex);
    json += ",\"step\":\"";
    json += stepName;
    json += "\",\"profileLoaded\":";
    json += state_.profileLoaded ? "true" : "false";
    json += ",\"profileConfigured\":";
    json += hasProfile ? "true" : "false";
    json += ",\"profileName\":\"";
    json += profileName;
    json += "\"}";
    return json;
}
