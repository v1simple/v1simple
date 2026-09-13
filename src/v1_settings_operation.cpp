#include "v1_settings_operation.h"

#include <Preferences.h>

#include <cstddef>
#include <cstring>
#include <type_traits>

#include "profile_name.h"

namespace {
constexpr char kNamespace[] = "v1setjob";
constexpr char kRecordKey[] = "record";
constexpr uint32_t kMagic = 0x56314A42u; // V1JB
constexpr uint16_t kVersion = 1;
constexpr uint8_t kReturnToMaintenance = 0x01;

bool isHexUpper(char value) {
    return (value >= '0' && value <= '9') || (value >= 'A' && value <= 'F');
}
} // namespace

struct V1SettingsOperationStore::Record {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint8_t kind = 0;
    uint8_t source = 0;
    uint8_t state = 0;
    uint8_t reason = 0;
    int8_t slot = -1;
    uint8_t flags = 0;
    uint32_t operationId = 0;
    uint32_t executorOperationId = 0;
    char targetAddress[18] = {0};
    char profileName[65] = {0};
    uint8_t requestedMask = 0;
    uint8_t sentMask = 0;
    uint8_t verifiedMask = 0;
    uint8_t componentOutcomes[kComponentCount] = {0};
    uint8_t componentReasons[kComponentCount] = {0};
    uint8_t reserved[2] = {0, 0};
    uint32_t crc32 = 0;
};

uint32_t V1SettingsOperationStore::calculateCrc(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t index = 0; index < length; ++index) {
        crc ^= data[index];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1u) ^ ((crc & 1u) ? 0xEDB88320u : 0u);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

bool V1SettingsOperationStore::isCanonicalAddress(const char* address) {
    if (!address || std::strlen(address) != 17u) return false;
    for (size_t index = 0; index < 17u; ++index) {
        if (index == 2u || index == 5u || index == 8u || index == 11u || index == 14u) {
            if (address[index] != ':') return false;
        } else if (!isHexUpper(address[index])) {
            return false;
        }
    }
    return true;
}

bool V1SettingsOperationStore::hasDurableFactoryResetSend(const Snapshot& snapshot) {
    if (snapshot.kind != Kind::FactoryReset) return false;
    const ComponentSummary& reset = snapshot.components[static_cast<size_t>(Component::FactoryReset)];
    return reset.requested && reset.sent && !reset.verified &&
           reset.outcome == ComponentOutcome::Sent && reset.reason == ComponentReason::None;
}

bool V1SettingsOperationStore::factoryUserDefaultsMatch(
    const std::array<uint8_t, 6>& userBytes, bool hasUserBytes) {
    if (!hasUserBytes) return false;
    for (uint8_t byte : userBytes) {
        if (byte != 0xFFu) return false;
    }
    return true;
}

uint32_t V1SettingsOperationStore::requiredPreApplyIngressBoundary(
    State state, uint32_t recaptureBoundary) {
    return state == State::Preparing ? recaptureBoundary : 0;
}

bool V1SettingsOperationStore::nextOperationId(uint32_t current, uint32_t& next) {
    if (current == UINT32_MAX) return false;
    next = current + 1u;
    return next != 0;
}

bool V1SettingsOperationStore::decodeRecord(const Record& record, Snapshot& snapshot) {
    const uint32_t crc = calculateCrc(reinterpret_cast<const uint8_t*>(&record), offsetof(Record, crc32));
    if (record.magic != kMagic || record.version != kVersion || record.crc32 != crc ||
        record.operationId == 0 || record.targetAddress[17] != '\0' ||
        !isCanonicalAddress(record.targetAddress) ||
        record.kind < static_cast<uint8_t>(Kind::ApplySlot) ||
        record.kind > static_cast<uint8_t>(Kind::FactoryReset) ||
        record.source < static_cast<uint8_t>(Source::MaintenanceUi) ||
        record.source > static_cast<uint8_t>(Source::TripleTap) ||
        record.state < static_cast<uint8_t>(State::PendingNormalBoot) ||
        record.state > static_cast<uint8_t>(State::Failed) ||
        record.reason > static_cast<uint8_t>(Reason::InvalidRecord) ||
        (record.kind == static_cast<uint8_t>(Kind::ApplySlot) && (record.slot < 0 || record.slot > 2)) ||
        (record.kind == static_cast<uint8_t>(Kind::ApplyProfile) && record.slot != -1) ||
        (record.kind == static_cast<uint8_t>(Kind::FactoryReset) && record.slot != -1) ||
        (record.kind == static_cast<uint8_t>(Kind::FactoryReset) &&
         record.source != static_cast<uint8_t>(Source::MaintenanceUi)) ||
        (record.source == static_cast<uint8_t>(Source::TripleTap) &&
         record.kind != static_cast<uint8_t>(Kind::ApplySlot)) ||
        (record.source == static_cast<uint8_t>(Source::TripleTap) &&
         ((record.flags & kReturnToMaintenance) != 0 ||
          record.state == static_cast<uint8_t>(State::PendingNormalBoot))) ||
        (record.flags & ~kReturnToMaintenance) != 0) {
        return false;
    }
    const size_t profileLength = strnlen(record.profileName, sizeof(record.profileName));
    String canonicalProfile;
    const bool profileCanonical = profileLength > 0 &&
        canonicalizeProfileName(String(record.profileName), canonicalProfile) == ProfileNameStatus::Valid &&
        canonicalProfile == record.profileName;
    if (profileLength == sizeof(record.profileName) ||
        (record.kind == static_cast<uint8_t>(Kind::ApplyProfile) && !profileCanonical) ||
        (record.kind != static_cast<uint8_t>(Kind::ApplyProfile) && profileLength != 0)) return false;
    constexpr uint8_t kComponentMask = static_cast<uint8_t>((1u << kComponentCount) - 1u);
    if ((record.requestedMask & ~kComponentMask) != 0 || (record.sentMask & ~record.requestedMask) != 0 ||
        (record.verifiedMask & ~record.requestedMask) != 0) return false;
    bool allRequestedSucceeded = true;
    for (size_t index = 0; index < kComponentCount; ++index) {
        if (record.componentOutcomes[index] > static_cast<uint8_t>(ComponentOutcome::LoadFailed) ||
            record.componentReasons[index] > static_cast<uint8_t>(ComponentReason::ProxyOwnsDetector)) return false;
        const uint8_t bit = static_cast<uint8_t>(1u << index);
        const bool requested = (record.requestedMask & bit) != 0;
        const bool sent = (record.sentMask & bit) != 0;
        const bool verified = (record.verifiedMask & bit) != 0;
        const ComponentOutcome outcome = static_cast<ComponentOutcome>(record.componentOutcomes[index]);
        const ComponentReason componentReason = static_cast<ComponentReason>(record.componentReasons[index]);
        if (requested) {
            allRequestedSucceeded = allRequestedSucceeded &&
                ((outcome == ComponentOutcome::Verified && sent && verified && componentReason == ComponentReason::None) ||
                 (outcome == ComponentOutcome::Unchanged && !sent && verified && componentReason == ComponentReason::None));
        }
        if ((!requested && (record.componentOutcomes[index] != static_cast<uint8_t>(ComponentOutcome::NotRequested) ||
                            record.componentReasons[index] != static_cast<uint8_t>(ComponentReason::None))) ||
            (requested && record.componentOutcomes[index] == static_cast<uint8_t>(ComponentOutcome::NotRequested)) ||
            (outcome == ComponentOutcome::Unchanged &&
             (!requested || sent || !verified || componentReason != ComponentReason::None)) ||
            (outcome == ComponentOutcome::Verified &&
             (!requested || !sent || !verified || componentReason != ComponentReason::None)) ||
            (verified && outcome != ComponentOutcome::Verified && outcome != ComponentOutcome::Unchanged) ||
            (outcome == ComponentOutcome::Pending &&
             (sent || componentReason != ComponentReason::None)) ||
            (outcome == ComponentOutcome::Sent && (!requested || !sent || verified))) {
            return false;
        }
    }
    const State state = static_cast<State>(record.state);
    const Reason reason = static_cast<Reason>(record.reason);
    if ((state == State::PendingNormalBoot || state == State::WaitingForDetector ||
         state == State::Preparing || state == State::Running) && reason != Reason::None) return false;
    if (state == State::Succeeded &&
        (reason != Reason::None || !allRequestedSucceeded ||
         record.kind == static_cast<uint8_t>(Kind::FactoryReset))) return false;
    if ((state == State::Partial || state == State::Failed) && reason == Reason::None) return false;
    const uint8_t factoryBit = static_cast<uint8_t>(1u << static_cast<uint8_t>(Component::FactoryReset));
    if (record.kind != static_cast<uint8_t>(Kind::FactoryReset) && (record.requestedMask & factoryBit) != 0) return false;
    const bool factoryResetRequested = (record.requestedMask & factoryBit) != 0;
    const bool preSendFactoryFailure = state == State::Failed &&
        (reason == Reason::DetectorTimeout || reason == Reason::WrongDetector ||
         reason == Reason::SnapshotStoreUnavailable || reason == Reason::Interrupted ||
         reason == Reason::StorageUnavailable);
    if (record.kind == static_cast<uint8_t>(Kind::FactoryReset) &&
        (state == State::Running || state == State::Recapturing || state == State::Partial ||
         state == State::Succeeded || (state == State::Failed && !preSendFactoryFailure)) &&
        !factoryResetRequested) return false;
    if (record.executorOperationId != 0 && record.kind == static_cast<uint8_t>(Kind::FactoryReset)) return false;
    if (record.executorOperationId != 0 && state != State::Running && state != State::Recapturing &&
        state != State::Succeeded && state != State::Partial && state != State::Failed) return false;

    snapshot = Snapshot{};
    snapshot.available = true;
    snapshot.valid = true;
    snapshot.operationId = record.operationId;
    snapshot.executorOperationId = record.executorOperationId;
    snapshot.kind = static_cast<Kind>(record.kind);
    snapshot.source = static_cast<Source>(record.source);
    snapshot.state = static_cast<State>(record.state);
    snapshot.reason = static_cast<Reason>(record.reason);
    snapshot.slot = record.slot;
    snapshot.returnToMaintenance = (record.flags & kReturnToMaintenance) != 0;
    std::memcpy(snapshot.targetAddress, record.targetAddress, sizeof(snapshot.targetAddress));
    snapshot.targetAddress[sizeof(snapshot.targetAddress) - 1u] = '\0';
    std::memcpy(snapshot.profileName, record.profileName, sizeof(snapshot.profileName));
    snapshot.profileName[sizeof(snapshot.profileName) - 1u] = '\0';
    for (size_t index = 0; index < kComponentCount; ++index) {
        const uint8_t bit = static_cast<uint8_t>(1u << index);
        snapshot.components[index].requested = (record.requestedMask & bit) != 0;
        snapshot.components[index].sent = (record.sentMask & bit) != 0;
        snapshot.components[index].verified = (record.verifiedMask & bit) != 0;
        snapshot.components[index].outcome = static_cast<ComponentOutcome>(record.componentOutcomes[index]);
        snapshot.components[index].reason = static_cast<ComponentReason>(record.componentReasons[index]);
    }
    return true;
}

V1SettingsOperationStore::Record V1SettingsOperationStore::encodeRecord(const Snapshot& snapshot) {
    Record record{};
    record.magic = kMagic;
    record.version = kVersion;
    record.kind = static_cast<uint8_t>(snapshot.kind);
    record.source = static_cast<uint8_t>(snapshot.source);
    record.state = static_cast<uint8_t>(snapshot.state);
    record.reason = static_cast<uint8_t>(snapshot.reason);
    record.slot = snapshot.slot;
    record.flags = snapshot.returnToMaintenance ? kReturnToMaintenance : 0;
    record.operationId = snapshot.operationId;
    record.executorOperationId = snapshot.executorOperationId;
    std::memcpy(record.targetAddress, snapshot.targetAddress, sizeof(record.targetAddress));
    record.targetAddress[sizeof(record.targetAddress) - 1u] = '\0';
    std::memcpy(record.profileName, snapshot.profileName, sizeof(record.profileName));
    record.profileName[sizeof(record.profileName) - 1u] = '\0';
    for (size_t index = 0; index < kComponentCount; ++index) {
        const uint8_t bit = static_cast<uint8_t>(1u << index);
        if (snapshot.components[index].requested) record.requestedMask |= bit;
        if (snapshot.components[index].sent) record.sentMask |= bit;
        if (snapshot.components[index].verified) record.verifiedMask |= bit;
        record.componentOutcomes[index] = static_cast<uint8_t>(snapshot.components[index].outcome);
        record.componentReasons[index] = static_cast<uint8_t>(snapshot.components[index].reason);
    }
    record.crc32 = calculateCrc(reinterpret_cast<const uint8_t*>(&record), offsetof(Record, crc32));
    return record;
}

V1SettingsOperationStore::LoadStatus V1SettingsOperationStore::begin() {
    Preferences preferences;
    // Opening read/write is intentional: Arduino Preferences may reject a
    // read-only open for a namespace that has never existed. This creates the
    // empty namespace but still reports real NVS-open failures.
    if (!preferences.begin(kNamespace, false)) {
        snapshot_ = Snapshot{};
        snapshot_.valid = false;
        loadStatus_ = LoadStatus::Unavailable;
        return loadStatus_;
    }
    if (!preferences.isKey(kRecordKey)) {
        preferences.end();
        snapshot_ = Snapshot{};
        loadStatus_ = LoadStatus::Ready;
        return loadStatus_;
    }
    Record record{};
    const size_t read = preferences.getBytes(kRecordKey, &record, sizeof(record));
    preferences.end();
    Snapshot decoded;
    if (read != sizeof(record) || !decodeRecord(record, decoded)) {
        snapshot_ = Snapshot{};
        snapshot_.available = true;
        snapshot_.valid = false;
        snapshot_.reason = Reason::InvalidRecord;
        loadStatus_ = LoadStatus::Corrupt;
        return loadStatus_;
    }
    snapshot_ = decoded;
    loadStatus_ = LoadStatus::Ready;
    return loadStatus_;
}

bool V1SettingsOperationStore::writeRecord(const Snapshot& snapshot) {
    const Record record = encodeRecord(snapshot);
    Snapshot validated;
    if (!decodeRecord(record, validated)) return false;
    Preferences preferences;
    if (!preferences.begin(kNamespace, false)) return false;
    const bool written = preferences.putBytes(kRecordKey, &record, sizeof(record)) == sizeof(record);
    preferences.end();
    if (written) {
        snapshot_ = validated;
        loadStatus_ = LoadStatus::Ready;
    }
    return written;
}

V1SettingsOperationStore::StartResult V1SettingsOperationStore::start(
    Kind kind, int slot, const char* profileName, const char* canonicalAddress, Source source,
    bool pendingNormalBoot, bool returnToMaintenance) {
    if (loadStatus_ != LoadStatus::Ready) return {StartStatus::StorageUnavailable, 0};
    if (isActive()) return {StartStatus::Active, snapshot_.operationId};
    const bool validSource = source == Source::MaintenanceUi || source == Source::TripleTap;
    if (!validSource || !isCanonicalAddress(canonicalAddress) ||
        (kind == Kind::ApplySlot && (slot < 0 || slot > 2)) ||
        (kind == Kind::ApplyProfile && (slot != -1 || !profileName || profileName[0] == '\0' ||
                                        std::strlen(profileName) > 64u)) ||
        (kind != Kind::ApplyProfile && profileName && profileName[0] != '\0') ||
        (kind == Kind::FactoryReset && slot != -1) || kind == Kind::None) {
        return {StartStatus::Invalid, 0};
    }
    if ((kind == Kind::FactoryReset && source != Source::MaintenanceUi) ||
        (source == Source::TripleTap &&
         (kind != Kind::ApplySlot || pendingNormalBoot || returnToMaintenance))) {
        return {StartStatus::Invalid, 0};
    }
    if (kind == Kind::ApplyProfile) {
        String canonical;
        if (canonicalizeProfileName(String(profileName), canonical) != ProfileNameStatus::Valid ||
            canonical != profileName) return {StartStatus::Invalid, 0};
    }

    Snapshot candidate{};
    candidate.available = true;
    candidate.valid = true;
    if (!nextOperationId(snapshot_.operationId, candidate.operationId)) {
        return {StartStatus::IdentityExhausted, 0};
    }
    candidate.kind = kind;
    candidate.source = source;
    candidate.state = pendingNormalBoot ? State::PendingNormalBoot : State::WaitingForDetector;
    candidate.reason = Reason::None;
    candidate.slot = static_cast<int8_t>(slot);
    candidate.returnToMaintenance = returnToMaintenance;
    std::memcpy(candidate.targetAddress, canonicalAddress, 18u);
    if (kind == Kind::ApplyProfile) std::memcpy(candidate.profileName, profileName, std::strlen(profileName) + 1u);
    if (!writeRecord(candidate)) return {StartStatus::StorageUnavailable, 0};
    return {StartStatus::Started, candidate.operationId};
}

V1SettingsOperationStore::StartResult V1SettingsOperationStore::startApply(
    int slot, const char* canonicalAddress, Source source,
    bool pendingNormalBoot, bool returnToMaintenance) {
    return start(Kind::ApplySlot, slot, nullptr, canonicalAddress, source, pendingNormalBoot, returnToMaintenance);
}

V1SettingsOperationStore::StartResult V1SettingsOperationStore::startProfileApply(
    const char* profileName, const char* canonicalAddress,
    bool pendingNormalBoot, bool returnToMaintenance) {
    return start(Kind::ApplyProfile, -1, profileName, canonicalAddress, Source::MaintenanceUi,
                 pendingNormalBoot, returnToMaintenance);
}

V1SettingsOperationStore::StartResult V1SettingsOperationStore::startFactoryReset(
    const char* canonicalAddress, bool pendingNormalBoot, bool returnToMaintenance) {
    return start(Kind::FactoryReset, -1, nullptr, canonicalAddress, Source::MaintenanceUi,
                 pendingNormalBoot, returnToMaintenance);
}

bool V1SettingsOperationStore::transition(State state, Reason reason, uint32_t executorOperationId) {
    if (loadStatus_ != LoadStatus::Ready || !snapshot_.available || !snapshot_.valid) return false;
    Snapshot candidate = snapshot_;
    candidate.state = state;
    candidate.reason = reason;
    candidate.executorOperationId = executorOperationId;
    return writeRecord(candidate);
}

bool V1SettingsOperationStore::beginNormalBoot() {
    if (loadStatus_ != LoadStatus::Ready || !snapshot_.available || !snapshot_.valid) return false;
    switch (snapshot_.state) {
    case State::PendingNormalBoot:
        return transition(State::WaitingForDetector, Reason::None, 0);
    case State::WaitingForDetector:
    case State::Preparing:
        // These states are entered only after a normal runtime has begun.
        // Seeing either at the next boot proves that RAM-only timeout/admission
        // state was lost; terminate instead of granting a fresh wait forever.
        return transition(State::Failed, Reason::Interrupted, snapshot_.executorOperationId);
    case State::Running:
        return transition(State::Recapturing, Reason::Interrupted, snapshot_.executorOperationId);
    case State::Recapturing:
        return transition(State::Partial, Reason::Interrupted, snapshot_.executorOperationId);
    case State::Succeeded:
    case State::Partial:
    case State::Failed:
        return true;
    case State::None:
        return false;
    }
    return false;
}

bool V1SettingsOperationStore::markPreparing() {
    return transition(State::Preparing, Reason::None, 0);
}

bool V1SettingsOperationStore::markRunning(uint32_t executorOperationId) {
    return transition(State::Running, Reason::None, executorOperationId);
}

bool V1SettingsOperationStore::updateComponents(
    const std::array<ComponentSummary, kComponentCount>& components,
    uint32_t executorOperationId) {
    if (loadStatus_ != LoadStatus::Ready || !snapshot_.available || !snapshot_.valid) return false;
    Snapshot candidate = snapshot_;
    candidate.components = components;
    candidate.executorOperationId = executorOperationId;
    return writeRecord(candidate);
}

bool V1SettingsOperationStore::markRecapturing(Reason reason) {
    return transition(State::Recapturing, reason, snapshot_.executorOperationId);
}

bool V1SettingsOperationStore::finish(State terminalState, Reason reason) {
    if (terminalState != State::Succeeded && terminalState != State::Partial && terminalState != State::Failed) {
        return false;
    }
    return transition(terminalState, reason, snapshot_.executorOperationId);
}

bool V1SettingsOperationStore::acknowledgeReturnToMaintenance() {
    if (loadStatus_ != LoadStatus::Ready || !snapshot_.available || !snapshot_.valid ||
        !isTerminal()) return false;
    if (!snapshot_.returnToMaintenance) return true;
    Snapshot candidate = snapshot_;
    candidate.returnToMaintenance = false;
    return writeRecord(candidate);
}

bool V1SettingsOperationStore::isActive() const {
    return snapshot_.available && snapshot_.valid && !isTerminal() && snapshot_.state != State::None;
}

bool V1SettingsOperationStore::isTerminal() const {
    return snapshot_.state == State::Succeeded || snapshot_.state == State::Partial ||
           snapshot_.state == State::Failed;
}

bool V1SettingsOperationStore::targetMatches(const char* canonicalAddress) const {
    return snapshot_.available && snapshot_.valid && isCanonicalAddress(canonicalAddress) &&
           std::memcmp(snapshot_.targetAddress, canonicalAddress, 18u) == 0;
}

const char* V1SettingsOperationStore::kindName(Kind kind) {
    switch (kind) {
    case Kind::ApplySlot: return "apply_slot";
    case Kind::ApplyProfile: return "apply_profile";
    case Kind::FactoryReset: return "factory_reset";
    case Kind::None: return "none";
    }
    return "none";
}

const char* V1SettingsOperationStore::sourceName(Source source) {
    return source == Source::TripleTap ? "triple_tap" : "maintenance_ui";
}

const char* V1SettingsOperationStore::stateName(State state) {
    switch (state) {
    case State::None: return "none";
    case State::PendingNormalBoot: return "pending_normal_boot";
    case State::WaitingForDetector: return "waiting_for_detector";
    case State::Preparing: return "preparing";
    case State::Running: return "running";
    case State::Recapturing: return "recapturing";
    case State::Succeeded: return "succeeded";
    case State::Partial: return "partial";
    case State::Failed: return "failed";
    }
    return "failed";
}

const char* V1SettingsOperationStore::reasonName(Reason reason) {
    switch (reason) {
    case Reason::None: return "none";
    case Reason::DetectorTimeout: return "detector_timeout";
    case Reason::WrongDetector: return "wrong_detector";
    case Reason::QueueRejected: return "queue_rejected";
    case Reason::DetectorDisconnected: return "detector_disconnected";
    case Reason::ExecutorBusy: return "executor_busy";
    case Reason::NoProfileConfigured: return "no_profile_configured";
    case Reason::ProfileBusy: return "profile_busy";
    case Reason::ProfileLoadFailed: return "profile_load_failed";
    case Reason::InvalidConfiguration: return "invalid_configuration";
    case Reason::UnsupportedConfiguration: return "unsupported_configuration";
    case Reason::ActiveSlotPersistFailed: return "active_slot_persist_failed";
    case Reason::StagingUnavailable: return "staging_unavailable";
    case Reason::ApplyPartial: return "apply_partial";
    case Reason::ApplyFailed: return "apply_failed";
    case Reason::FactoryResetSendFailed: return "factory_reset_send_failed";
    case Reason::FactoryResetScopeUnverified: return "factory_reset_scope_unverified";
    case Reason::FactoryResetDefaultsMismatch: return "factory_reset_defaults_mismatch";
    case Reason::RecaptureStartFailed: return "recapture_start_failed";
    case Reason::RecaptureTimedOut: return "recapture_timed_out";
    case Reason::SnapshotStoreUnavailable: return "snapshot_store_unavailable";
    case Reason::SnapshotPersistFailed: return "snapshot_persist_failed";
    case Reason::Interrupted: return "interrupted";
    case Reason::StorageUnavailable: return "storage_unavailable";
    case Reason::InvalidRecord: return "invalid_record";
    }
    return "invalid_record";
}

const char* V1SettingsOperationStore::componentName(Component component) {
    switch (component) {
    case Component::UserSettings: return "userSettings";
    case Component::Display: return "display";
    case Component::Mode: return "mode";
    case Component::Volume: return "volume";
    case Component::CustomFrequencies: return "customFrequencies";
    case Component::FactoryReset: return "factoryReset";
    case Component::Count: break;
    }
    return "unknown";
}

const char* V1SettingsOperationStore::componentOutcomeName(ComponentOutcome outcome) {
    switch (outcome) {
    case ComponentOutcome::NotRequested: return "not_requested";
    case ComponentOutcome::Pending: return "pending";
    case ComponentOutcome::Unchanged: return "unchanged";
    case ComponentOutcome::Sent: return "sent";
    case ComponentOutcome::Verified: return "verified";
    case ComponentOutcome::Unsupported: return "unsupported";
    case ComponentOutcome::Invalid: return "invalid";
    case ComponentOutcome::Blocked: return "blocked";
    case ComponentOutcome::WriteFailed: return "write_failed";
    case ComponentOutcome::ReadFailed: return "read_failed";
    case ComponentOutcome::Mismatch: return "mismatch";
    case ComponentOutcome::Timeout: return "timeout";
    case ComponentOutcome::Disconnected: return "disconnected";
    case ComponentOutcome::SessionChanged: return "session_changed";
    case ComponentOutcome::LoadFailed: return "load_failed";
    }
    return "invalid";
}

const char* V1SettingsOperationStore::componentReasonName(ComponentReason reason) {
    switch (reason) {
    case ComponentReason::None: return "none";
    case ComponentReason::Disconnected: return "disconnected";
    case ComponentReason::SessionChanged: return "session_changed";
    case ComponentReason::MissingLiveSnapshot: return "missing_live_snapshot";
    case ComponentReason::VersionUnknown: return "version_unknown";
    case ComponentReason::UnsupportedFirmware: return "unsupported_firmware";
    case ComponentReason::ProfileBusy: return "profile_busy";
    case ComponentReason::ProfileLoadFailed: return "profile_load_failed";
    case ComponentReason::InvalidProfileSchema: return "invalid_profile_schema";
    case ComponentReason::InvalidUserSettingValue: return "invalid_user_setting_value";
    case ComponentReason::InvalidPolicy: return "invalid_policy";
    case ComponentReason::InvalidVolumePair: return "invalid_volume_pair";
    case ComponentReason::UnsupportedSavedVolume: return "unsupported_saved_volume";
    case ComponentReason::UnsupportedCustomFrequencies: return "unsupported_custom_frequencies";
    case ComponentReason::UnsupportedBluetoothLed: return "unsupported_bluetooth_led";
    case ComponentReason::CustomFrequencyPreservationRequired: return "custom_frequency_preservation_required";
    case ComponentReason::EuroAdvancedModeInvalid: return "euro_advanced_mode_invalid";
    case ComponentReason::VolumeOwnerBusy: return "volume_owner_busy";
    case ComponentReason::UserBytesBeforeRequired: return "user_bytes_before_required";
    case ComponentReason::UserBytesWriteFailed: return "user_bytes_write_failed";
    case ComponentReason::UserBytesReadFailed: return "user_bytes_read_failed";
    case ComponentReason::UserBytesMismatch: return "user_bytes_mismatch";
    case ComponentReason::UserBytesTimeout: return "user_bytes_timeout";
    case ComponentReason::DisplayWriteFailed: return "display_write_failed";
    case ComponentReason::DisplayMismatch: return "display_mismatch";
    case ComponentReason::DisplayTimeout: return "display_timeout";
    case ComponentReason::ModeWriteFailed: return "mode_write_failed";
    case ComponentReason::ModeMismatch: return "mode_mismatch";
    case ComponentReason::ModeTimeout: return "mode_timeout";
    case ComponentReason::VolumeWriteFailed: return "volume_write_failed";
    case ComponentReason::VolumeReadFailed: return "volume_read_failed";
    case ComponentReason::VolumeMismatch: return "volume_mismatch";
    case ComponentReason::VolumeTimeout: return "volume_timeout";
    case ComponentReason::CustomConfigurationInvalid: return "custom_configuration_invalid";
    case ComponentReason::CustomWriteFailed: return "custom_write_failed";
    case ComponentReason::CustomCommitRejected: return "custom_commit_rejected";
    case ComponentReason::CustomCommitTimeout: return "custom_commit_timeout";
    case ComponentReason::CustomReadFailed: return "custom_read_failed";
    case ComponentReason::CustomReadbackInvalid: return "custom_readback_invalid";
    case ComponentReason::CustomReadbackTimeout: return "custom_readback_timeout";
    case ComponentReason::FactoryDefaultUnverified: return "factory_default_unverified";
    case ComponentReason::FactoryUserDefaultsMismatch: return "factory_user_defaults_mismatch";
    case ComponentReason::FactoryResetWriteFailed: return "factory_reset_write_failed";
    case ComponentReason::ProxyOwnsDetector: return "proxy_owns_detector";
    }
    return "invalid_policy";
}
