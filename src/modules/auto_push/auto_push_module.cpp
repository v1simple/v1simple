#include "auto_push_module.h"

#include "../quiet/quiet_coordinator_module.h"
#include "v1_firmware_compat.h"
#include "v1_profile_push_policy.h"
#include "psram_json_document.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>

namespace {

constexpr uint8_t kCustomBandK = 0x01;
constexpr uint8_t kCustomBandKa = 0x02;

#ifdef UNIT_TEST
enum class AutoPushAdmissionFailurePoint : uint8_t {
    None = 0,
    OperationStorage,
    SlotProfile,
    ProfileName,
    ProfileDescription,
    StatusProfile,
    DefinitionCapacity,
};
AutoPushAdmissionFailurePoint g_autoPushAdmissionFailurePointForTest =
    AutoPushAdmissionFailurePoint::None;
#endif

bool copyAdmissionString(const String& source, String& destination
#ifdef UNIT_TEST
                         , AutoPushAdmissionFailurePoint point
#endif
) {
#ifdef UNIT_TEST
    if (g_autoPushAdmissionFailurePointForTest == point) {
        destination = String();
        return false;
    }
#endif
    destination = source;
    return destination.length() == source.length() && destination == source;
}

bool copyAdmissionProfile(const V1Profile& source, V1Profile& destination) {
    if (!copyAdmissionString(source.name, destination.name
#ifdef UNIT_TEST
                             , AutoPushAdmissionFailurePoint::ProfileName
#endif
                             ) ||
        !copyAdmissionString(source.description, destination.description
#ifdef UNIT_TEST
                             , AutoPushAdmissionFailurePoint::ProfileDescription
#endif
                             )) {
        return false;
    }
    destination.settings = source.settings;
    // Copy detector scalars individually. The operation state deliberately
    // keeps the profile's std::vector empty; definitions are staged directly
    // into its fixed-capacity list before durable slot activation.
    destination.detector.userSettingsPolicy = source.detector.userSettingsPolicy;
    destination.detector.modePolicy = source.detector.modePolicy;
    destination.detector.mode = source.detector.mode;
    destination.detector.displayPolicy = source.detector.displayPolicy;
    destination.detector.volumePolicy = source.detector.volumePolicy;
    destination.detector.mainVolume = source.detector.mainVolume;
    destination.detector.mutedVolume = source.detector.mutedVolume;
    destination.detector.volumeFeedback = source.detector.volumeFeedback;
    destination.detector.volumeDisconnect = source.detector.volumeDisconnect;
    destination.detector.bluetoothLedPolicy = source.detector.bluetoothLedPolicy;
    destination.detector.customFrequencyPolicy = source.detector.customFrequencyPolicy;
    destination.schemaVersion = source.schemaVersion;
    destination.displayOn = source.displayOn;
    destination.mainVolume = source.mainVolume;
    destination.mutedVolume = source.mutedVolume;
    return true;
}

int sweepDefinitionLiveSection(const V1CustomFrequencyDefinition& definition,
                               const V1DetectorSnapshot& snapshot) {
    int match = -1;
    for (uint8_t index = 0; index < snapshot.sweepSectionCount; ++index) {
        const auto& section = snapshot.sweepSections[index];
        if (section.lowerMHz == 0 && section.upperMHz == 0) continue;
        if (definition.lowerMHz >= section.lowerMHz && definition.upperMHz <= section.upperMHz) {
            if (match != -1) return -1; // overlapping authority is ambiguous
            match = index;
        }
    }
    return match;
}

uint8_t customBandForDefinition(const V1CustomFrequencyDefinition& definition,
                                const V1DetectorSnapshot& snapshot,
                                uint8_t lowestSection) {
    const int section = sweepDefinitionLiveSection(definition, snapshot);
    if (section < 0) return 0;
    return static_cast<uint8_t>(section) == lowestSection ? kCustomBandK : kCustomBandKa;
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

bool AutoPushModule::prepareState(int slotIndex, const AutoPushSlot& slot, bool profileLoaded,
                                  const V1Profile& profile, bool isPushNow,
                                  bool updateProfileIndicator, bool retainLoadStep,
                                  bool applySlotModifiers,
                                  State& preparedState,
                                  OperationStatus& preparedStatus) const {
    preparedState = State{};
    preparedState.slotIndex = slotIndex;
    preparedState.slot.mode = slot.mode;
    if (!copyAdmissionString(slot.profileName, preparedState.slot.profileName
#ifdef UNIT_TEST
                             , AutoPushAdmissionFailurePoint::SlotProfile
#endif
                             )) {
        return false;
    }
    if (profileLoaded && !copyAdmissionProfile(profile, preparedState.profile)) return false;
    if (profileLoaded &&
        (profile.detector.customFrequencyDefinitions.size() > FixedDefinitionList::kCapacity
#ifdef UNIT_TEST
         || g_autoPushAdmissionFailurePointForTest ==
                AutoPushAdmissionFailurePoint::DefinitionCapacity
#endif
         )) {
        return false;
    }
    if (profileLoaded &&
        !preparedState.customDefinitions.assign(profile.detector.customFrequencyDefinitions)) {
        return false;
    }
    preparedState.profileLoaded = profileLoaded;
    preparedState.applySlotModifiers = applySlotModifiers;
    preparedState.retainLoadStep = retainLoadStep;
    preparedState.profileOwned =
        settings_->get().autoPushProfileSchemaVersion == V1_PROFILE_SCHEMA_VERSION;
    preparedState.isPushNow = isPushNow;
    preparedState.updateProfileIndicator = updateProfileIndicator;
    preparedState.before = preApplySnapshot_;
    // A connect-time observation can authorize only one operation. Later
    // gestures in the same long-lived session must acquire a fresh capture in
    // Phase 5 rather than silently reusing stale before-state.
    preparedState.sessionGeneration = bleClient_->sessionGeneration();
    preparedState.step = Step::WaitReady;
    preparedState.nextStepAtMs = static_cast<uint32_t>(millis()) + 100u;

    const uint32_t nextOperationId = status_.operationId + 1;
    preparedStatus = OperationStatus{};
    preparedStatus.operationId = nextOperationId;
    preparedStatus.result = Result::QUEUED;
    preparedStatus.slotIndex = slotIndex;
    if (!copyAdmissionString(slot.profileName, preparedStatus.profileName
#ifdef UNIT_TEST
                             , AutoPushAdmissionFailurePoint::StatusProfile
#endif
                             )) {
        return false;
    }
    preparedStatus.profileLoaded = profileLoaded;
    return true;
}

void AutoPushModule::commitPreparedState(State&& preparedState,
                                         OperationStatus&& preparedStatus) {
    // This executor owns its observation revisions directly. Clear any legacy
    // component-level verifier/edge only once admission is fully staged and
    // any requested slot activation is durable.
    if (bleClient_) bleClient_->cancelUserBytesVerification();
    state_ = std::move(preparedState);
    status_ = std::move(preparedStatus);
    preApplySnapshot_ = V1DetectorSnapshot{};
    if (display_ && state_.updateProfileIndicator) {
        display_->drawProfileIndicator(state_.slotIndex);
    }
}

AutoPushModule::QueueResult AutoPushModule::queuePreparedSlot(int slotIndex, const AutoPushSlot& slot,
                                                              bool profileLoaded, const V1Profile& profile,
                                                              bool isPushNow, bool activateSlot,
                                                              bool updateProfileIndicator,
                                                              bool retainLoadStep,
                                                              bool applySlotModifiers) {
    if (!settings_ || !profiles_ || !bleClient_ || !parser_ || !display_ || !quiet_) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }
    if (!bleClient_->isConnected()) return QueueResult::V1_NOT_CONNECTED;
    if (isActive()) return QueueResult::ALREADY_IN_PROGRESS;

    const bool profileOwned = settings_->get().autoPushProfileSchemaVersion == V1_PROFILE_SCHEMA_VERSION;
    if (profileOwned && slot.profileName.length() == 0) return QueueResult::NO_PROFILE_CONFIGURED;

    const uint8_t configuredVolume = settings_->getSlotVolume(slotIndex);
    const uint8_t configuredMuteVolume = settings_->getSlotMuteVolume(slotIndex);
    if ((!profileOwned || (applySlotModifiers && settings_->getSlotVolumeOverride(slotIndex))) &&
        (configuredVolume == 0xFF) != (configuredMuteVolume == 0xFF)) {
        return QueueResult::INVALID_VOLUME_PAIR;
    }
    if (profileOwned && applySlotModifiers && settings_->getSlotVolumeOverride(slotIndex) &&
        (configuredVolume > 9 || configuredMuteVolume > 9)) return QueueResult::INVALID_VOLUME_PAIR;

    const int clampedIndex = std::max(0, std::min(2, slotIndex));
#ifdef UNIT_TEST
    if (g_autoPushAdmissionFailurePointForTest ==
        AutoPushAdmissionFailurePoint::OperationStorage) {
        return QueueResult::STAGING_UNAVAILABLE;
    }
#endif
    // Slot admission stays alive while atomic settings persistence commits.
    // Keeping both bounded objects on loopTask retained several kilobytes
    // before entering NVS and could trip the task's stack canary. Stage the
    // complete candidate off-stack and publish it only after persistence.
    std::unique_ptr<PreparedOperation> prepared(
        new (std::nothrow) PreparedOperation());
    if (!prepared) return QueueResult::STAGING_UNAVAILABLE;
    if (!prepareState(clampedIndex, slot, profileLoaded, profile, isPushNow,
                      updateProfileIndicator, retainLoadStep, applySlotModifiers, prepared->state,
                      prepared->status)) {
        return QueueResult::STAGING_UNAVAILABLE;
    }
    if (activateSlot && !settings_->setActiveSlot(clampedIndex).success) {
        return QueueResult::ACTIVE_SLOT_PERSIST_FAILED;
    }
    commitPreparedState(std::move(prepared->state), std::move(prepared->status));
    return QueueResult::QUEUED;
}

AutoPushModule::QueueResult AutoPushModule::queueSlotPush(int slotIndex, bool activateSlot,
                                                          bool updateProfileIndicator) {
    if (!settings_ || !profiles_ || !bleClient_ || !parser_ || !display_ || !quiet_) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }
    if (!bleClient_->isConnected()) return QueueResult::V1_NOT_CONNECTED;
    if (isActive()) return QueueResult::ALREADY_IN_PROGRESS;
    const int clampedIndex = std::max(0, std::min(2, slotIndex));
    const AutoPushSlot& slot = settings_->getSlot(clampedIndex);
    V1Profile profile;
    bool profileLoaded = false;
    if (slot.profileName.length() > 0) {
        const ProfileOperationResult loaded = profiles_->loadProfileResult(slot.profileName, profile, 0);
        if (loaded.status == ProfileStorageStatus::Busy) return QueueResult::PROFILE_BUSY;
        if (!loaded.success()) return QueueResult::PROFILE_LOAD_FAILED;
        profileLoaded = true;
    }
    return queuePreparedSlot(clampedIndex, slot, profileLoaded, profile, false,
                             activateSlot, updateProfileIndicator, true);
}

AutoPushModule::QueueResult AutoPushModule::queuePushNow(const PushNowRequest& request) {
    if (!settings_ || !profiles_ || !bleClient_ || !parser_ || !display_ || !quiet_) {
        return QueueResult::PROFILE_LOAD_FAILED;
    }
    if (!bleClient_->isConnected()) return QueueResult::V1_NOT_CONNECTED;
    if (isActive()) return QueueResult::ALREADY_IN_PROGRESS;

    const int clampedIndex = std::max(0, std::min(2, request.slotIndex));
    const AutoPushSlot& configuredSlot = settings_->getSlot(clampedIndex);
    AutoPushSlot slot;
    slot.mode = request.hasModeOverride
                    ? request.mode
                    : (request.hasProfileOverride ? V1_MODE_UNKNOWN : configuredSlot.mode);
    const String& requestedProfile = request.hasProfileOverride
                                         ? request.profileName
                                         : configuredSlot.profileName;
    if (!copyAdmissionString(requestedProfile, slot.profileName
#ifdef UNIT_TEST
                             , AutoPushAdmissionFailurePoint::SlotProfile
#endif
                             )) {
        return QueueResult::STAGING_UNAVAILABLE;
    }
    if (slot.profileName.length() == 0) return QueueResult::NO_PROFILE_CONFIGURED;

    V1Profile profile;
    const ProfileOperationResult loaded = profiles_->loadProfileResult(slot.profileName, profile, 0);
    if (loaded.status == ProfileStorageStatus::Busy) return QueueResult::PROFILE_BUSY;
    if (!loaded.success()) return QueueResult::PROFILE_LOAD_FAILED;
    return queuePreparedSlot(clampedIndex, slot, true, profile, true, request.activateSlot, true,
                             false, !request.hasProfileOverride);
}

V1SettingsOperationStore::Reason AutoPushModule::durableReasonForQueueResult(QueueResult result) {
    using Reason = V1SettingsOperationStore::Reason;
    switch (result) {
    case QueueResult::QUEUED: return Reason::None;
    case QueueResult::V1_NOT_CONNECTED: return Reason::DetectorDisconnected;
    case QueueResult::ALREADY_IN_PROGRESS: return Reason::ExecutorBusy;
    case QueueResult::NO_PROFILE_CONFIGURED: return Reason::NoProfileConfigured;
    case QueueResult::PROFILE_BUSY: return Reason::ProfileBusy;
    case QueueResult::PROFILE_LOAD_FAILED: return Reason::ProfileLoadFailed;
    case QueueResult::INVALID_VOLUME_PAIR: return Reason::InvalidConfiguration;
    case QueueResult::UNSUPPORTED_CONFIGURATION: return Reason::UnsupportedConfiguration;
    case QueueResult::ACTIVE_SLOT_PERSIST_FAILED: return Reason::ActiveSlotPersistFailed;
    case QueueResult::STAGING_UNAVAILABLE: return Reason::StagingUnavailable;
    }
    return Reason::QueueRejected;
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
        const bool slotVolumeOverride = state_.applySlotModifiers &&
            settings_->getSlotVolumeOverride(state_.slotIndex);
        const bool slotDarkModeOverride = state_.applySlotModifiers &&
            settings_->getSlotDarkModeOverride(state_.slotIndex);
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
        if (detector.volumeFeedback != V1VolumeFeedbackPolicy::None &&
            detector.volumeFeedback != V1VolumeFeedbackPolicy::ChangedOnly &&
            detector.volumeFeedback != V1VolumeFeedbackPolicy::Always) {
            failWholePlan(&status_.volume, Outcome::INVALID, FailureReason::INVALID_POLICY);
            return false;
        }
        if (detector.volumeDisconnect != V1VolumeDisconnectPolicy::RestoreSaved &&
            detector.volumeDisconnect != V1VolumeDisconnectPolicy::KeepCurrent) {
            failWholePlan(&status_.volume, Outcome::INVALID, FailureReason::INVALID_POLICY);
            return false;
        }
        if (detector.bluetoothLedPolicy != V1BluetoothLedPolicy::Unchanged &&
            detector.bluetoothLedPolicy != V1BluetoothLedPolicy::Off &&
            detector.bluetoothLedPolicy != V1BluetoothLedPolicy::On) {
            failWholePlan(&status_.display, Outcome::INVALID, FailureReason::INVALID_POLICY);
            return false;
        }
        if (detector.customFrequencyPolicy != V1CustomFrequencyPolicy::Unchanged &&
            detector.customFrequencyPolicy != V1CustomFrequencyPolicy::Value) {
            failWholePlan(&status_.customFrequencies, Outcome::INVALID, FailureReason::INVALID_POLICY);
            return false;
        }
        if (slotVolumeOverride && detector.volumePolicy == V1VolumePolicy::Unchanged) {
            failWholePlan(&status_.volume, Outcome::INVALID, FailureReason::INVALID_POLICY);
            return false;
        }

        status_.userSettings.requested = detector.userSettingsPolicy == V1UserSettingsPolicy::Value;
        status_.display.requested = slotDarkModeOverride ||
                                    detector.displayPolicy != V1DisplayPolicy::Unchanged ||
                                    detector.bluetoothLedPolicy != V1BluetoothLedPolicy::Unchanged;
        status_.mode.requested = detector.modePolicy == V1ModePolicy::Value;
        status_.volume.requested = detector.volumePolicy != V1VolumePolicy::Unchanged;
        status_.customFrequencies.requested =
            detector.customFrequencyPolicy == V1CustomFrequencyPolicy::Value;
        state_.displayOn = slotDarkModeOverride ? !settings_->getSlotDarkMode(state_.slotIndex) :
                           detector.displayPolicy == V1DisplayPolicy::Unchanged
                               ? state_.before.displayOn
                               : detector.displayPolicy == V1DisplayPolicy::On;
        state_.desiredMode = detector.mode;
        state_.volumePolicy = detector.volumePolicy;
        state_.bluetoothLedPolicy = slotDarkModeOverride && state_.displayOn
                                        ? V1BluetoothLedPolicy::Unchanged : detector.bluetoothLedPolicy;
        state_.customFrequencyPolicy = detector.customFrequencyPolicy;
        state_.volume = slotVolumeOverride ? settings_->getSlotVolume(state_.slotIndex) : detector.mainVolume;
        state_.muteVolume = slotVolumeOverride ? settings_->getSlotMuteVolume(state_.slotIndex) :
                                                 detector.mutedVolume;
        status_.volumePolicy = detector.volumePolicy;
        status_.volumeFeedback = detector.volumeFeedback;
        status_.volumeDisconnect = detector.volumeDisconnect;
        if (detector.volumeFeedback == V1VolumeFeedbackPolicy::Always) state_.volumeAux |= 0x01;
        else if (detector.volumeFeedback == V1VolumeFeedbackPolicy::ChangedOnly) state_.volumeAux |= 0x03;
        if (detector.volumePolicy == V1VolumePolicy::Saved) state_.volumeAux |= 0x04;
        if (detector.volumeDisconnect == V1VolumeDisconnectPolicy::KeepCurrent) state_.volumeAux |= 0x08;
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
        status_.volumePolicy = state_.volumePolicy;
        state_.volume = volume;
        state_.muteVolume = muted;
        if (state_.profileLoaded) {
            std::memcpy(status_.desiredUserBytes.data(), state_.profile.settings.bytes,
                        status_.desiredUserBytes.size());
            if (settings_->getSlotMuteToZero(state_.slotIndex)) status_.desiredUserBytes[0] &= ~0x10;
            else status_.desiredUserBytes[0] |= 0x10;
        }
    }

    ComponentStatus* components[] = {&status_.userSettings, &status_.display, &status_.mode, &status_.volume,
                                     &status_.customFrequencies};
    for (ComponentStatus* component : components) {
        component->outcome = component->requested ? Outcome::PENDING : Outcome::NOT_REQUESTED;
    }
    return true;
}

AutoPushModule::Step AutoPushModule::customEntryStep() const {
    return state_.customCompilationDeferred ? Step::CustomRefreshSections : Step::CustomWrite;
}

bool AutoPushModule::compileCustomDefinitions(const V1DetectorSnapshot& live) {
    const auto invalid = [this]() {
        failWholePlan(&status_.customFrequencies, Outcome::INVALID,
                      FailureReason::CUSTOM_CONFIGURATION_INVALID);
        return false;
    };
    if (!live.hasSweepSections || !live.hasMaxSweepIndex || !live.hasSweepDefinitions ||
        live.sweepSectionCount < 2 || live.sweepSectionCount > live.sweepSections.size() ||
        live.maxSweepIndex >= live.sweepDefinitions.size() ||
        state_.customDefinitions.size() > static_cast<size_t>(live.maxSweepIndex) + 1u) {
        return invalid();
    }

    uint8_t lowestSection = UINT8_MAX;
    uint8_t usableSectionCount = 0;
    for (uint8_t index = 0; index < live.sweepSectionCount; ++index) {
        const auto& section = live.sweepSections[index];
        const bool unused = section.lowerMHz == 0 && section.upperMHz == 0;
        if (section.index != index || section.count != live.sweepSectionCount ||
            (section.lowerMHz == 0) != (section.upperMHz == 0) ||
            (!unused && section.lowerMHz >= section.upperMHz)) {
            return invalid();
        }
        if (unused) continue;
        ++usableSectionCount;
        for (uint8_t prior = 0; prior < index; ++prior) {
            const auto& other = live.sweepSections[prior];
            if (other.lowerMHz == 0) continue;
            if (section.lowerMHz < other.upperMHz && section.upperMHz > other.lowerMHz) {
                return invalid();
            }
        }
        if (lowestSection == UINT8_MAX ||
            section.lowerMHz < live.sweepSections[lowestSection].lowerMHz) {
            lowestSection = index;
        }
    }
    if (lowestSection == UINT8_MAX || usableSectionCount < 2) return invalid();

    const FixedDefinitionList authored = state_.customDefinitions;
    if (!status_.requestedCustomDefinitions.assign(authored)) return invalid();
    state_.customPreservedMask = 0;
    uint8_t ownedBands = 0;
    for (size_t index = 0; index < authored.size(); ++index) {
        const auto& definition = authored[index];
        const bool unused = definition.lowerMHz == 0 && definition.upperMHz == 0;
        if (definition.index != index || (definition.lowerMHz == 0) != (definition.upperMHz == 0) ||
            (!unused && definition.lowerMHz >= definition.upperMHz)) {
            return invalid();
        }
        if (unused) continue;
        const uint8_t band = customBandForDefinition(definition, live, lowestSection);
        if (band == 0) return invalid();
        ownedBands = static_cast<uint8_t>(ownedBands | band);
    }

    std::array<V1CustomFrequencyDefinition, FixedDefinitionList::kCapacity> compiled{};
    std::array<bool, FixedDefinitionList::kCapacity> occupied{};
    for (uint8_t index = 0; index <= live.maxSweepIndex; ++index) {
        compiled[index].index = index;
        const auto& observed = live.sweepDefinitions[index];
        const bool unused = observed.lowerMHz == 0 && observed.upperMHz == 0;
        if (observed.index != index || (observed.lowerMHz == 0) != (observed.upperMHz == 0) ||
            (!unused && observed.lowerMHz >= observed.upperMHz)) {
            return invalid();
        }
        if (unused) continue;
        const V1CustomFrequencyDefinition liveDefinition{
            index, observed.lowerMHz, observed.upperMHz};
        const uint8_t band = customBandForDefinition(liveDefinition, live, lowestSection);
        if (band == 0) return invalid();
        if ((ownedBands & band) == 0) {
            compiled[index] = liveDefinition;
            occupied[index] = true;
            state_.customPreservedMask |= uint64_t{1} << index;
        }
    }

    for (const auto& definition : authored) {
        if (definition.lowerMHz == 0) continue;
        size_t target = definition.index;
        if (target > live.maxSweepIndex || occupied[target]) {
            target = 0;
            while (target <= live.maxSweepIndex && occupied[target]) ++target;
        }
        if (target > live.maxSweepIndex) return invalid();
        compiled[target] = V1CustomFrequencyDefinition{
            static_cast<uint8_t>(target), definition.lowerMHz, definition.upperMHz};
        occupied[target] = true;
    }

    FixedDefinitionList prepared;
    bool hasK = false;
    bool hasKa = false;
    bool definitionsDiffer = false;
    size_t lastUsed = 0;
    for (uint8_t index = 0; index <= live.maxSweepIndex; ++index) {
        const auto& definition = compiled[index];
        if (!prepared.push_back(definition)) return invalid();
        if (definition.lowerMHz != 0) {
            const uint8_t band = customBandForDefinition(definition, live, lowestSection);
            if (band == 0) return invalid();
            hasK |= band == kCustomBandK;
            hasKa |= band == kCustomBandKa;
            lastUsed = index;
        }
        const auto& before = live.sweepDefinitions[index];
        definitionsDiffer |= definition.index != before.index ||
                             definition.lowerMHz != before.lowerMHz ||
                             definition.upperMHz != before.upperMHz;
    }
    if (!hasK || !hasKa) return invalid();

    state_.customDefinitions = prepared;
    state_.customLastUsedIndex = lastUsed;
    state_.customRequiredMask = live.maxSweepIndex == 63
                                    ? UINT64_MAX
                                    : ((uint64_t{1} << (live.maxSweepIndex + 1u)) - 1u);
    status_.customFrequencies.beforeAvailable = true;
    status_.customFrequencies.needed = definitionsDiffer;
    if (!definitionsDiffer) {
        status_.customFrequencies.verified = true;
        status_.customFrequencies.outcome = Outcome::UNCHANGED;
        status_.effectiveCustomDefinitions.assign(prepared);
    } else {
        status_.customFrequencies.verified = false;
        status_.customFrequencies.outcome = Outcome::PENDING;
        status_.effectiveCustomDefinitions.clear();
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
                                   (status_.mode.requested && state_.desiredMode == 3) ||
                                   status_.customFrequencies.requested;
    if (needsUserBaseline &&
        (!state_.before.hasUserBytes || !liveUserBytesAvailable ||
         std::memcmp(liveUserBytes, state_.before.userBytes.data(), sizeof(liveUserBytes)) != 0)) {
        failWholePlan(status_.userSettings.requested
                          ? &status_.userSettings
                          : (status_.customFrequencies.requested ? &status_.customFrequencies : &status_.mode),
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
        state_.userWriteBytes = state_.effectiveUserBytes;
        state_.regionChanged =
            ((status_.beforeUserBytes[1] ^ state_.effectiveUserBytes[1]) & 0x01) != 0;

        if (!V1FirmwareCompat::hasValidModeledUserSettingValues(state_.effectiveUserBytes.data(),
                                                                state_.firmwareVersion)) {
            failWholePlan(&status_.userSettings, Outcome::INVALID,
                          FailureReason::INVALID_USER_SETTING_VALUE);
            return false;
        }

        // A profile that turns Custom Frequencies on, or changes K/Ka coverage
        // while it is on, must own the ranges the DUT will synchronize. A
        // migrated profile may still apply unrelated legacy user-byte changes
        // when the live detector already has the same custom/band state.
        const bool enablesCustomFrequencies = (state_.effectiveUserBytes[1] & 0x08) == 0;
        const bool changesCustomEnable =
            ((status_.beforeUserBytes[1] ^ state_.effectiveUserBytes[1]) & 0x08) != 0;
        const bool changesCustomBandCoverage =
            ((status_.beforeUserBytes[0] ^ state_.effectiveUserBytes[0]) & 0x06) != 0;
        if (enablesCustomFrequencies && !status_.customFrequencies.requested &&
            (changesCustomEnable || changesCustomBandCoverage || state_.regionChanged)) {
            failWholePlan(&status_.customFrequencies, Outcome::INVALID,
                          FailureReason::CUSTOM_CONFIGURATION_INVALID);
            return false;
        }

        // The Euro/USA user bit intentionally resets Gen2 definitions to the
        // target region's factory table. An unchanged definition policy can
        // accept that detector-defined result only when filtering will be off.
        // An explicit table is written later, after the region transition.
        if (state_.regionChanged) {
            const bool changingToEuro = (state_.effectiveUserBytes[1] & 0x01) == 0;
            if (changingToEuro) {
                const V1ModeObservation& observedMode = parser_->modeObservation();
                if (!state_.before.hasMode || !observedMode.available || observedMode.revision == 0 ||
                    observedMode.ingressSequence == 0 || observedMode.value != state_.before.mode) {
                    failWholePlan(&status_.mode, Outcome::INVALID, FailureReason::MISSING_LIVE_SNAPSHOT);
                    return false;
                }
                if (state_.before.mode == 'L' &&
                    (!status_.mode.requested || state_.desiredMode < 1 || state_.desiredMode > 2)) {
                    failWholePlan(&status_.mode, Outcome::INVALID,
                                  FailureReason::EURO_ADVANCED_MODE_INVALID);
                    return false;
                }
            }
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
        const V1BluetoothIndicatorObservation& bluetooth = parser_->bluetoothIndicatorObservation();
        if (!state_.before.hasDisplayOn || !observed.available || observed.revision == 0 ||
            observed.ingressSequence == 0 ||
            observed.value != state_.before.displayOn || !state_.before.hasBluetoothIndicator ||
            !bluetooth.available || bluetooth.revision == 0 || bluetooth.ingressSequence == 0 ||
            bluetooth.state != state_.before.bluetoothIndicator) {
            failWholePlan(&status_.display, Outcome::INVALID, FailureReason::MISSING_LIVE_SNAPSHOT);
            return false;
        }
        if (state_.displayOn && state_.bluetoothLedPolicy != V1BluetoothLedPolicy::Unchanged) {
            failWholePlan(&status_.display, Outcome::INVALID, FailureReason::INVALID_POLICY);
            return false;
        }
        status_.display.beforeAvailable = true;
        status_.beforeDisplayOn = state_.before.displayOn;
        status_.desiredDisplayOn = state_.displayOn;
        status_.beforeBluetoothIndicatorActive =
            state_.before.bluetoothIndicator != V1BluetoothIndicatorState::Off;
        state_.bluetoothIndicatorActive = state_.bluetoothLedPolicy == V1BluetoothLedPolicy::On
                                              ? true
                                              : (state_.bluetoothLedPolicy == V1BluetoothLedPolicy::Off
                                                     ? false
                                                     : status_.beforeBluetoothIndicatorActive);
        status_.desiredBluetoothIndicatorActive = state_.bluetoothIndicatorActive;
        if (!state_.displayOn && state_.bluetoothIndicatorActive &&
            !V1FirmwareCompat::capabilities(state_.firmwareVersion).keepBluetoothLedOn) {
            failWholePlan(&status_.display, Outcome::UNSUPPORTED, FailureReason::UNSUPPORTED_BLUETOOTH_LED);
            return false;
        }
        status_.display.needed = status_.beforeDisplayOn != state_.displayOn ||
                                 (!state_.displayOn && status_.beforeBluetoothIndicatorActive !=
                                                          state_.bluetoothIndicatorActive);
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
        if (state_.volumePolicy != V1VolumePolicy::Temporary && state_.volumePolicy != V1VolumePolicy::Saved) {
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
        if ((state_.volumeAux & 0x08) != 0 &&
            (state_.volumePolicy != V1VolumePolicy::Temporary ||
             state_.firmwareVersion < V1FirmwareCompat::kKeepCurrentVolumeOnDisconnectVersion)) {
            failWholePlan(&status_.volume, Outcome::UNSUPPORTED, FailureReason::UNSUPPORTED_FIRMWARE);
            return false;
        }
        if (!quiet_->canApplyAutoPushVolumeExactly()) {
            failWholePlan(&status_.volume, Outcome::BLOCKED, FailureReason::VOLUME_OWNER_BUSY);
            return false;
        }
        const V1AllVolumeObservation& allVolume = parser_->allVolumeObservation();
        if (!state_.before.hasCurrentVolume || !state_.before.hasSavedVolume || !allVolume.available ||
            allVolume.revision == 0 || allVolume.ingressSequence == 0 ||
            allVolume.currentMain != state_.before.currentMainVolume ||
            allVolume.currentMuted != state_.before.currentMutedVolume ||
            allVolume.savedMain != state_.before.savedMainVolume ||
            allVolume.savedMuted != state_.before.savedMutedVolume) {
            failWholePlan(&status_.volume, Outcome::INVALID, FailureReason::MISSING_LIVE_SNAPSHOT);
            return false;
        }
        status_.volume.beforeAvailable = true;
        status_.beforeMainVolume = state_.before.currentMainVolume;
        status_.beforeMutedVolume = state_.before.currentMutedVolume;
        status_.beforeSavedMainVolume = state_.before.savedMainVolume;
        status_.beforeSavedMutedVolume = state_.before.savedMutedVolume;
        status_.desiredMainVolume = state_.volume;
        status_.desiredMutedVolume = state_.muteVolume;
        status_.volumeCommandAux = state_.volumeAux;
        status_.volume.needed = status_.beforeMainVolume != state_.volume ||
                                status_.beforeMutedVolume != state_.muteVolume ||
                                (state_.volumePolicy == V1VolumePolicy::Saved &&
                                 (status_.beforeSavedMainVolume != state_.volume ||
                                  status_.beforeSavedMutedVolume != state_.muteVolume)) ||
                                // Temporary disconnect behavior is command-only
                                // state. Even restore_saved (aux b3=0) must be
                                // sent explicitly because numeric readback
                                // cannot disprove a prior keep-current command.
                                state_.volumePolicy == V1VolumePolicy::Temporary ||
                                (state_.volumeAux & 0x03) == 0x01;
        if (!status_.volume.needed) {
            status_.volume.verified = true;
            status_.volume.outcome = Outcome::UNCHANGED;
        }
    }

    if (status_.customFrequencies.requested) {
        const auto& liveSections = parser_->sweepSectionsObservation();
        const auto& liveMax = parser_->sweepMaxObservation();
        const auto& liveDefinitions = parser_->sweepDefinitionsObservation();
        const uint64_t liveRequiredMask = state_.before.maxSweepIndex == 63
                                              ? UINT64_MAX
                                              : ((uint64_t{1} << (state_.before.maxSweepIndex + 1u)) - 1u);
        if (!V1FirmwareCompat::capabilities(state_.firmwareVersion).customSweeps ||
            !state_.before.hasSweepSections || !state_.before.hasMaxSweepIndex ||
            !state_.before.hasSweepDefinitions || state_.before.sweepSectionCount == 0 ||
            !liveSections.available || !liveSections.complete || liveSections.poisoned ||
            liveSections.ingressSequence == 0 || liveSections.count != state_.before.sweepSectionCount ||
            !liveMax.available || liveMax.poisoned || liveMax.ingressSequence == 0 ||
            liveMax.maxIndex != state_.before.maxSweepIndex || liveDefinitions.poisoned ||
            liveDefinitions.ingressSequence == 0 || liveDefinitions.presentMask != liveRequiredMask) {
            failWholePlan(&status_.customFrequencies, Outcome::UNSUPPORTED,
                          FailureReason::MISSING_LIVE_SNAPSHOT);
            return false;
        }
        for (uint8_t index = 0; index < state_.before.sweepSectionCount; ++index) {
            if (liveSections.sections[index].index != state_.before.sweepSections[index].index ||
                liveSections.sections[index].count != state_.before.sweepSections[index].count ||
                liveSections.sections[index].lowerMHz != state_.before.sweepSections[index].lowerMHz ||
                liveSections.sections[index].upperMHz != state_.before.sweepSections[index].upperMHz) {
                failWholePlan(&status_.customFrequencies, Outcome::INVALID,
                              FailureReason::MISSING_LIVE_SNAPSHOT);
                return false;
            }
        }
        for (uint8_t index = 0; index <= state_.before.maxSweepIndex; ++index) {
            if (liveDefinitions.definitions[index].index != state_.before.sweepDefinitions[index].index ||
                liveDefinitions.definitions[index].lowerMHz != state_.before.sweepDefinitions[index].lowerMHz ||
                liveDefinitions.definitions[index].upperMHz != state_.before.sweepDefinitions[index].upperMHz) {
                failWholePlan(&status_.customFrequencies, Outcome::INVALID,
                              FailureReason::MISSING_LIVE_SNAPSHOT);
                return false;
            }
        }
        if (state_.regionChanged) {
            // USA and Euro have different factory tables, and the region write
            // resets the live table. Compile only from a fresh target-region
            // capture so an unowned band is never restored from the old mode.
            state_.customCompilationDeferred = true;
            status_.customFrequencies.beforeAvailable = true;
            status_.customFrequencies.needed = true;
            if (!status_.requestedCustomDefinitions.assign(state_.customDefinitions)) {
                failWholePlan(&status_.customFrequencies, Outcome::INVALID,
                              FailureReason::CUSTOM_CONFIGURATION_INVALID);
                return false;
            }
        } else if (!compileCustomDefinitions(state_.before)) {
            return false;
        }
    }

    if (status_.userSettings.requested) {
        const bool beforeCustomEnabled = (status_.beforeUserBytes[1] & 0x08) == 0;
        const bool finalCustomEnabled = (state_.effectiveUserBytes[1] & 0x08) == 0;
        const bool enablingCustom = !beforeCustomEnabled && finalCustomEnabled;
        if (finalCustomEnabled && status_.customFrequencies.requested &&
            (enablingCustom || state_.regionChanged)) {
            // A complete table must be committed and read back before the final
            // enable bit is exposed. Region changes must happen first because
            // the V1 resets the table, so make that transition with filtering
            // disabled and apply the final enabled bytes only after readback.
            status_.customFrequencies.needed = true;
            status_.customFrequencies.verified = false;
            status_.customFrequencies.outcome = Outcome::PENDING;
            status_.effectiveCustomDefinitions.clear();
            state_.finalUserWriteAfterCustom = true;
            if (state_.regionChanged) {
                state_.intermediateCustomDisabledWrite = true;
                state_.userWriteBytes[1] |= 0x08;
            }
        }
    }

    if (state_.intermediateCustomDisabledWrite) state_.step = Step::UserWrite;
    else if (state_.finalUserWriteAfterCustom) state_.step = customEntryStep();
    else if (status_.userSettings.needed) state_.step = Step::UserWrite;
    else if (status_.display.needed) state_.step = Step::DisplayWrite;
    else if (status_.mode.needed) state_.step = Step::ModeWrite;
    else if (status_.volume.needed) state_.step = Step::VolumeWrite;
    else if (status_.customFrequencies.needed && !status_.customFrequencies.verified)
        state_.step = customEntryStep();
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
    ComponentStatus* components[] = {&status_.userSettings, &status_.display, &status_.mode, &status_.volume,
                                     &status_.customFrequencies};
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
    const ComponentStatus* components[] = {&status_.userSettings, &status_.display, &status_.mode, &status_.volume,
                                           &status_.customFrequencies};
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
    else if (status_.customFrequencies.requested && !status_.customFrequencies.verified)
        status_.result = Result::FAILED;
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
    else if (status_.customFrequencies.needed && !status_.customFrequencies.verified)
        state_.step = customEntryStep();
    else {
        finishOperation();
        return;
    }
    state_.nextStepAtMs = nowMs + 30;
}

void AutoPushModule::advanceAfterDisplay(uint32_t nowMs) {
    if (status_.mode.needed) state_.step = Step::ModeWrite;
    else if (status_.volume.needed) state_.step = Step::VolumeWrite;
    else if (status_.customFrequencies.needed && !status_.customFrequencies.verified)
        state_.step = customEntryStep();
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
    } else if (status_.customFrequencies.needed && !status_.customFrequencies.verified) {
        state_.step = customEntryStep();
        state_.nextStepAtMs = nowMs + 30;
    } else {
        finishOperation();
    }
}

void AutoPushModule::advanceAfterVolume(uint32_t nowMs) {
    if (status_.customFrequencies.needed && !status_.customFrequencies.verified) {
        state_.step = customEntryStep();
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
        state_.step = state_.retainLoadStep ? Step::LoadProfile : Step::Preflight;
        state_.nextStepAtMs = now;
        return;

    case Step::LoadProfile:
        // queueSlotPush retains this step for timing compatibility, but every
        // fallible profile load/copy completed before queue admission and any
        // requested active-slot persistence.
        state_.retainLoadStep = false;
        state_.step = Step::Preflight;
        state_.nextStepAtMs = now;
        return;

    case Step::Preflight:
        (void)preflight();
        return;

    case Step::UserWrite:
        if (!bleClient_->writeUserBytesExact(state_.userWriteBytes.data())) {
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
        // The callback may run while the synchronous send waits. Preserve its
        // ingress as fresh even when queue parsing follows the send return.
        state_.observationIngressBoundary = bleClient_->latestV1NotificationIngressSequence();
        if (!bleClient_->requestUserBytes()) {
            failWholePlan(&status_.userSettings, Outcome::READ_FAILED, FailureReason::USER_BYTES_READ_FAILED);
            return;
        }
        state_.step = Step::UserVerify;
        state_.verifyDeadlineMs = now + kVerificationTimeoutMs;
        state_.nextStepAtMs = now;
        return;

    case Step::UserVerify: {
        if (bleClient_->sessionUserBytesRevision() != state_.observationRevision &&
            ingressAfter(bleClient_->sessionUserBytesIngressSequence(), state_.observationIngressBoundary)) {
            uint8_t observed[6];
            if (bleClient_->copySessionUserBytes(observed) &&
                std::memcmp(observed, state_.userWriteBytes.data(), sizeof(observed)) == 0) {
                if (state_.intermediateCustomDisabledWrite) {
                    state_.intermediateCustomDisabledWrite = false;
                    state_.step = customEntryStep();
                    state_.nextStepAtMs = now + 30;
                    return;
                }
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
        if (!bleClient_->setDisplayOn(state_.displayOn, state_.bluetoothIndicatorActive)) {
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
        const V1BluetoothIndicatorObservation& bluetooth = parser_->bluetoothIndicatorObservation();
        if (observed.revision != state_.observationRevision &&
            ingressAfter(observed.ingressSequence, state_.observationIngressBoundary)) {
            const bool bluetoothActive = bluetooth.available &&
                                         bluetooth.state != V1BluetoothIndicatorState::Off;
            if (observed.available && bluetooth.available &&
                bluetooth.ingressSequence == observed.ingressSequence &&
                observed.value == state_.displayOn &&
                (state_.displayOn || bluetoothActive == state_.bluetoothIndicatorActive)) {
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
        if (!quiet_->sendAutoPushVolume(state_.volume, state_.muteVolume, state_.volumeAux)) {
            failWholePlan(&status_.volume, Outcome::WRITE_FAILED, FailureReason::VOLUME_WRITE_FAILED);
            return;
        }
        status_.volume.sent = true;
        status_.volume.outcome = Outcome::SENT;
        state_.step = Step::VolumeRead;
        state_.nextStepAtMs = now + 30;
        return;

    case Step::VolumeRead:
        state_.observationRevision = parser_->allVolumeObservation().revision;
        state_.observationIngressBoundary = bleClient_->latestV1NotificationIngressSequence();
        if (!bleClient_->requestAllVolume()) {
            failWholePlan(&status_.volume, Outcome::READ_FAILED, FailureReason::VOLUME_READ_FAILED);
            return;
        }
        state_.step = Step::VolumeVerify;
        state_.verifyDeadlineMs = now + kVerificationTimeoutMs;
        state_.nextStepAtMs = now;
        return;

    case Step::VolumeVerify:
        if (parser_->allVolumeObservation().revision != state_.observationRevision &&
            ingressAfter(parser_->allVolumeObservation().ingressSequence,
                         state_.observationIngressBoundary)) {
            const V1AllVolumeObservation& observed = parser_->allVolumeObservation();
            const bool currentMatches = observed.available && observed.currentMain == state_.volume &&
                                        observed.currentMuted == state_.muteVolume;
            const bool savedMatches = state_.volumePolicy == V1VolumePolicy::Saved
                                          ? observed.savedMain == state_.volume &&
                                                observed.savedMuted == state_.muteVolume
                                          : observed.savedMain == status_.beforeSavedMainVolume &&
                                                observed.savedMuted == status_.beforeSavedMutedVolume;
            if (currentMatches && savedMatches) {
                status_.volume.verified = true;
                status_.volume.outcome = Outcome::VERIFIED;
                advanceAfterVolume(now);
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

    case Step::CustomRefreshSections: {
        if (state_.sendDeadlineMs == 0) {
            state_.sendDeadlineMs = now + kVerificationTimeoutMs;
        }
        const SendResult sent = bleClient_->requestSweepSectionsResult();
        if (sent == SendResult::NOT_YET) {
            if (deadlineReached(now, state_.sendDeadlineMs)) {
                failWholePlan(&status_.customFrequencies, Outcome::TIMEOUT,
                              FailureReason::CUSTOM_READBACK_TIMEOUT);
                return;
            }
            state_.nextStepAtMs = now + 5;
            return;
        }
        if (sent != SendResult::SENT) {
            failWholePlan(&status_.customFrequencies, Outcome::READ_FAILED,
                          FailureReason::CUSTOM_READ_FAILED);
            return;
        }
        state_.sendDeadlineMs = 0;
        bleClient_->beginSessionSweepSectionsCapture(
            bleClient_->latestV1NotificationIngressSequence());
        state_.step = Step::CustomRefreshMax;
        state_.nextStepAtMs = now + 30;
        return;
    }

    case Step::CustomRefreshMax: {
        if (state_.sendDeadlineMs == 0) {
            state_.sendDeadlineMs = now + kVerificationTimeoutMs;
        }
        const SendResult sent = bleClient_->requestMaxSweepIndexResult();
        if (sent == SendResult::NOT_YET) {
            if (deadlineReached(now, state_.sendDeadlineMs)) {
                failWholePlan(&status_.customFrequencies, Outcome::TIMEOUT,
                              FailureReason::CUSTOM_READBACK_TIMEOUT);
                return;
            }
            state_.nextStepAtMs = now + 5;
            return;
        }
        if (sent != SendResult::SENT) {
            failWholePlan(&status_.customFrequencies, Outcome::READ_FAILED,
                          FailureReason::CUSTOM_READ_FAILED);
            return;
        }
        state_.sendDeadlineMs = 0;
        bleClient_->beginSessionSweepMaxCapture(
            bleClient_->latestV1NotificationIngressSequence());
        state_.step = Step::CustomRefreshDefinitions;
        state_.nextStepAtMs = now + 30;
        return;
    }

    case Step::CustomRefreshDefinitions: {
        if (state_.sendDeadlineMs == 0) {
            state_.sendDeadlineMs = now + kVerificationTimeoutMs;
        }
        const SendResult sent = bleClient_->requestAllSweepDefinitionsResult();
        if (sent == SendResult::NOT_YET) {
            if (deadlineReached(now, state_.sendDeadlineMs)) {
                failWholePlan(&status_.customFrequencies, Outcome::TIMEOUT,
                              FailureReason::CUSTOM_READBACK_TIMEOUT);
                return;
            }
            state_.nextStepAtMs = now + 5;
            return;
        }
        if (sent != SendResult::SENT) {
            failWholePlan(&status_.customFrequencies, Outcome::READ_FAILED,
                          FailureReason::CUSTOM_READ_FAILED);
            return;
        }
        state_.sendDeadlineMs = 0;
        bleClient_->beginSessionSweepDefinitionsCapture(
            bleClient_->latestV1NotificationIngressSequence());
        state_.step = Step::CustomRefreshVerify;
        state_.verifyDeadlineMs = now + kVerificationTimeoutMs;
        state_.nextStepAtMs = now;
        return;
    }

    case Step::CustomRefreshVerify: {
        const auto& sections = parser_->sweepSectionsObservation();
        const auto& maximum = parser_->sweepMaxObservation();
        const auto& definitions = parser_->sweepDefinitionsObservation();
        const bool maxUsable = bleClient_->hasSessionSweepMaxCapture() &&
                               maximum.available && !maximum.poisoned && maximum.maxIndex < 64;
        const uint64_t required = maxUsable
                                      ? (maximum.maxIndex == 63
                                             ? UINT64_MAX
                                             : ((uint64_t{1} << (maximum.maxIndex + 1u)) - 1u))
                                      : 0;
        bool definitionsFresh = maxUsable &&
                                bleClient_->hasSessionSweepDefinitionsCapture() &&
                                !definitions.poisoned && definitions.presentMask == required;
        const uint32_t definitionsBoundary =
            bleClient_->sessionSweepDefinitionsIngressBoundary();
        for (uint8_t index = 0;
             definitionsFresh && index <= maximum.maxIndex; ++index) {
            definitionsFresh = ingressAfter(
                definitions.ingressSequences[index], definitionsBoundary);
        }
        if (bleClient_->hasSessionSweepSectionsCapture() && sections.available &&
            sections.complete && !sections.poisoned && sections.count >= 2 &&
            maxUsable && definitionsFresh) {
            state_.before.hasSweepSections = true;
            state_.before.sweepSectionCount = sections.count;
            state_.before.sweepSections = sections.sections;
            state_.before.hasMaxSweepIndex = true;
            state_.before.maxSweepIndex = maximum.maxIndex;
            state_.before.hasSweepDefinitions = true;
            state_.before.sweepDefinitions = definitions.definitions;
            state_.customCompilationDeferred = false;
            if (!compileCustomDefinitions(state_.before)) return;
            if (state_.finalUserWriteAfterCustom) {
                status_.customFrequencies.needed = true;
                status_.customFrequencies.verified = false;
                status_.customFrequencies.outcome = Outcome::PENDING;
                status_.effectiveCustomDefinitions.clear();
            }
            if (status_.customFrequencies.needed &&
                !status_.customFrequencies.verified) {
                state_.step = Step::CustomWrite;
                state_.nextStepAtMs = now + 30;
            } else {
                finishOperation();
            }
            return;
        }
        if (sections.poisoned || maximum.poisoned || definitions.poisoned) {
            failWholePlan(&status_.customFrequencies, Outcome::MISMATCH,
                          FailureReason::CUSTOM_READBACK_INVALID);
            return;
        }
        if (deadlineReached(now, state_.verifyDeadlineMs)) {
            failWholePlan(&status_.customFrequencies, Outcome::TIMEOUT,
                          FailureReason::CUSTOM_READBACK_TIMEOUT);
            return;
        }
        state_.nextStepAtMs = now + 10;
        return;
    }

    case Step::CustomWrite: {
        if (state_.sendDeadlineMs == 0) {
            state_.sendDeadlineMs = now + kVerificationTimeoutMs;
        }
        while (state_.customWriteIndex < state_.customDefinitions.size() &&
               state_.customDefinitions[state_.customWriteIndex].lowerMHz == 0) {
            ++state_.customWriteIndex;
        }
        if (state_.customWriteIndex >= state_.customDefinitions.size()) {
            failWholePlan(&status_.customFrequencies, Outcome::INVALID,
                          FailureReason::CUSTOM_CONFIGURATION_INVALID);
            return;
        }
        const auto& definition = state_.customDefinitions[state_.customWriteIndex];
        // The vendor contract disables every index omitted from a committed
        // write set. A (0,0) packet is not specified, so send used entries
        // only, preserve their original indices, and commit the final used
        // packet. Fresh full readback below proves every omitted index is off.
        const bool commit = state_.customWriteIndex == state_.customLastUsedIndex;
        if (commit) {
            state_.observationRevision = parser_->sweepWriteResultObservation().revision;
        }
        const SendResult sent = bleClient_->writeSweepDefinition(
            definition.index, definition.lowerMHz, definition.upperMHz, commit);
        if (sent == SendResult::NOT_YET) {
            if (deadlineReached(now, state_.sendDeadlineMs)) {
                failWholePlan(&status_.customFrequencies, Outcome::TIMEOUT,
                              FailureReason::CUSTOM_WRITE_FAILED);
                return;
            }
            state_.nextStepAtMs = now + 5;
            return;
        }
        if (sent != SendResult::SENT) {
            failWholePlan(&status_.customFrequencies, Outcome::WRITE_FAILED,
                          FailureReason::CUSTOM_WRITE_FAILED);
            return;
        }
        state_.sendDeadlineMs = 0;
        if (!commit) {
            ++state_.customWriteIndex;
            state_.nextStepAtMs = now + 5;
            return;
        }
        status_.customFrequencies.sent = true;
        status_.customFrequencies.outcome = Outcome::SENT;
        state_.observationIngressBoundary = bleClient_->latestV1NotificationIngressSequence();
        state_.step = Step::CustomCommitVerify;
        state_.verifyDeadlineMs = now + kVerificationTimeoutMs;
        state_.nextStepAtMs = now;
        return;
    }

    case Step::CustomCommitVerify: {
        const auto& result = parser_->sweepWriteResultObservation();
        if (result.revision != state_.observationRevision &&
            ingressAfter(result.ingressSequence, state_.observationIngressBoundary)) {
            status_.customCommitResultAvailable = result.available;
            status_.customCommitResultRaw = result.result;
            status_.customCommitInvalidDefinitionIndex = result.available && result.result != 0
                                                             ? static_cast<int16_t>(result.result - 1u)
                                                             : -1;
            if (!result.available || result.result != 0) {
                failWholePlan(&status_.customFrequencies, Outcome::MISMATCH,
                              FailureReason::CUSTOM_COMMIT_REJECTED);
                return;
            }
            state_.step = Step::CustomRead;
            state_.nextStepAtMs = now + 30;
            return;
        }
        if (deadlineReached(now, state_.verifyDeadlineMs)) {
            failWholePlan(&status_.customFrequencies, Outcome::TIMEOUT,
                          FailureReason::CUSTOM_COMMIT_TIMEOUT);
            return;
        }
        state_.nextStepAtMs = now + 10;
        return;
    }

    case Step::CustomRead: {
        if (state_.sendDeadlineMs == 0) {
            state_.sendDeadlineMs = now + kVerificationTimeoutMs;
        }
        // Capture ingress before the synchronous request: the first reply can
        // enter BLE while the acknowledged send is still waiting to return.
        // Reset before that reply is parsed, while the ingress boundary still
        // excludes packets queued before this request (including on retries).
        state_.observationIngressBoundary = bleClient_->latestV1NotificationIngressSequence();
        parser_->resetSweepDefinitionsObservation();
        const SendResult sent = bleClient_->requestAllSweepDefinitionsResult();
        if (sent == SendResult::NOT_YET) {
            if (deadlineReached(now, state_.sendDeadlineMs)) {
                failWholePlan(&status_.customFrequencies, Outcome::TIMEOUT,
                              FailureReason::CUSTOM_READBACK_TIMEOUT);
                return;
            }
            state_.nextStepAtMs = now + 5;
            return;
        }
        if (sent != SendResult::SENT) {
            failWholePlan(&status_.customFrequencies, Outcome::READ_FAILED,
                          FailureReason::CUSTOM_READ_FAILED);
            return;
        }
        state_.sendDeadlineMs = 0;
        state_.step = Step::CustomVerify;
        state_.verifyDeadlineMs = now + kVerificationTimeoutMs;
        state_.nextStepAtMs = now;
        return;
    }

    case Step::CustomVerify: {
        const auto& definitions = parser_->sweepDefinitionsObservation();
        bool everyDefinitionAfterBoundary = true;
        for (uint8_t index = 0; index <= state_.before.maxSweepIndex; ++index) {
            everyDefinitionAfterBoundary &=
                ingressAfter(definitions.ingressSequences[index], state_.observationIngressBoundary);
        }
        if (!definitions.poisoned &&
            definitions.presentMask == state_.customRequiredMask &&
            everyDefinitionAfterBoundary) {
            status_.effectiveCustomDefinitions.clear();
            bool valid = true;
            for (uint8_t index = 0; index <= state_.before.maxSweepIndex; ++index) {
                const auto& observed = definitions.definitions[index];
                V1CustomFrequencyDefinition effective{index, observed.lowerMHz, observed.upperMHz};
                const bool unused = effective.lowerMHz == 0 && effective.upperMHz == 0;
                const bool hasRequested = index < state_.customDefinitions.size();
                const V1CustomFrequencyDefinition requested = hasRequested
                    ? state_.customDefinitions[index]
                    : V1CustomFrequencyDefinition{static_cast<uint8_t>(index), 0, 0};
                const bool requestedUnused = requested.lowerMHz == 0 && requested.upperMHz == 0;
                const bool preserved =
                    (state_.customPreservedMask & (uint64_t{1} << index)) != 0;
                if (unused != requestedUnused ||
                    (preserved &&
                     (effective.lowerMHz != requested.lowerMHz ||
                      effective.upperMHz != requested.upperMHz)) ||
                    (!unused && (sweepDefinitionLiveSection(effective, state_.before) < 0 ||
                                 sweepDefinitionLiveSection(effective, state_.before) !=
                                     sweepDefinitionLiveSection(requested, state_.before))) ||
                    (effective.lowerMHz == 0) != (effective.upperMHz == 0)) {
                    valid = false;
                    break;
                }
                if (!status_.effectiveCustomDefinitions.push_back(effective)) {
                    valid = false;
                    break;
                }
            }
            if (!valid) {
                failWholePlan(&status_.customFrequencies, Outcome::MISMATCH,
                              FailureReason::CUSTOM_READBACK_INVALID);
                return;
            }
            status_.customFrequencies.verified = true;
            status_.customFrequencies.outcome = Outcome::VERIFIED;
            if (state_.finalUserWriteAfterCustom) {
                state_.finalUserWriteAfterCustom = false;
                state_.userWriteBytes = state_.effectiveUserBytes;
                state_.step = Step::UserWrite;
                state_.nextStepAtMs = now + 30;
                return;
            }
            finishOperation();
            return;
        }
        if (deadlineReached(now, state_.verifyDeadlineMs)) {
            failWholePlan(&status_.customFrequencies, Outcome::TIMEOUT,
                          FailureReason::CUSTOM_READBACK_TIMEOUT);
            return;
        }
        state_.nextStepAtMs = now + 10;
        return;
    }

    case Step::Idle:
    default:
        return;
    }
}

bool AutoPushModule::appendStatusJson(JsonObject root) const {
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
        case Step::CustomRefreshSections: return "CustomRefreshSections";
        case Step::CustomRefreshMax: return "CustomRefreshMax";
        case Step::CustomRefreshDefinitions: return "CustomRefreshDefinitions";
        case Step::CustomRefreshVerify: return "CustomRefreshVerify";
        case Step::CustomWrite: return "CustomWrite";
        case Step::CustomCommitVerify: return "CustomCommitVerify";
        case Step::CustomRead: return "CustomRead";
        case Step::CustomVerify: return "CustomVerify";
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
        case FailureReason::CUSTOM_CONFIGURATION_INVALID: return "custom_configuration_invalid";
        case FailureReason::CUSTOM_WRITE_FAILED: return "custom_write_failed";
        case FailureReason::CUSTOM_COMMIT_REJECTED: return "custom_commit_rejected";
        case FailureReason::CUSTOM_COMMIT_TIMEOUT: return "custom_commit_timeout";
        case FailureReason::CUSTOM_READ_FAILED: return "custom_read_failed";
        case FailureReason::CUSTOM_READBACK_INVALID: return "custom_readback_invalid";
        case FailureReason::CUSTOM_READBACK_TIMEOUT: return "custom_readback_timeout";
        case FailureReason::PROXY_OWNS_DETECTOR: return "proxy_owns_detector";
        }
        return "invalid_policy";
    };
    const auto appendBytes = [](JsonArray values, const std::array<uint8_t, 6>& bytes) {
        for (uint8_t value : bytes) values.add(value);
    };
    const auto appendDefinitions = [](JsonArray values,
                                      const FixedDefinitionList& definitions) {
        for (const V1CustomFrequencyDefinition& definition : definitions) {
            JsonObject item = values.add<JsonObject>();
            item["index"] = definition.index;
            item["lowerMHz"] = definition.lowerMHz;
            item["upperMHz"] = definition.upperMHz;
        }
    };
    const auto appendBase = [&](JsonObject object, const ComponentStatus& component) {
        object["requested"] = component.requested;
        object["beforeAvailable"] = component.beforeAvailable;
        object["needed"] = component.needed;
        object["sent"] = component.sent;
        object["verified"] = component.verified;
        object["applied"] = component.verified; // backward-compatible alias; never merely sent
        object["outcome"] = outcomeName(component.outcome);
        object["reason"] = reasonName(component.reason);
    };

    root["active"] = state_.step != Step::Idle;
    root["operationId"] = status_.operationId;
    root["slot"] = status_.slotIndex;
    root["step"] = stepName();
    root["result"] = operationResultName(status_.result);
    root["reason"] = reasonName(status_.reason);
    root["profileLoaded"] = status_.profileLoaded;
    root["profileConfigured"] = status_.profileName.length() > 0;
    root["profileName"] = status_.profileName;
    JsonObject components = root["components"].to<JsonObject>();

    JsonObject profile = components["profile"].to<JsonObject>();
    appendBase(profile, status_.userSettings);
    if (status_.userSettings.beforeAvailable) {
        appendBytes(profile["before"].to<JsonArray>(), status_.beforeUserBytes);
    } else profile["before"] = nullptr;
    appendBytes(profile["desired"].to<JsonArray>(), status_.desiredUserBytes);
    appendBytes(profile["effective"].to<JsonArray>(), status_.effectiveUserBytes);
    appendBytes(profile["supportedMasks"].to<JsonArray>(), status_.supportedUserMasks);
    profile["alpLaserOverrideActive"] = status_.alpLaserOverrideActive;
    profile["alpLaserOverrideApplied"] = status_.alpLaserOverrideApplied;

    JsonObject display = components["display"].to<JsonObject>();
    appendBase(display, status_.display);
    if (status_.display.beforeAvailable) display["before"] = status_.beforeDisplayOn;
    else display["before"] = nullptr;
    display["desired"] = status_.desiredDisplayOn;
    display["bluetoothBeforeActive"] = status_.beforeBluetoothIndicatorActive;
    display["bluetoothDesiredActive"] = status_.desiredBluetoothIndicatorActive;

    JsonObject mode = components["mode"].to<JsonObject>();
    appendBase(mode, status_.mode);
    if (status_.mode.beforeAvailable) mode["before"] = status_.beforeMode;
    else mode["before"] = nullptr;
    mode["desired"] = status_.desiredMode;

    JsonObject volume = components["volume"].to<JsonObject>();
    appendBase(volume, status_.volume);
    if (status_.volume.beforeAvailable) {
        JsonObject before = volume["before"].to<JsonObject>();
        before["main"] = status_.beforeMainVolume;
        before["muted"] = status_.beforeMutedVolume;
        before["savedMain"] = status_.beforeSavedMainVolume;
        before["savedMuted"] = status_.beforeSavedMutedVolume;
    } else volume["before"] = nullptr;
    JsonObject desired = volume["desired"].to<JsonObject>();
    desired["main"] = status_.desiredMainVolume;
    desired["muted"] = status_.desiredMutedVolume;
    volume["feedback"] = status_.volumeFeedback == V1VolumeFeedbackPolicy::Always
                             ? "always"
                             : (status_.volumeFeedback == V1VolumeFeedbackPolicy::ChangedOnly
                                    ? "changed_only" : "none");
    volume["disconnect"] = status_.volumeDisconnect == V1VolumeDisconnectPolicy::KeepCurrent
                                ? "keep_current" : "restore_saved";
    volume["commandAux"] = status_.volumeCommandAux;
    volume["valuesVerified"] = status_.volume.verified;
    volume["verificationScope"] = status_.volumePolicy == V1VolumePolicy::Saved
                                       ? "current_and_saved_values_only"
                                       : "current_values_and_saved_preservation_only";
    volume["policyReadbackAvailable"] = false;
    volume["policyOutcome"] = status_.volume.sent ? "sent_unobservable" : "not_sent";

    JsonObject custom = components["customFrequencies"].to<JsonObject>();
    appendBase(custom, status_.customFrequencies);
    custom["verificationScope"] = "topology_and_live_section_with_calibrated_readback";
    if (status_.customCommitResultAvailable) {
        custom["commitResultRaw"] = status_.customCommitResultRaw;
    } else {
        custom["commitResultRaw"] = nullptr;
    }
    if (status_.customCommitInvalidDefinitionIndex >= 0) {
        custom["invalidDefinitionIndex"] = status_.customCommitInvalidDefinitionIndex;
    } else {
        custom["invalidDefinitionIndex"] = nullptr;
    }
    appendDefinitions(custom["requestedDefinitions"].to<JsonArray>(), status_.requestedCustomDefinitions);
    appendDefinitions(custom["calibratedReadback"].to<JsonArray>(), status_.effectiveCustomDefinitions);
    return true;
}

String AutoPushModule::getStatusJson() const {
    PsramJson::Document doc;
    if (!appendStatusJson(doc.to<JsonObject>()) || doc.overflowed()) return String();
    const size_t expected = measureJson(doc);
    if (expected == 0 || expected > 16u * 1024u) return String();
    String json;
    json.reserve(expected);
    if (serializeJson(doc, json) != expected || json.length() != expected) return String();
    return json;
}

AutoPushModule::ExecutionSummary AutoPushModule::executionSummary() const {
    ExecutionSummary summary;
    summary.operationId = status_.operationId;
    summary.active = isActive();
    switch (status_.result) {
    case Result::NONE: summary.result = PublicResult::None; break;
    case Result::QUEUED: summary.result = PublicResult::Queued; break;
    case Result::IN_PROGRESS: summary.result = PublicResult::InProgress; break;
    case Result::SUCCEEDED: summary.result = PublicResult::Succeeded; break;
    case Result::PARTIAL: summary.result = PublicResult::Partial; break;
    case Result::FAILED: summary.result = PublicResult::Failed; break;
    }

    const ComponentStatus* source[] = {
        &status_.userSettings,
        &status_.display,
        &status_.mode,
        &status_.volume,
        &status_.customFrequencies,
    };
    const auto mapOutcome = [](Outcome outcome) {
        using Durable = V1SettingsOperationStore::ComponentOutcome;
        switch (outcome) {
        case Outcome::NOT_REQUESTED: return Durable::NotRequested;
        case Outcome::PENDING: return Durable::Pending;
        case Outcome::UNCHANGED: return Durable::Unchanged;
        case Outcome::SENT: return Durable::Sent;
        case Outcome::VERIFIED: return Durable::Verified;
        case Outcome::UNSUPPORTED: return Durable::Unsupported;
        case Outcome::INVALID: return Durable::Invalid;
        case Outcome::BLOCKED: return Durable::Blocked;
        case Outcome::WRITE_FAILED: return Durable::WriteFailed;
        case Outcome::READ_FAILED: return Durable::ReadFailed;
        case Outcome::MISMATCH: return Durable::Mismatch;
        case Outcome::TIMEOUT: return Durable::Timeout;
        case Outcome::DISCONNECTED: return Durable::Disconnected;
        case Outcome::SESSION_CHANGED: return Durable::SessionChanged;
        case Outcome::LOAD_FAILED: return Durable::LoadFailed;
        }
        return Durable::Invalid;
    };
    const auto mapReason = [](FailureReason reason) {
        using Durable = V1SettingsOperationStore::ComponentReason;
        switch (reason) {
        case FailureReason::NONE: return Durable::None;
        case FailureReason::DISCONNECTED: return Durable::Disconnected;
        case FailureReason::SESSION_CHANGED: return Durable::SessionChanged;
        case FailureReason::MISSING_LIVE_SNAPSHOT: return Durable::MissingLiveSnapshot;
        case FailureReason::VERSION_UNKNOWN: return Durable::VersionUnknown;
        case FailureReason::UNSUPPORTED_FIRMWARE: return Durable::UnsupportedFirmware;
        case FailureReason::PROFILE_BUSY: return Durable::ProfileBusy;
        case FailureReason::PROFILE_LOAD_FAILED: return Durable::ProfileLoadFailed;
        case FailureReason::INVALID_PROFILE_SCHEMA: return Durable::InvalidProfileSchema;
        case FailureReason::INVALID_USER_SETTING_VALUE: return Durable::InvalidUserSettingValue;
        case FailureReason::INVALID_POLICY: return Durable::InvalidPolicy;
        case FailureReason::INVALID_VOLUME_PAIR: return Durable::InvalidVolumePair;
        case FailureReason::UNSUPPORTED_SAVED_VOLUME: return Durable::UnsupportedSavedVolume;
        case FailureReason::UNSUPPORTED_CUSTOM_FREQUENCIES: return Durable::UnsupportedCustomFrequencies;
        case FailureReason::UNSUPPORTED_BLUETOOTH_LED: return Durable::UnsupportedBluetoothLed;
        case FailureReason::CUSTOM_FREQUENCY_PRESERVATION_REQUIRED:
            return Durable::CustomFrequencyPreservationRequired;
        case FailureReason::EURO_ADVANCED_MODE_INVALID: return Durable::EuroAdvancedModeInvalid;
        case FailureReason::VOLUME_OWNER_BUSY: return Durable::VolumeOwnerBusy;
        case FailureReason::USER_BYTES_BEFORE_REQUIRED: return Durable::UserBytesBeforeRequired;
        case FailureReason::USER_BYTES_WRITE_FAILED: return Durable::UserBytesWriteFailed;
        case FailureReason::USER_BYTES_READ_FAILED: return Durable::UserBytesReadFailed;
        case FailureReason::USER_BYTES_MISMATCH: return Durable::UserBytesMismatch;
        case FailureReason::USER_BYTES_TIMEOUT: return Durable::UserBytesTimeout;
        case FailureReason::DISPLAY_WRITE_FAILED: return Durable::DisplayWriteFailed;
        case FailureReason::DISPLAY_MISMATCH: return Durable::DisplayMismatch;
        case FailureReason::DISPLAY_TIMEOUT: return Durable::DisplayTimeout;
        case FailureReason::MODE_WRITE_FAILED: return Durable::ModeWriteFailed;
        case FailureReason::MODE_MISMATCH: return Durable::ModeMismatch;
        case FailureReason::MODE_TIMEOUT: return Durable::ModeTimeout;
        case FailureReason::VOLUME_WRITE_FAILED: return Durable::VolumeWriteFailed;
        case FailureReason::VOLUME_READ_FAILED: return Durable::VolumeReadFailed;
        case FailureReason::VOLUME_MISMATCH: return Durable::VolumeMismatch;
        case FailureReason::VOLUME_TIMEOUT: return Durable::VolumeTimeout;
        case FailureReason::CUSTOM_CONFIGURATION_INVALID: return Durable::CustomConfigurationInvalid;
        case FailureReason::CUSTOM_WRITE_FAILED: return Durable::CustomWriteFailed;
        case FailureReason::CUSTOM_COMMIT_REJECTED: return Durable::CustomCommitRejected;
        case FailureReason::CUSTOM_COMMIT_TIMEOUT: return Durable::CustomCommitTimeout;
        case FailureReason::CUSTOM_READ_FAILED: return Durable::CustomReadFailed;
        case FailureReason::CUSTOM_READBACK_INVALID: return Durable::CustomReadbackInvalid;
        case FailureReason::CUSTOM_READBACK_TIMEOUT: return Durable::CustomReadbackTimeout;
        case FailureReason::PROXY_OWNS_DETECTOR: return Durable::ProxyOwnsDetector;
        }
        return Durable::InvalidPolicy;
    };
    for (size_t index = 0; index < sizeof(source) / sizeof(source[0]); ++index) {
        summary.components[index].requested = source[index]->requested;
        summary.components[index].sent = source[index]->sent;
        summary.components[index].verified = source[index]->verified;
        summary.components[index].outcome = mapOutcome(source[index]->outcome);
        summary.components[index].reason = mapReason(source[index]->reason);
    }
    return summary;
}
