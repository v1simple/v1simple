#if defined(V1SIMPLE_HIL_FAULT_CONTROL)

#include "hil_fault_controller.h"

#include <climits>

namespace {
struct AllowlistEntry {
    HilCaseId caseId;
    HilFaultId faultId;
};

constexpr AllowlistEntry kAllowlist[] = {
    {HilCaseId::Bsc02, HilFaultId::WifiApStartFailOnce},
    {HilCaseId::Bsc02, HilFaultId::WifiInternalSramHold},
    {HilCaseId::Bsc04, HilFaultId::V1VerifyPushSuppressOnce},
    {HilCaseId::Bsc05, HilFaultId::V1NotificationDelayOnce},
    {HilCaseId::Bsc06, HilFaultId::ObdTransportOperationBarrierOnce},
    {HilCaseId::Bsc10, HilFaultId::WifiEnableAdmissionFailOnce},
    {HilCaseId::Bsc13, HilFaultId::ObdPhysicalLinkPreownershipBarrierOnce},
    {HilCaseId::Bsc14, HilFaultId::SdMutexHold},
    {HilCaseId::Bsc16, HilFaultId::BatteryAdcInitFailOnce},
};
} // namespace

HilFaultController::HilFaultController(MonotonicClock clock, void* clockContext) noexcept
    : clock_(clock), clockContext_(clockContext) {
    for (auto& word : sessionHashWords_) {
        word.store(0);
    }
}

uint32_t HilFaultController::pack(const uint32_t epoch, const HilFaultState state) noexcept {
    return ((epoch & kEpochMask) << kStateBits) | static_cast<uint32_t>(state);
}

uint32_t HilFaultController::packRaw(const uint32_t epoch, const uint32_t state) noexcept {
    return ((epoch & kEpochMask) << kStateBits) | (state & kStateMask);
}

uint32_t HilFaultController::unpackEpoch(const uint32_t packed) noexcept {
    return packed >> kStateBits;
}

HilFaultState HilFaultController::unpackState(const uint32_t packed) noexcept {
    return static_cast<HilFaultState>(packed & kStateMask);
}

bool HilFaultController::deadlineReached(const uint32_t nowMs, const uint32_t deadlineMs) noexcept {
    return static_cast<int32_t>(nowMs - deadlineMs) >= 0;
}

bool HilFaultController::durationIsBounded(const uint32_t nowMs, const uint32_t deadlineMs,
                                           const uint32_t maximumDurationMs) noexcept {
    const int32_t duration = static_cast<int32_t>(deadlineMs - nowMs);
    return duration > 0 && static_cast<uint32_t>(duration) <= maximumDurationMs;
}

size_t HilFaultController::faultIndex(const HilFaultId faultId) noexcept {
    return static_cast<size_t>(faultId);
}

void HilFaultController::storeSessionHash(const HilSessionTokenHash& hash) noexcept {
    for (size_t wordIndex = 0; wordIndex < sessionHashWords_.size(); ++wordIndex) {
        uint32_t word = 0;
        for (size_t byteIndex = 0; byteIndex < 4; ++byteIndex) {
            word = (word << 8u) | hash.bytes[(wordIndex * 4u) + byteIndex];
        }
        sessionHashWords_[wordIndex].store(word);
    }
}

bool HilFaultController::sessionHashIsZero(const HilSessionTokenHash& hash) noexcept {
    uint8_t combined = 0;
    for (const uint8_t byte : hash.bytes) {
        combined |= byte;
    }
    return combined == 0;
}

bool HilFaultController::sessionHashWasUsed(const HilSessionTokenHash& hash) const noexcept {
    for (size_t index = 0; index < usedSessionHashCount_; ++index) {
        if (usedSessionHashes_[index] == hash) {
            return true;
        }
    }
    return false;
}

uint32_t HilFaultController::completionTime(const uint32_t fallbackNowMs) const noexcept {
    return clock_ == nullptr ? fallbackNowMs : clock_(clockContext_);
}

bool HilFaultController::sessionHashMatches(const HilSessionTokenHash& hash) const noexcept {
    uint32_t difference = 0;
    for (size_t wordIndex = 0; wordIndex < sessionHashWords_.size(); ++wordIndex) {
        uint32_t expected = 0;
        for (size_t byteIndex = 0; byteIndex < 4; ++byteIndex) {
            expected = (expected << 8u) | hash.bytes[(wordIndex * 4u) + byteIndex];
        }
        difference |= sessionHashWords_[wordIndex].load() ^ expected;
    }
    return difference == 0;
}

bool HilFaultController::sessionMatches(const HilCaseId caseId, const HilSessionTokenHash& hash,
                                        uint32_t& epoch) const noexcept {
    const uint32_t before = activeEpoch_.load();
    if (before == 0 || activeCase_.load() != static_cast<uint8_t>(caseId) || !sessionHashMatches(hash)) {
        return false;
    }
    const uint32_t after = activeEpoch_.load();
    if (before != after) {
        return false;
    }
    epoch = after;
    return true;
}

void HilFaultController::resetSlot(Slot& slot, const uint32_t epoch) noexcept {
    slot.armSequence.store(0);
    slot.readySequence.store(0);
    slot.activeGeneration.store(0);
    slot.exactPhase.store(0);
    slot.readyTimestampMs.store(0);
    slot.automaticReleaseDeadlineMs.store(0);
    slot.epochAndState.store(pack(epoch, HilFaultState::Disarmed));
}

HilFaultResult HilFaultController::beginSession(const HilCaseId caseId, const HilSessionTokenHash& sessionHash,
                                                const uint32_t deadlineMs, const uint32_t nowMs) noexcept {
    if (static_cast<uint8_t>(caseId) >= static_cast<uint8_t>(HilCaseId::Invalid)) {
        return HilFaultResult::WrongCase;
    }
    if (sessionHashIsZero(sessionHash)) {
        return HilFaultResult::InvalidSessionHash;
    }
    if (!durationIsBounded(nowMs, deadlineMs, maximumSessionDurationMs(caseId))) {
        return HilFaultResult::InvalidDeadline;
    }
    HilAtomicLease mutation(mutationGate_);
    if (!mutation.acquired()) {
        return HilFaultResult::Busy;
    }
    if (sessionHashWasUsed(sessionHash)) {
        return HilFaultResult::InvalidSessionHash;
    }
    if (usedSessionHashCount_ == usedSessionHashes_.size()) {
        return HilFaultResult::SessionHashRegistryFull;
    }
    usedSessionHashes_[usedSessionHashCount_++] = sessionHash;

    activeEpoch_.store(0);
    uint32_t epoch = (epochCounter_.fetchAdd(1) + 1u) & kEpochMask;
    if (epoch == 0) {
        epoch = (epochCounter_.fetchAdd(1) + 1u) & kEpochMask;
    }
    storeSessionHash(sessionHash);
    activeCase_.store(static_cast<uint32_t>(caseId));
    sessionDeadlineMs_.store(deadlineMs);
    nextReadySequence_.store(1);
    for (auto& slot : slots_) {
        resetSlot(slot, epoch);
    }
    activeEpoch_.store(epoch);
    return HilFaultResult::Ok;
}

HilFaultResult HilFaultController::endSession(const HilCaseId caseId, const HilSessionTokenHash& sessionHash) noexcept {
    HilAtomicLease mutation(mutationGate_);
    if (!mutation.acquired()) {
        return HilFaultResult::Busy;
    }
    uint32_t epoch = 0;
    if (!sessionMatches(caseId, sessionHash, epoch)) {
        return sessionActive() ? HilFaultResult::WrongSession : HilFaultResult::NoSession;
    }
    activeEpoch_.store(0);
    for (auto& slot : slots_) {
        resetSlot(slot, 0);
    }
    HilSessionTokenHash cleared{};
    storeSessionHash(cleared);
    activeCase_.store(static_cast<uint32_t>(HilCaseId::Invalid));
    sessionDeadlineMs_.store(0);
    return HilFaultResult::Ok;
}

HilFaultResult HilFaultController::arm(const HilCaseId caseId, const HilFaultId faultId,
                                       const HilSessionTokenHash& sessionHash, const uint32_t armSequence,
                                       const uint32_t nowMs) noexcept {
    service(nowMs);
    if (!isAllowed(caseId, faultId)) {
        return faultId == HilFaultId::Invalid ? HilFaultResult::WrongFault : HilFaultResult::WrongCase;
    }
    HilAtomicLease mutation(mutationGate_);
    if (!mutation.acquired()) {
        return HilFaultResult::Busy;
    }
    uint32_t epoch = 0;
    if (!sessionMatches(caseId, sessionHash, epoch)) {
        return sessionActive() ? HilFaultResult::WrongSession : HilFaultResult::NoSession;
    }
    if (armSequence == 0 || deadlineReached(nowMs, sessionDeadlineMs_.load())) {
        return HilFaultResult::Expired;
    }
    Slot& slot = slots_[faultIndex(faultId)];
    uint32_t expected = pack(epoch, HilFaultState::Disarmed);
    if (!slot.epochAndState.compareExchange(expected, packRaw(epoch, kUpdatingStateRaw))) {
        return HilFaultResult::WrongState;
    }
    slot.armSequence.store(armSequence);
    expected = packRaw(epoch, kUpdatingStateRaw);
    if (!slot.epochAndState.compareExchange(expected, pack(epoch, HilFaultState::Armed))) {
        return activeEpoch_.load() == epoch ? HilFaultResult::WrongState : HilFaultResult::WrongSession;
    }
    return HilFaultResult::Ok;
}

HilReadyResult HilFaultController::publishReady(const HilCaseId caseId, const HilFaultId faultId,
                                                const HilSessionTokenHash& sessionHash, const uint32_t armSequence,
                                                const uint32_t activeGeneration, const uint16_t exactPhase,
                                                const uint32_t nowMs, const uint32_t automaticReleaseAfterMs) noexcept {
    if (!isAllowed(caseId, faultId)) {
        return {HilFaultResult::WrongCase, 0};
    }
    HilAtomicLease mutation(mutationGate_);
    if (!mutation.acquired()) {
        return {HilFaultResult::Busy, 0};
    }
    uint32_t epoch = 0;
    if (!sessionMatches(caseId, sessionHash, epoch)) {
        return {sessionActive() ? HilFaultResult::WrongSession : HilFaultResult::NoSession, 0};
    }
    if (activeGeneration == 0 || exactPhase == 0 || automaticReleaseAfterMs == 0 ||
        automaticReleaseAfterMs > kMaximumAutomaticReleaseMs ||
        !durationIsBounded(nowMs, nowMs + automaticReleaseAfterMs, kMaximumAutomaticReleaseMs) ||
        deadlineReached(nowMs, sessionDeadlineMs_.load()) ||
        deadlineReached(nowMs + automaticReleaseAfterMs, sessionDeadlineMs_.load())) {
        return {HilFaultResult::InvalidDeadline, 0};
    }
    Slot& slot = slots_[faultIndex(faultId)];
    if (slot.armSequence.load() != armSequence) {
        return {HilFaultResult::WrongArmSequence, 0};
    }
    uint32_t expected = pack(epoch, HilFaultState::Armed);
    if (!slot.epochAndState.compareExchange(expected, packRaw(epoch, kUpdatingStateRaw))) {
        return {HilFaultResult::WrongState, 0};
    }
#if defined(UNIT_TEST)
    if (readyReservationHook_ != nullptr) {
        readyReservationHook_(*this, nowMs, readyReservationContext_);
    }
#endif
    const uint32_t completionNowMs = completionTime(nowMs);
    const uint32_t automaticReleaseDeadlineMs = completionNowMs + automaticReleaseAfterMs;
    if (static_cast<int32_t>(completionNowMs - nowMs) < 0 ||
        deadlineReached(completionNowMs, sessionDeadlineMs_.load()) ||
        !durationIsBounded(completionNowMs, automaticReleaseDeadlineMs, kMaximumAutomaticReleaseMs) ||
        deadlineReached(automaticReleaseDeadlineMs, sessionDeadlineMs_.load())) {
        expected = packRaw(epoch, kUpdatingStateRaw);
        slot.epochAndState.compareExchange(expected, pack(epoch, HilFaultState::Expired));
        return {HilFaultResult::Expired, 0};
    }
    uint32_t readySequence = nextReadySequence_.fetchAdd(1);
    if (readySequence == 0) {
        readySequence = nextReadySequence_.fetchAdd(1);
    }
    slot.readySequence.store(readySequence);
    slot.activeGeneration.store(activeGeneration);
    slot.exactPhase.store(exactPhase);
    slot.readyTimestampMs.store(completionNowMs);
    slot.automaticReleaseDeadlineMs.store(automaticReleaseDeadlineMs);
    expected = packRaw(epoch, kUpdatingStateRaw);
    if (!slot.epochAndState.compareExchange(expected, pack(epoch, HilFaultState::Ready))) {
        return {activeEpoch_.load() == epoch ? HilFaultResult::WrongState : HilFaultResult::WrongSession, 0};
    }
    return {HilFaultResult::Ok, readySequence};
}

HilFaultResult HilFaultController::validateReadyIdentity(const Slot& slot, const uint32_t epoch,
                                                         const uint32_t armSequence, const uint32_t readySequence,
                                                         const uint32_t activeGeneration,
                                                         const uint16_t exactPhase) const noexcept {
    if (unpackEpoch(slot.epochAndState.load()) != epoch) {
        return HilFaultResult::WrongState;
    }
    if (slot.armSequence.load() != armSequence) {
        return HilFaultResult::WrongArmSequence;
    }
    if (slot.readySequence.load() != readySequence) {
        return HilFaultResult::WrongReadySequence;
    }
    if (slot.activeGeneration.load() != activeGeneration) {
        return HilFaultResult::WrongGeneration;
    }
    if (slot.exactPhase.load() != exactPhase) {
        return HilFaultResult::WrongPhase;
    }
    return HilFaultResult::Ok;
}

HilFaultResult HilFaultController::fire(const HilCaseId caseId, const HilFaultId faultId,
                                        const HilSessionTokenHash& sessionHash, const uint32_t armSequence,
                                        const uint32_t readySequence, const uint32_t activeGeneration,
                                        const uint16_t exactPhase, const uint32_t nowMs) noexcept {
    service(nowMs);
    uint32_t epoch = 0;
    if (!isAllowed(caseId, faultId)) {
        return HilFaultResult::WrongCase;
    }
    if (!sessionMatches(caseId, sessionHash, epoch)) {
        return sessionActive() ? HilFaultResult::WrongSession : HilFaultResult::NoSession;
    }
    Slot& slot = slots_[faultIndex(faultId)];
    const HilFaultResult identity =
        validateReadyIdentity(slot, epoch, armSequence, readySequence, activeGeneration, exactPhase);
    if (identity != HilFaultResult::Ok) {
        return identity;
    }
    uint32_t expected = pack(epoch, HilFaultState::Ready);
    if (!slot.epochAndState.compareExchange(expected, pack(epoch, HilFaultState::Fired))) {
        return unpackState(expected) == HilFaultState::Fired ? HilFaultResult::DuplicateFire
                                                             : HilFaultResult::WrongState;
    }
    return HilFaultResult::Ok;
}

HilFaultResult HilFaultController::observeCompetingOperation(const HilCaseId caseId, const HilFaultId faultId,
                                                             const HilSessionTokenHash& sessionHash,
                                                             const uint32_t armSequence, const uint32_t readySequence,
                                                             const uint32_t activeGeneration, const uint16_t exactPhase,
                                                             const uint32_t nowMs) noexcept {
    service(nowMs);
    uint32_t epoch = 0;
    if (!isAllowed(caseId, faultId)) {
        return HilFaultResult::WrongCase;
    }
    if (!sessionMatches(caseId, sessionHash, epoch)) {
        return sessionActive() ? HilFaultResult::WrongSession : HilFaultResult::NoSession;
    }
    Slot& slot = slots_[faultIndex(faultId)];
    const HilFaultResult identity =
        validateReadyIdentity(slot, epoch, armSequence, readySequence, activeGeneration, exactPhase);
    if (identity != HilFaultResult::Ok) {
        return identity;
    }
    uint32_t expected = pack(epoch, HilFaultState::Fired);
    if (!slot.epochAndState.compareExchange(expected, packRaw(epoch, kCompetingOperationObservedStateRaw))) {
        return HilFaultResult::WrongState;
    }
    return HilFaultResult::Ok;
}

HilFaultResult HilFaultController::release(const HilCaseId caseId, const HilFaultId faultId,
                                           const HilSessionTokenHash& sessionHash, const uint32_t armSequence,
                                           const uint32_t readySequence, const uint32_t activeGeneration,
                                           const uint16_t exactPhase, const uint32_t nowMs) noexcept {
    service(nowMs);
    uint32_t epoch = 0;
    if (!isAllowed(caseId, faultId)) {
        return HilFaultResult::WrongCase;
    }
    if (!sessionMatches(caseId, sessionHash, epoch)) {
        return sessionActive() ? HilFaultResult::WrongSession : HilFaultResult::NoSession;
    }
    Slot& slot = slots_[faultIndex(faultId)];
    const HilFaultResult identity =
        validateReadyIdentity(slot, epoch, armSequence, readySequence, activeGeneration, exactPhase);
    if (identity != HilFaultResult::Ok) {
        return identity;
    }
    uint32_t expected = packRaw(epoch, kCompetingOperationObservedStateRaw);
    if (!slot.epochAndState.compareExchange(expected, pack(epoch, HilFaultState::Released))) {
        return expected == pack(epoch, HilFaultState::Fired) ? HilFaultResult::CompetingOperationMissing
                                                             : HilFaultResult::WrongState;
    }
    return HilFaultResult::Ok;
}

void HilFaultController::service(const uint32_t nowMs) noexcept {
    const uint32_t epoch = activeEpoch_.load();
    if (epoch == 0) {
        return;
    }
    const bool sessionExpired = deadlineReached(nowMs, sessionDeadlineMs_.load());
    for (auto& slot : slots_) {
        uint32_t current = slot.epochAndState.load();
        if (unpackEpoch(current) != epoch) {
            continue;
        }
        const uint32_t rawState = current & kStateMask;
        if (rawState == kUpdatingStateRaw) {
            continue;
        }
        const HilFaultState state =
            rawState == kCompetingOperationObservedStateRaw ? HilFaultState::Fired : unpackState(current);
        HilFaultState target = state;
        if (sessionExpired && rawState == kCompetingOperationObservedStateRaw) {
            target = HilFaultState::Released;
        } else if (sessionExpired &&
                   (state == HilFaultState::Armed || state == HilFaultState::Ready || state == HilFaultState::Fired)) {
            target = HilFaultState::Expired;
        } else if ((state == HilFaultState::Ready || state == HilFaultState::Fired) &&
                   deadlineReached(nowMs, slot.automaticReleaseDeadlineMs.load())) {
            target = rawState == kCompetingOperationObservedStateRaw ? HilFaultState::Released : HilFaultState::Expired;
        }
        if (target != state) {
            slot.epochAndState.compareExchange(current, pack(epoch, target));
        }
    }
}

bool HilFaultController::sessionActive() const noexcept {
    return activeEpoch_.load() != 0;
}

HilCaseId HilFaultController::activeCase() const noexcept {
    return sessionActive() ? static_cast<HilCaseId>(activeCase_.load()) : HilCaseId::Invalid;
}

HilFaultSnapshot HilFaultController::snapshot(const HilFaultId faultId) const noexcept {
    HilFaultSnapshot result{};
    const size_t index = faultIndex(faultId);
    if (index >= kFaultCount) {
        return result;
    }
    const Slot& slot = slots_[index];
    const uint32_t current = slot.epochAndState.load();
    const uint32_t activeEpoch = activeEpoch_.load();
    if (unpackEpoch(current) != activeEpoch) {
        return result;
    }
#if defined(UNIT_TEST)
    if (snapshotReadHook_ != nullptr) {
        snapshotReadHook_(snapshotReadContext_);
    }
#endif
    const uint32_t rawState = current & kStateMask;
    if (rawState == kUpdatingStateRaw) {
        result.state = HilFaultState::Armed;
        result.armSequence = slot.armSequence.load();
        return slot.epochAndState.load() == current && activeEpoch_.load() == activeEpoch ? result : HilFaultSnapshot{};
    }
    result.state = rawState == kCompetingOperationObservedStateRaw ? HilFaultState::Fired : unpackState(current);
    result.armSequence = slot.armSequence.load();
    result.readySequence = slot.readySequence.load();
    result.activeGeneration = slot.activeGeneration.load();
    result.exactPhase = static_cast<uint16_t>(slot.exactPhase.load());
    result.readyTimestampMs = slot.readyTimestampMs.load();
    result.automaticReleaseDeadlineMs = slot.automaticReleaseDeadlineMs.load();
    if (slot.epochAndState.load() != current || activeEpoch_.load() != activeEpoch) {
        return HilFaultSnapshot{};
    }
    return result;
}

bool HilFaultController::shouldPause(const HilCaseId caseId, const HilFaultId faultId,
                                     const HilSessionTokenHash& sessionHash, const uint32_t armSequence,
                                     const uint32_t readySequence, const uint32_t activeGeneration,
                                     const uint16_t exactPhase, const uint32_t nowMs) const noexcept {
    if (!isAllowed(caseId, faultId)) {
        return false;
    }
    uint32_t epoch = 0;
    if (!sessionMatches(caseId, sessionHash, epoch)) {
        return false;
    }
    const Slot& slot = slots_[faultIndex(faultId)];
    const uint32_t before = slot.epochAndState.load();
    const uint32_t rawState = before & kStateMask;
    if (unpackEpoch(before) != epoch ||
        (rawState != static_cast<uint32_t>(HilFaultState::Ready) &&
         rawState != static_cast<uint32_t>(HilFaultState::Fired) && rawState != kCompetingOperationObservedStateRaw)) {
        return false;
    }
    if (deadlineReached(nowMs, slot.automaticReleaseDeadlineMs.load()) ||
        deadlineReached(nowMs, sessionDeadlineMs_.load())) {
        return false;
    }
    if (slot.armSequence.load() != armSequence || slot.readySequence.load() != readySequence ||
        slot.activeGeneration.load() != activeGeneration || slot.exactPhase.load() != exactPhase) {
        return false;
    }
    const uint32_t after = slot.epochAndState.load();
    return before == after && activeEpoch_.load() == epoch;
}

bool HilFaultController::isAllowed(const HilCaseId caseId, const HilFaultId faultId) noexcept {
    for (const auto& entry : kAllowlist) {
        if (entry.caseId == caseId && entry.faultId == faultId) {
            return true;
        }
    }
    return false;
}

const char* HilFaultController::caseName(const HilCaseId caseId) noexcept {
    switch (caseId) {
    case HilCaseId::Bsc02:
        return "BSC-02";
    case HilCaseId::Bsc04:
        return "BSC-04";
    case HilCaseId::Bsc05:
        return "BSC-05";
    case HilCaseId::Bsc06:
        return "BSC-06";
    case HilCaseId::Bsc10:
        return "BSC-10";
    case HilCaseId::Bsc13:
        return "BSC-13";
    case HilCaseId::Bsc14:
        return "BSC-14";
    case HilCaseId::Bsc16:
        return "BSC-16";
    case HilCaseId::Invalid:
        return "invalid";
    }
    return "invalid";
}

const char* HilFaultController::faultName(const HilFaultId faultId) noexcept {
    switch (faultId) {
    case HilFaultId::WifiApStartFailOnce:
        return "wifi-ap-start-fail-once";
    case HilFaultId::WifiInternalSramHold:
        return "wifi-internal-sram-hold";
    case HilFaultId::V1VerifyPushSuppressOnce:
        return "v1-verify-push-suppress-once";
    case HilFaultId::V1NotificationDelayOnce:
        return "v1-notification-delay-once";
    case HilFaultId::ObdTransportOperationBarrierOnce:
        return "obd-transport-operation-barrier-once";
    case HilFaultId::WifiEnableAdmissionFailOnce:
        return "wifi-enable-admission-fail-once";
    case HilFaultId::ObdPhysicalLinkPreownershipBarrierOnce:
        return "obd-physical-link-preownership-barrier-once";
    case HilFaultId::SdMutexHold:
        return "sd-mutex-hold";
    case HilFaultId::BatteryAdcInitFailOnce:
        return "battery-adc-init-fail-once";
    case HilFaultId::Invalid:
        return "invalid";
    }
    return "invalid";
}

const char* HilFaultController::stateName(const HilFaultState state) noexcept {
    switch (state) {
    case HilFaultState::Disarmed:
        return "disarmed";
    case HilFaultState::Armed:
        return "armed";
    case HilFaultState::Ready:
        return "ready";
    case HilFaultState::Fired:
        return "fired";
    case HilFaultState::Released:
        return "released";
    case HilFaultState::Expired:
        return "expired";
    }
    return "invalid";
}

#endif // V1SIMPLE_HIL_FAULT_CONTROL
